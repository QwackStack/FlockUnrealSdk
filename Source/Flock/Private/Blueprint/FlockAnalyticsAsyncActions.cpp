// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockAnalyticsAsyncActions.h"
#include "Blueprint/FlockBlueprintCallOrigin.h"
#include "Http/FlockErrorHints.h"

#include "FlockEventModels.h"
#include "FlockSubsystem.h"
#include "Providers/FlockAnalyticsProvider.h"

namespace
{
	FFlockAnalyticsProvider* ResolveProvider(UObject* WorldContextObject, FFlockError& OutError)
	{
		UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
		FFlockAnalyticsProvider* Provider = Sdk ? Sdk->GetAnalyticsProvider() : nullptr;
		if (!Provider)
		{
			OutError = FFlockError::MakeWithFix(EFlockErrorType::Validation,
				TEXT("Flock analytics is not available: Flock is not initialized, or analytics is turned off"), FString(TEXT("Turn on Analytics Enabled under Project Settings > Flock SDK. ")) + FFlockErrorHints::InitializeFlockFirst());
		}
		return Provider;
	}
}

UFlockFlushAnalyticsAction* UFlockFlushAnalyticsAction::FlushAnalytics(UObject* WorldContextObject)
{
	UFlockFlushAnalyticsAction* Action = NewObject<UFlockFlushAnalyticsAction>();
	Action->WorldContextObject = WorldContextObject;
	return Action;
}

void UFlockFlushAnalyticsAction::Activate()
{
	FFlockError Error;
	FFlockAnalyticsProvider* Provider = ResolveProvider(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockAnalyticsAck>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockFlushAnalyticsAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject));
	Provider->Flush([WeakThis](TFlockResult<FFlockAnalyticsAck> Result)
	{
		if (UFlockFlushAnalyticsAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockFlushAnalyticsAction::Complete(const TFlockResult<FFlockAnalyticsAck>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(FFlockError());
	}
	else
	{
		OnFailure.Broadcast(Result.Error);
	}
	SetReadyToDestroy();
}

UFlockAnalyticsSessionAction* UFlockAnalyticsSessionAction::StartAnalyticsSession(UObject* WorldContextObject,
	const FString& PlayerId)
{
	UFlockAnalyticsSessionAction* Action = NewObject<UFlockAnalyticsSessionAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->bStart = true;
	Action->PlayerId = PlayerId;
	return Action;
}

UFlockAnalyticsSessionAction* UFlockAnalyticsSessionAction::EndAnalyticsSession(UObject* WorldContextObject)
{
	UFlockAnalyticsSessionAction* Action = NewObject<UFlockAnalyticsSessionAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->bStart = false;
	return Action;
}

void UFlockAnalyticsSessionAction::Activate()
{
	FFlockError Error;
	FFlockAnalyticsProvider* Provider = ResolveProvider(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(false, FString(), Error);
		return;
	}

	TWeakObjectPtr<UFlockAnalyticsSessionAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject));

	if (bStart)
	{
		Provider->StartSession(PlayerId, [WeakThis](TFlockResult<FString> Result)
		{
			if (UFlockAnalyticsSessionAction* Self = WeakThis.Get())
			{
				Self->Complete(Result.bSuccess, Result.Value, Result.Error);
			}
		});
		return;
	}

	// The id is reported before the end call clears it, so the graph can still correlate.
	const FString EndedId = Provider->GetCurrentSessionId();
	Provider->EndSession(EFlockSessionEndReason::Manual,
		[WeakThis, EndedId](TFlockResult<FFlockAnalyticsAck> Result)
		{
			if (UFlockAnalyticsSessionAction* Self = WeakThis.Get())
			{
				Self->Complete(Result.bSuccess, EndedId, Result.Error);
			}
		});
}

void UFlockAnalyticsSessionAction::Complete(bool bSuccess, const FString& SessionId, const FFlockError& Error)
{
	if (bSuccess)
	{
		OnSuccess.Broadcast(SessionId, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FString(), Error);
	}
	SetReadyToDestroy();
}
