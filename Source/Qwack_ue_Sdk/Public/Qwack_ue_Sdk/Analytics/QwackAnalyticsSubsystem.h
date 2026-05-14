// /v1/analytics endpoints — sessions, events (single + batch), transactions.
// Owns a disk-backed FFlockEventSpool for write-ahead retry of analytics events when
// the network or auth is unavailable. Binds to UQwackAuthSubsystem::OnAccessTokenChanged
// to drain the spool whenever a fresh token arrives.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Qwack_ue_Sdk/Utils/Schemas.h"
#include "QwackAnalyticsSubsystem.generated.h"

class FFlockEventSpool;
struct FQwackHTTPResponse;

UCLASS()
class QWACK_UE_SDK_API UQwackAnalyticsSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	DECLARE_DYNAMIC_DELEGATE_OneParam(FFlockOnSessionStart,    const FFlockSessionStartResponse&, Response);
	DECLARE_DYNAMIC_DELEGATE_OneParam(FFlockOnGenericResponse, const FFlockGenericResponse&,      Response);

	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics")
	void StartSession(const FFlockSessionStartRequest& Request, const FFlockOnSessionStart& Callback);

	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics")
	void EndSession(const FString& SessionId, const FFlockSessionEndRequest& Request, const FFlockOnGenericResponse& Callback);

	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics")
	void TrackEvent(const FFlockAnalyticsEventRequest& Request, const FFlockOnGenericResponse& Callback);

	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics")
	void TrackEvents(const FFlockAnalyticsEventsRequest& Request, const FFlockOnGenericResponse& Callback);

	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics")
	void RecordTransaction(const FFlockTransactionRequest& Request, const FFlockOnGenericResponse& Callback);

private:
	// Disk spool kept off the UObject graph — per-event entries would otherwise add GC
	// walk cost. Null when caching is disabled in settings.
	TUniquePtr<FFlockEventSpool> Spool;

	UFUNCTION()
	void HandleAccessTokenChanged(const FString& Token);

	// Apply HTTP response to the spool: drop on 2xx / 4xx-permanent, leave on transient.
	// Triggers a follow-up flush when there's still pending work after a successful send.
	void OnSpoolResponse(const TArray<FString>& Handles, const FQwackHTTPResponse& R);

	// Drains one batch from the spool to /v1/analytics/events. Self-recurses while the
	// spool is non-empty and the previous batch landed.
	void FlushSpoolAsBatch();

	// Non-static: needs GetGameInstance() to reach the context subsystem.
	FString SerializeEvent(const FFlockAnalyticsEventRequest& Req) const;
	static FString BuildBatchPayload(const TArray<FString>& Bodies);
};
