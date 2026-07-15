// Copyright 2022, Qwacks. All Rights Reserved.

#include "FlockEditor.h"
#include "Guards/FlockPlayModeGuard.h"
#include "Version/FlockVersionResolver.h"
#include "Version/FlockVersionLookup.h"
#include "Version/FlockHttpVersionLookup.h"
#include "Config/FlockConfig.h"
#include "ToolMenus.h"
#include "Textures/SlateIcon.h"
#include "Framework/Commands/UIAction.h"
#include "Editor.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "FlockEditor"

DEFINE_LOG_CATEGORY(LogFlockEditor);

void FFlockEditorModule::StartupModule()
{
	// Register the HTTP-backed version lookup (replacing the default stub). This also flips the build
	// guard live, since the guard is inert until a lookup reports CanResolve() == true.
	FFlockVersionLookupRegistry::Set(MakeShared<FFlockHttpVersionLookup>());

	FFlockPlayModeGuard::Register();

	// Auto-bake the Game Version ID at edit time: on a resolve-input change, and once on editor startup
	// if unresolved. Removes the manual menu click while keeping runtime init network-free.
	ConfigChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(this, &FFlockEditorModule::OnConfigPropertyChanged);
	EditorInitializedHandle = FEditorDelegates::OnEditorInitialized.AddRaw(this, &FFlockEditorModule::OnEditorInitialized);

	// ToolMenus may not be ready at module load; defer menu registration until it is.
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FFlockEditorModule::RegisterMenus));
}

void FFlockEditorModule::ShutdownModule()
{
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(ConfigChangedHandle);
	FEditorDelegates::OnEditorInitialized.Remove(EditorInitializedHandle);

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	FFlockPlayModeGuard::Unregister();

	// Revert to the stub so a later re-init doesn't dangle on this module's lookup.
	FFlockVersionLookupRegistry::Reset();
}

void FFlockEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
	if (!ToolsMenu)
	{
		return;
	}

	FToolMenuSection& Section = ToolsMenu->FindOrAddSection("Flock", LOCTEXT("FlockSection", "Flock"));
	Section.AddMenuEntry(
		"FlockResolveGameVersion",
		LOCTEXT("ResolveGameVersion", "Resolve Game Version"),
		LOCTEXT("ResolveGameVersionTooltip", "Resolve the Game Version name to its ID and bake it into project settings, so runtime init needs no network."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&FFlockEditorModule::OnResolveGameVersion)));
}

void FFlockEditorModule::OnResolveGameVersion()
{
	// Manual menu action: always resolve, so an incomplete config surfaces a clear failure toast.
	UFlockConfig* Config = GetMutableDefault<UFlockConfig>();
	FFlockVersionResolver::ResolveAndBake(*Config);
}

void FFlockEditorModule::OnConfigPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent)
{
	if (!Object || !Object->IsA<UFlockConfig>())
	{
		return;
	}
	// Ignore mid-drag interactive edits; act on the committed value.
	if ((PropertyChangedEvent.ChangeType & EPropertyChangeType::Interactive) != 0)
	{
		return;
	}
	// React only to the inputs that determine the resolved ID. GameVersionId itself is written by the
	// bake and is not in this set, so auto-resolve can never re-trigger itself.
	const FName Changed = PropertyChangedEvent.GetMemberPropertyName();
	if (Changed != GET_MEMBER_NAME_CHECKED(UFlockConfig, GameVersion) &&
		Changed != GET_MEMBER_NAME_CHECKED(UFlockConfig, ApiUrl) &&
		Changed != GET_MEMBER_NAME_CHECKED(UFlockConfig, ApiKey))
	{
		return;
	}
	TryAutoResolve(TEXT("settings changed"));
}

void FFlockEditorModule::OnEditorInitialized(double /*Duration*/)
{
	// Catch-up for a project that has a version name but no baked ID yet (e.g. a fresh clone).
	const UFlockConfig* Config = GetDefault<UFlockConfig>();
	if (Config && Config->GameVersionId.IsEmpty())
	{
		TryAutoResolve(TEXT("unresolved on editor startup"));
	}
}

void FFlockEditorModule::TryAutoResolve(const TCHAR* Reason)
{
	// Inert until a real (non-stub) lookup is registered.
	if (!FFlockVersionLookupRegistry::Get().CanResolve())
	{
		return;
	}
	// Only auto-resolve a complete config; an incomplete one would just fail noisily. The manual menu
	// action stays available for that case (it surfaces the failure on purpose).
	UFlockConfig* Config = GetMutableDefault<UFlockConfig>();
	FString Error;
	if (!Config || !Config->IsValid(Error))
	{
		return;
	}
	UE_LOG(LogFlockEditor, Log, TEXT("Flock: auto-resolving Game Version (%s)."), Reason);
	FFlockVersionResolver::ResolveAndBake(*Config);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFlockEditorModule, FlockEditor)
