// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Http/FlockResult.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Models/FlockAnalyticsModels.h"
#include "FlockAnalyticsAsyncActions.generated.h"

/**
 * Blueprint async nodes for the analytics calls that have an outcome worth waiting on. Each resolves
 * the SDK from its world context on Activate; when analytics is unavailable the failure pin fires
 * with a Validation error. Exactly one pin fires per activation.
 *
 * The fire-and-forget half of the API (Log Event / Error / Exception, Record Screen View, consent,
 * snapshot) is on UFlockSubsystem directly — those never fail at the call site, so a latent node
 * would only add noise to a graph.
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockAnalyticsPin, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockAnalyticsSessionPin, const FString&, SessionId, const FFlockError&, Error);

/** Drains the offline spool now instead of waiting for the periodic flush. */
UCLASS()
class FLOCK_API UFlockFlushAnalyticsAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockAnalyticsPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockAnalyticsPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Flush Analytics"), Category = "Flock|Analytics")
	static UFlockFlushAnalyticsAction* FlushAnalytics(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockAnalyticsAck>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;
};

/**
 * Opens or closes an analytics session explicitly. Games with Auto Start Session on do not need the
 * start node — the SDK opens one on sign-in, because the backend requires a player id.
 */
UCLASS()
class FLOCK_API UFlockAnalyticsSessionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockAnalyticsSessionPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockAnalyticsSessionPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Start Analytics Session"), Category = "Flock|Analytics")
	static UFlockAnalyticsSessionAction* StartAnalyticsSession(UObject* WorldContextObject, const FString& PlayerId);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock End Analytics Session"), Category = "Flock|Analytics")
	static UFlockAnalyticsSessionAction* EndAnalyticsSession(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	void Complete(bool bSuccess, const FString& SessionId, const FFlockError& Error);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	bool bStart = true;
	FString PlayerId;
};
