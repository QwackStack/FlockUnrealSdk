// Copyright 2022, Qwacks. All Rights Reserved.

#include "FlockEditor.h"
#include "Guards/FlockPlayModeGuard.h"
#include "Version/FlockVersionResolver.h"
#include "Config/FlockConfig.h"
#include "ToolMenus.h"
#include "Textures/SlateIcon.h"
#include "Framework/Commands/UIAction.h"

#define LOCTEXT_NAMESPACE "FlockEditor"

DEFINE_LOG_CATEGORY(LogFlockEditor);

void FFlockEditorModule::StartupModule()
{
	FFlockPlayModeGuard::Register();

	// ToolMenus may not be ready at module load; defer menu registration until it is.
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FFlockEditorModule::RegisterMenus));
}

void FFlockEditorModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	FFlockPlayModeGuard::Unregister();
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
	UFlockConfig* Config = GetMutableDefault<UFlockConfig>();
	FFlockVersionResolver::ResolveAndBake(*Config);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFlockEditorModule, FlockEditor)
