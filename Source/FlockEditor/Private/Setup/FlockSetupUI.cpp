// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Setup/FlockSetupUI.h"
#include "Setup/SFlockPanel.h"
#include "Setup/FlockSetupContext.h"
#include "Setup/FlockSummonPolicy.h"
#include "Setup/FlockEditorUserSettings.h"
#include "Setup/FlockConfigDetails.h"
#include "Setup/FlockEditorStyle.h"
#include "Config/FlockConfig.h"
#include "FlockSubsystem.h"
#include "FlockEditor.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "PropertyEditorModule.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "FlockEditor"

const FName FFlockSetupUI::TabName(TEXT("FlockSetup"));
TWeakPtr<SFlockPanel> FFlockSetupUI::LivePanel;

void FFlockSetupUI::Register()
{
	// Before the tab spawner: the icon brush has to be registered by the time the tab claims it.
	FFlockEditorStyle::Register();

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateStatic(&FFlockSetupUI::SpawnTab))
		.SetDisplayName(LOCTEXT("FlockTabTitle", "Flock"))
		.SetTooltipText(LOCTEXT("FlockTabTooltip", "Flock SDK setup status, credentials, and live state during Play In Editor."))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
		.SetIcon(FSlateIcon(FFlockEditorStyle::GetStyleSetName(), "Flock.TabIcon"));

	// The banner on Project Settings > Flock SDK. Same findings as the tab — that shared model is the
	// whole reason both surfaces were worth building.
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropertyModule.RegisterCustomClassLayout(
		UFlockConfig::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FFlockConfigDetails::MakeInstance));
	PropertyModule.NotifyCustomizationModuleChanged();
}

void FFlockSetupUI::Unregister()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
	}

	if (FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
	{
		PropertyModule->UnregisterCustomClassLayout(UFlockConfig::StaticClass()->GetFName());
		PropertyModule->NotifyCustomizationModuleChanged();
	}

	LivePanel.Reset();

	// After the tab spawner is gone, so nothing can be asked to draw a brush that no longer exists.
	FFlockEditorStyle::Unregister();
}

TSharedRef<SDockTab> FFlockSetupUI::SpawnTab(const FSpawnTabArgs& Args)
{
	TSharedRef<SFlockPanel> Panel = SNew(SFlockPanel);
	LivePanel = Panel;

	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			Panel
		];
}

void FFlockSetupUI::OpenPanel()
{
	FGlobalTabmanager::Get()->TryInvokeTab(TabName);
	RefreshPanel();
}

void FFlockSetupUI::RefreshPanel()
{
	if (const TSharedPtr<SFlockPanel> Panel = LivePanel.Pin())
	{
		Panel->Refresh();
	}
}

void FFlockSetupUI::SummonIfNeeded()
{
	const TArray<FFlockSetupFinding> Findings = FFlockSetupContext::Evaluate();
	const FFlockSummonContext Context = FFlockSetupContext::BuildSummonContext(Findings);

	// Both halves come from the policy. This function used to decide for itself when to record the
	// seen-state and got it wrong for headless runs, silently consuming the developer's one-shot notice.
	const FFlockSummonDecision Decision = FFlockSummonPolicy::Decide(Context);

	if (Decision.bOpenPanel)
	{
		UE_LOG(LogFlockEditor, Log, TEXT("Flock: opening the setup panel (%s)."),
			Context.bFirstAdd ? TEXT("first run") : TEXT("setup needs attention"));

		OpenPanel();
		FFlockSetupContext::MarkSummoned();
	}

	if (Decision.bRecordSeen)
	{
		if (UFlockEditorUserSettings* Settings = UFlockEditorUserSettings::Get())
		{
			Settings->MarkSeen(UFlockSubsystem::SdkVersion);
		}
	}
}

#undef LOCTEXT_NAMESPACE
