// Copyright 2022, Qwacks. All Rights Reserved.

#include "FlockEvents.h"

void UFlockEvents::CallOrRegister_OnInitialized(FFlockInitializedCallback Callback)
{
	if (bSdkInitialized)
	{
		Callback.ExecuteIfBound();
		return;
	}
	PendingInitialized.Add(MoveTemp(Callback));
}

void UFlockEvents::CallOrRegister_OnInitializationFailed(FFlockInitializationFailedCallback Callback)
{
	if (!LastInitializationError.IsEmpty())
	{
		Callback.ExecuteIfBound(LastInitializationError);
		return;
	}
	PendingInitializationFailed.Add(MoveTemp(Callback));
}

void UFlockEvents::InvokeInitialized()
{
	bSdkInitialized = true;
	LastInitializationError.Reset();

	LogRaise(TEXT("OnInitialized"), OnInitialized.GetAllObjects().Num() + PendingInitialized.Num());
	OnInitialized.Broadcast();

	// One-shot late binders: fire once, then drop. Moved out first so a callback that registers a new
	// callback lands in the fresh pending list (for the next init) instead of this loop.
	TArray<FFlockInitializedCallback> Pending = MoveTemp(PendingInitialized);
	for (FFlockInitializedCallback& Callback : Pending)
	{
		Callback.ExecuteIfBound();
	}
}

void UFlockEvents::InvokeInitializationFailed(const FString& Error)
{
	LastInitializationError = Error;

	LogRaise(TEXT("OnInitializationFailed"), OnInitializationFailed.GetAllObjects().Num() + PendingInitializationFailed.Num());
	OnInitializationFailed.Broadcast(Error);

	TArray<FFlockInitializationFailedCallback> Pending = MoveTemp(PendingInitializationFailed);
	for (FFlockInitializationFailedCallback& Callback : Pending)
	{
		Callback.ExecuteIfBound(Error);
	}
}

void UFlockEvents::InvokeShutdown()
{
	bSdkInitialized = false;

	LogRaise(TEXT("OnShutdown"), OnShutdown.GetAllObjects().Num());
	OnShutdown.Broadcast();
}

void UFlockEvents::InvokeAuthenticated(const FFlockAuthInfo& Info)
{
	LogRaise(TEXT("OnAuthenticated"), OnAuthenticated.GetAllObjects().Num());
	OnAuthenticated.Broadcast(Info);
}

void UFlockEvents::InvokeTokenRefreshed()
{
	LogRaise(TEXT("OnTokenRefreshed"), OnTokenRefreshed.GetAllObjects().Num());
	OnTokenRefreshed.Broadcast();
}

void UFlockEvents::InvokeAuthExpired()
{
	LogRaise(TEXT("OnAuthExpired"), OnAuthExpired.GetAllObjects().Num());
	OnAuthExpired.Broadcast();
}

void UFlockEvents::InvokeLoggedOut()
{
	LogRaise(TEXT("OnLoggedOut"), OnLoggedOut.GetAllObjects().Num());
	OnLoggedOut.Broadcast();
}

void UFlockEvents::InvokeSessionRestored(bool bRestored)
{
	LogRaise(TEXT("OnSessionRestored"), OnSessionRestored.GetAllObjects().Num());
	OnSessionRestored.Broadcast(bRestored);
}

void UFlockEvents::InvokeSessionStarted(const FString& SessionId)
{
	LogRaise(TEXT("OnSessionStarted"), OnSessionStarted.GetAllObjects().Num());
	OnSessionStarted.Broadcast(SessionId);
}

void UFlockEvents::InvokeSessionEnded(const FFlockSessionEndedArgs& Args)
{
	LogRaise(TEXT("OnSessionEnded"), OnSessionEnded.GetAllObjects().Num());
	OnSessionEnded.Broadcast(Args);
}

void UFlockEvents::InvokeSessionPaused()
{
	LogRaise(TEXT("OnSessionPaused"), OnSessionPaused.GetAllObjects().Num());
	OnSessionPaused.Broadcast();
}

void UFlockEvents::InvokeSessionResumed()
{
	LogRaise(TEXT("OnSessionResumed"), OnSessionResumed.GetAllObjects().Num());
	OnSessionResumed.Broadcast();
}

void UFlockEvents::InvokeConsentChanged(bool bGranted)
{
	LogRaise(TEXT("OnConsentChanged"), OnConsentChanged.GetAllObjects().Num());
	OnConsentChanged.Broadcast(bGranted);
}

void UFlockEvents::LogRaise(const TCHAR* EventName, int32 SubscriberCount) const
{
	if (Logger.IsValid())
	{
		Logger->LogDebug(FString::Printf(TEXT("%s fired -> %d subscriber(s)"), EventName, SubscriberCount));
	}
}
