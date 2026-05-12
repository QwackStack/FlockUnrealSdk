#include "Qwack_ue_SdkEditor.h"

#include "Framework/Notifications/NotificationManager.h"
#include "ISettingsModule.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Qwack_ue_Sdk/Config/QwackSettings.h"
#include "QwackSettingsDetails.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "FQwack_ue_SdkEditorModule"

void FQwack_ue_SdkEditorModule::StartupModule()
{
	// Register the custom details panel for UQwackSettings.
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropertyModule.RegisterCustomClassLayout(
		UQwackSettings::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FQwackSettingsDetails::MakeInstance));
	PropertyModule.NotifyCustomizationModuleChanged();

	// Defer the first-run check until the editor is fully up.
	FCoreDelegates::OnFEngineLoopInitComplete.AddRaw(this, &FQwack_ue_SdkEditorModule::CheckFirstRun);
}

void FQwack_ue_SdkEditorModule::ShutdownModule()
{
	FCoreDelegates::OnFEngineLoopInitComplete.RemoveAll(this);

	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.UnregisterCustomClassLayout(UQwackSettings::StaticClass()->GetFName());
	}

	if (TSharedPtr<SNotificationItem> Note = ActiveNotification.Pin())
	{
		Note->ExpireAndFadeout();
	}
}

void FQwack_ue_SdkEditorModule::CheckFirstRun()
{
	const UQwackSettings* S = GetDefault<UQwackSettings>();
	if (S && !S->ApiKey.IsEmpty())
	{
		return; // already configured
	}

	FNotificationInfo Info(LOCTEXT("FlockUnconfigured", "Flock SDK is not configured"));
	Info.SubText = LOCTEXT("FlockUnconfiguredSub", "Set your API Key in Project Settings → Plugins → Flock.");
	Info.bFireAndForget = false;
	Info.bUseSuccessFailIcons = false;
	Info.bUseThrobber = false;
	Info.ExpireDuration = 0.f;
	Info.FadeOutDuration = 0.3f;

	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("OpenSettings", "Open Settings"),
		LOCTEXT("OpenSettingsTip", "Jump to the Flock settings page"),
		FSimpleDelegate::CreateRaw(this, &FQwack_ue_SdkEditorModule::OpenFlockSettings)));

	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("Dismiss", "Dismiss"),
		FText::GetEmpty(),
		FSimpleDelegate::CreateRaw(this, &FQwack_ue_SdkEditorModule::DismissNotification)));

	const TSharedPtr<SNotificationItem> Note = FSlateNotificationManager::Get().AddNotification(Info);
	if (Note.IsValid())
	{
		Note->SetCompletionState(SNotificationItem::CS_Pending);
		ActiveNotification = Note;
	}
}

void FQwack_ue_SdkEditorModule::OpenFlockSettings()
{
	DismissNotification();
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->ShowViewer(TEXT("Project"), TEXT("Plugins"), TEXT("Flock"));
	}
}

void FQwack_ue_SdkEditorModule::DismissNotification()
{
	if (TSharedPtr<SNotificationItem> Note = ActiveNotification.Pin())
	{
		Note->SetCompletionState(SNotificationItem::CS_None);
		Note->ExpireAndFadeout();
		ActiveNotification.Reset();
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FQwack_ue_SdkEditorModule, Qwack_ue_SdkEditor)
