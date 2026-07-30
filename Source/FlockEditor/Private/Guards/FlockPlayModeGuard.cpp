// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Guards/FlockPlayModeGuard.h"
#include "Config/FlockConfig.h"
#include "Setup/FlockSetupContext.h"
#include "Setup/FlockSetupUI.h"
#include "FlockEditor.h"
#include "Editor.h"
#include "Misc/App.h"
#include "Logging/MessageLog.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "FlockEditor"

FDelegateHandle FFlockPlayModeGuard::BeginPIEHandle;

void FFlockPlayModeGuard::Register()
{
	if (!BeginPIEHandle.IsValid())
	{
		BeginPIEHandle = FEditorDelegates::PreBeginPIE.AddStatic(&FFlockPlayModeGuard::OnBeginPIE);
	}
}

void FFlockPlayModeGuard::Unregister()
{
	if (BeginPIEHandle.IsValid())
	{
		FEditorDelegates::PreBeginPIE.Remove(BeginPIEHandle);
		BeginPIEHandle.Reset();
	}
}

TArray<FFlockSetupFinding> FFlockPlayModeGuard::BlockingFindings(const TArray<FFlockSetupFinding>& Findings,
	bool bGuardEnabled, bool bAutoInit)
{
	if (!bGuardEnabled)
	{
		return {};
	}

	// The guard only matters when the SDK would try to auto-initialize. If auto-init is off, the game
	// initializes the SDK itself and we don't second-guess that.
	if (!bAutoInit)
	{
		return {};
	}

	// Errors only: a warning still runs, and interrupting Play for one would be exactly the nagging this
	// design exists to avoid.
	return FFlockSetupStatus::AtLeast(Findings, EFlockSetupSeverity::Error);
}

void FFlockPlayModeGuard::OnBeginPIE(const bool bIsSimulating)
{
	// Never surface dialogs/toasts in headless/CI.
	if (FApp::IsUnattended() || IsRunningCommandlet())
	{
		return;
	}

	const UFlockConfig* Config = GetDefault<UFlockConfig>();
	if (!Config)
	{
		return;
	}

	const TArray<FFlockSetupFinding> Blocking = BlockingFindings(
		FFlockSetupContext::Evaluate(), Config->bPlayModeGuardEnabled, Config->bAutoInitializeOnLoad);

	if (Blocking.Num() == 0)
	{
		return;
	}

	TArray<FString> Reasons;
	Reasons.Reserve(Blocking.Num());
	for (const FFlockSetupFinding& Finding : Blocking)
	{
		Reasons.Add(Finding.Title.ToString());
	}
	const FString Joined = FString::Join(Reasons, TEXT("; "));

	const FText Message = FText::Format(
		LOCTEXT("PlayModeGuardWarn", "Flock isn't fully set up, so the SDK won't initialize this session: {0}"),
		FText::FromString(Joined));

	FMessageLog("PIE").Warning(Message);

	FNotificationInfo Info(Message);
	Info.ExpireDuration = 7.0f;

	// One click to the place that can fix it. The old toast named a menu path and left the developer to
	// find it.
	Info.HyperlinkText = LOCTEXT("OpenFlockHyperlink", "Open Flock");
	Info.Hyperlink = FSimpleDelegate::CreateStatic(&FFlockSetupUI::OpenPanel);

	FSlateNotificationManager::Get().AddNotification(Info);

	UE_LOG(LogFlockEditor, Warning, TEXT("Flock play-mode guard: %s"), *Joined);
}

#undef LOCTEXT_NAMESPACE
