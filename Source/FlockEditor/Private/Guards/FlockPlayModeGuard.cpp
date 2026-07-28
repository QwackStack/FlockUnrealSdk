// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Guards/FlockPlayModeGuard.h"
#include "Config/FlockConfig.h"
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

void FFlockPlayModeGuard::OnBeginPIE(const bool bIsSimulating)
{
	// Never surface dialogs/toasts in headless/CI.
	if (FApp::IsUnattended() || IsRunningCommandlet())
	{
		return;
	}

	const UFlockConfig* Config = GetDefault<UFlockConfig>();
	if (!Config->bPlayModeGuardEnabled)
	{
		return;
	}

	// The guard only matters when the SDK would try to auto-initialize. If auto-init is off, the game
	// initializes the SDK itself and we don't second-guess that.
	if (!Config->bAutoInitializeOnLoad)
	{
		return;
	}

	FString Reason;
	FString ValidationError;
	if (!Config->IsValid(ValidationError))
	{
		Reason = ValidationError;
	}
	else if (Config->GameVersionId.IsEmpty())
	{
		Reason = TEXT("Game Version ID is not resolved.");
	}
	else
	{
		return; // fully set up
	}

	const FText Message = FText::Format(
		LOCTEXT("PlayModeGuardWarn",
			"Flock isn't fully set up, so the SDK won't initialize this session: {0}\nOpen Project Settings > Flock SDK (or Tools > Flock > Resolve Game Version) to fix it."),
		FText::FromString(Reason));

	FMessageLog("PIE").Warning(Message);

	FNotificationInfo Info(Message);
	Info.ExpireDuration = 7.0f;
	FSlateNotificationManager::Get().AddNotification(Info);

	UE_LOG(LogFlockEditor, Warning, TEXT("Flock play-mode guard: %s"), *Reason);
}

#undef LOCTEXT_NAMESPACE
