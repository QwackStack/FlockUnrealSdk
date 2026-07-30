// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Setup/FlockEditorLiveProbe.h"
#include "FlockEvents.h"

void UFlockEditorLiveProbe::Bind(UFlockEvents* Events)
{
	Unbind();
	if (!Events)
	{
		return;
	}

	Bound = Events;

	Events->OnInitialized.AddDynamic(this, &UFlockEditorLiveProbe::HandleInitialized);
	Events->OnInitializationFailed.AddDynamic(this, &UFlockEditorLiveProbe::HandleInitializationFailed);
	Events->OnShutdown.AddDynamic(this, &UFlockEditorLiveProbe::HandleShutdown);
	Events->OnAuthenticated.AddDynamic(this, &UFlockEditorLiveProbe::HandleAuthenticated);
	Events->OnTokenRefreshed.AddDynamic(this, &UFlockEditorLiveProbe::HandleTokenRefreshed);
	Events->OnAuthExpired.AddDynamic(this, &UFlockEditorLiveProbe::HandleAuthExpired);
	Events->OnLoggedOut.AddDynamic(this, &UFlockEditorLiveProbe::HandleLoggedOut);
	Events->OnSessionRestored.AddDynamic(this, &UFlockEditorLiveProbe::HandleSessionRestored);
	Events->OnSessionStarted.AddDynamic(this, &UFlockEditorLiveProbe::HandleSessionStarted);
	Events->OnSessionEnded.AddDynamic(this, &UFlockEditorLiveProbe::HandleSessionEnded);
	Events->OnSessionPaused.AddDynamic(this, &UFlockEditorLiveProbe::HandleSessionPaused);
	Events->OnSessionResumed.AddDynamic(this, &UFlockEditorLiveProbe::HandleSessionResumed);
	Events->OnConsentChanged.AddDynamic(this, &UFlockEditorLiveProbe::HandleConsentChanged);
}

void UFlockEditorLiveProbe::Unbind()
{
	UFlockEvents* Events = Bound.Get();
	Bound.Reset();
	if (!Events)
	{
		return;
	}

	Events->OnInitialized.RemoveDynamic(this, &UFlockEditorLiveProbe::HandleInitialized);
	Events->OnInitializationFailed.RemoveDynamic(this, &UFlockEditorLiveProbe::HandleInitializationFailed);
	Events->OnShutdown.RemoveDynamic(this, &UFlockEditorLiveProbe::HandleShutdown);
	Events->OnAuthenticated.RemoveDynamic(this, &UFlockEditorLiveProbe::HandleAuthenticated);
	Events->OnTokenRefreshed.RemoveDynamic(this, &UFlockEditorLiveProbe::HandleTokenRefreshed);
	Events->OnAuthExpired.RemoveDynamic(this, &UFlockEditorLiveProbe::HandleAuthExpired);
	Events->OnLoggedOut.RemoveDynamic(this, &UFlockEditorLiveProbe::HandleLoggedOut);
	Events->OnSessionRestored.RemoveDynamic(this, &UFlockEditorLiveProbe::HandleSessionRestored);
	Events->OnSessionStarted.RemoveDynamic(this, &UFlockEditorLiveProbe::HandleSessionStarted);
	Events->OnSessionEnded.RemoveDynamic(this, &UFlockEditorLiveProbe::HandleSessionEnded);
	Events->OnSessionPaused.RemoveDynamic(this, &UFlockEditorLiveProbe::HandleSessionPaused);
	Events->OnSessionResumed.RemoveDynamic(this, &UFlockEditorLiveProbe::HandleSessionResumed);
	Events->OnConsentChanged.RemoveDynamic(this, &UFlockEditorLiveProbe::HandleConsentChanged);
}

void UFlockEditorLiveProbe::Emit(const FString& Line)
{
	OnLiveEvent.Broadcast(Line);
}

void UFlockEditorLiveProbe::HandleInitialized()
{
	Emit(TEXT("SDK initialized"));
}

void UFlockEditorLiveProbe::HandleInitializationFailed(const FString& Error)
{
	Emit(FString::Printf(TEXT("Initialization failed: %s"), *Error));
}

void UFlockEditorLiveProbe::HandleShutdown()
{
	Emit(TEXT("SDK shut down"));
}

void UFlockEditorLiveProbe::HandleAuthenticated(const FFlockAuthInfo& Info)
{
	Emit(FString::Printf(TEXT("Signed in (player %s)"), *Info.PlayerId));
}

void UFlockEditorLiveProbe::HandleTokenRefreshed()
{
	Emit(TEXT("Access token refreshed"));
}

void UFlockEditorLiveProbe::HandleAuthExpired()
{
	Emit(TEXT("Session expired"));
}

void UFlockEditorLiveProbe::HandleLoggedOut()
{
	Emit(TEXT("Signed out"));
}

void UFlockEditorLiveProbe::HandleSessionRestored(bool bRestored)
{
	Emit(bRestored ? TEXT("Previous session restored") : TEXT("No session to restore"));
}

void UFlockEditorLiveProbe::HandleSessionStarted(const FString& SessionId)
{
	Emit(FString::Printf(TEXT("Analytics session started (%s)"), *SessionId));
}

void UFlockEditorLiveProbe::HandleSessionEnded(const FFlockSessionEndedArgs& Args)
{
	// The id lives on the snapshot, not the args. Reason is the more useful half anyway — "Quit" versus
	// "Timeout" is what a developer is actually trying to tell apart.
	const UEnum* ReasonEnum = StaticEnum<EFlockSessionEndReason>();
	const FString Reason = ReasonEnum
		? ReasonEnum->GetNameStringByValue(static_cast<int64>(Args.Reason))
		: FString(TEXT("?"));

	Emit(FString::Printf(TEXT("Analytics session ended — %s (%s)"), *Reason, *Args.Snapshot.SessionId));
}

void UFlockEditorLiveProbe::HandleSessionPaused()
{
	Emit(TEXT("Analytics session paused"));
}

void UFlockEditorLiveProbe::HandleSessionResumed()
{
	Emit(TEXT("Analytics session resumed"));
}

void UFlockEditorLiveProbe::HandleConsentChanged(bool bGranted)
{
	Emit(bGranted ? TEXT("Analytics consent granted") : TEXT("Analytics consent revoked"));
}
