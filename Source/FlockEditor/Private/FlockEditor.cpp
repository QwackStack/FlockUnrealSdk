// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockEditor.h"
#include "Codegen/FlockCodegenRunner.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Guards/FlockPlayModeGuard.h"
#include "Setup/FlockSetupUI.h"
#include "Setup/FlockSetupContext.h"
#include "Setup/FlockEditorStyle.h"
#include "Styling/AppStyle.h"
#include "Widgets/Notifications/SNotificationList.h"
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
	FFlockSetupUI::Register();

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
	FFlockSetupUI::Unregister();
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

	// The brand mark goes on the entry that opens the tab, and only that one — it is the same action, so
	// it reads as the same thing in both places. The three actions below take icons that say what they
	// *do*: stamping one identical logo on all four would make the icons pure decoration, which is the
	// opposite of why a menu has them.
	Section.AddMenuEntry(
		"FlockOpenPanel",
		LOCTEXT("OpenFlockPanelMenu", "Flock Panel"),
		LOCTEXT("OpenFlockPanelMenuTooltip",
			"Setup status, credentials, and — during Play In Editor — the SDK's live state."),
		FSlateIcon(FFlockEditorStyle::GetStyleSetName(), "Flock.TabIcon"),
		FUIAction(FExecuteAction::CreateStatic(&FFlockSetupUI::OpenPanel)));

	Section.AddMenuEntry(
		"FlockResolveGameVersion",
		LOCTEXT("ResolveGameVersion", "Resolve Game Version"),
		LOCTEXT("ResolveGameVersionTooltip", "Resolve the Game Version name to its ID and bake it into project settings, so runtime init needs no network."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh"),
		FUIAction(FExecuteAction::CreateStatic(&FFlockEditorModule::OnResolveGameVersion)));

	Section.AddMenuEntry(
		"FlockSyncSchemas",
		LOCTEXT("SyncSchemas", "Sync Schemas"),
		LOCTEXT("SyncSchemasTooltip",
			"Fetch this game version's player templates, game configs, and shops, and regenerate the typed "
			"Blueprint structs, enums, and function library from them."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Import"),
		FUIAction(FExecuteAction::CreateStatic(&FFlockEditorModule::OnSyncSchemas)));

	Section.AddMenuEntry(
		"FlockCleanGenerated",
		LOCTEXT("CleanGenerated", "Clean Generated"),
		LOCTEXT("CleanGeneratedTooltip",
			"Delete every asset a schema sync generated, and the manifest recording it. Blueprints "
			"referencing a generated struct or enum will be listed before anything is removed."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Delete"),
		FUIAction(FExecuteAction::CreateStatic(&FFlockEditorModule::OnCleanGenerated)));
}

namespace
{
	/**
	 * The same sync as the menu entry, reachable without a UI. Two reasons it exists: the menu path cannot
	 * be driven headlessly, so this is how a sync is verified against a real backend; and it is the entry
	 * point the CI commandlet wraps.
	 */
	FAutoConsoleCommand GFlockSyncSchemasCommand(
		TEXT("Flock.SyncSchemas"),
		TEXT("Fetches this game version's schemas and regenerates the typed Blueprint assets from them."),
		FConsoleCommandDelegate::CreateStatic(&FFlockEditorModule::OnSyncSchemas));

	FAutoConsoleCommand GFlockCleanGeneratedCommand(
		TEXT("Flock.CleanGenerated"),
		TEXT("Deletes every generated Flock asset and the codegen manifest."),
		FConsoleCommandDelegate::CreateStatic(&FFlockEditorModule::OnCleanGenerated));
}

void FFlockEditorModule::OnCleanGenerated()
{
	// Confirmation on: this deletes assets a project's Blueprints may reference, and the engine's own
	// prompt is what lists those referencers.
	const FFlockCodegenRunner::FCleanResult Result = FFlockCodegenRunner::Clean(/*bShowConfirmation*/ true);
	const FString Summary = Result.Describe();
	UE_LOG(LogFlockEditor, Log, TEXT("%s"), *Summary);

	FNotificationInfo Info(FText::FromString(Summary));
	Info.ExpireDuration = Result.bSucceeded ? 6.f : 10.f;
	if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Item->SetCompletionState(Result.bSucceeded ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		Item->ExpireAndFadeout();
	}
}

void FFlockEditorModule::OnSyncSchemas()
{
	FFlockCodegenRunner::Sync(FFlockCodegenRunner::FOnSyncComplete::CreateLambda(
		[](const FFlockCodegenRunner::FRunResult& Result)
		{
			const FString Summary = Result.Describe();
			if (Result.bSucceeded)
			{
				UE_LOG(LogFlockEditor, Log, TEXT("%s"), *Summary);
			}
			else
			{
				UE_LOG(LogFlockEditor, Error, TEXT("%s"), *Summary);
			}

			// Warnings are already in the log in full; the toast carries the headline so a sync that half
			// worked cannot look like one that fully worked.
			FNotificationInfo Info(FText::FromString(Summary));
			Info.ExpireDuration = Result.bSucceeded ? 6.f : 10.f;
			if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
			{
				Item->SetCompletionState(Result.bSucceeded ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
				Item->ExpireAndFadeout();
			}
		}));
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
	// Any settings edit can change what the panel should say, so it refreshes before the narrower
	// auto-resolve check below decides whether to touch the network.
	FFlockSetupUI::RefreshPanel();

	const FName Changed = PropertyChangedEvent.GetMemberPropertyName();
	if (Changed != GET_MEMBER_NAME_CHECKED(UFlockConfig, GameVersion) &&
		Changed != GET_MEMBER_NAME_CHECKED(UFlockConfig, ApiUrl) &&
		Changed != GET_MEMBER_NAME_CHECKED(UFlockConfig, ApiKey))
	{
		return;
	}

	// A credential edit invalidates the last probe: reporting "key accepted" about a key that has since
	// changed would be a lie.
	FFlockSetupContext::ClearProbe();

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

	// Cheap drift check: compares the generated manifest against the baked version id, no network. Silent
	// when codegen has never run, so a project that does not use it hears nothing.
	FFlockCodegenRunner::WarnIfBakedVersionDrifted();

	// Opens the panel only if something is actionable, or this developer has never seen the SDK. A
	// healthy project on a fresh clone gets nothing.
	FFlockSetupUI::SummonIfNeeded();
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
