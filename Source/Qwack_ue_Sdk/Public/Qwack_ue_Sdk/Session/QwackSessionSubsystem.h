// Auto session lifecycle. StartSession fires once auth + game_version_id are ready;
// EndSession on background timeout, login swap, quit, or crash-recovered next launch.
// screens_viewed counts PostLoadMapWithWorld; is_bounce = duration<threshold || screens<=1.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Qwack_ue_Sdk/Session/FlockSessionStore.h"
#include "Qwack_ue_Sdk/Utils/Schemas.h"
#include "QwackSessionSubsystem.generated.h"

class UQwackAnalyticsSubsystem;
class UQwackAuthSubsystem;
class UQwackConfigSubsystem;
class UWorld;

UCLASS()
class QWACK_UE_SDK_API UQwackSessionSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "Flock|Session")
	bool IsSessionActive() const { return bSessionActive; }

	UFUNCTION(BlueprintCallable, Category = "Flock|Session")
	FString GetActiveSessionId() const { return ActiveSessionId; }

	UFUNCTION(BlueprintCallable, Category = "Flock|Session")
	int32 GetScreensViewed() const { return ScreensViewed; }

private:
	// AddDynamic targets must be UFUNCTIONs.
	UFUNCTION() void HandleAccessTokenChanged(const FString& Token);
	UFUNCTION() void HandleAutoStartCompleted(const FFlockSessionStartResponse& Response);
	UFUNCTION() void HandleAutoEndCompleted(const FFlockGenericResponse& Response);
	UFUNCTION() void HandleEndForRestartCompleted(const FFlockGenericResponse& Response);
	UFUNCTION() void HandleRecoveryEndCompleted(const FFlockGenericResponse& Response);

	void TryStartSession();
	void FireStartSession();
	void FireEndSession(bool bChainStartAfter);

	void PersistActiveMarker();
	void StartHeartbeatTicker();
	void StopHeartbeatTicker();
	bool OnHeartbeatTick(float DeltaSeconds);

	void DispatchNextRecovery();

	void OnPostLoadMapWithWorld(UWorld* LoadedWorld);
	void OnEnterBackground();
	void OnEnterForeground();
	void OnPreExit();

	UQwackAnalyticsSubsystem* GetAnalytics() const;
	UQwackAuthSubsystem* GetAuth() const;
	UQwackConfigSubsystem* GetConfig() const;

	FDelegateHandle DeactivateHandle;
	FDelegateHandle ReactivateHandle;
	FDelegateHandle BackgroundHandle;
	FDelegateHandle ForegroundHandle;
	FDelegateHandle PostLoadMapHandle;
	FDelegateHandle PreExitHandle;
	FDelegateHandle TerminateHandle;

	bool bSessionActive = false;
	bool bStartInFlight = false;
	bool bEndDispatched = false;
	bool bGameVersionCallbackRegistered = false;

	// player_id at session start; mismatch with auth signals a re-login.
	FString SessionPlayerId;

	FString ActiveSessionId;
	FDateTime SessionStartUtc;
	double SessionStartMonotonic = 0.0;
	int32 ScreensViewed = 0;

	// Monotonic time avoids wall-clock skew.
	bool bInBackground = false;
	double BackgroundEnterMonotonic = 0.0;

	// Null when AnalyticsPersistSession is off. Off the UObject graph.
	TUniquePtr<FFlockSessionStore> Store;

	FTSTicker::FDelegateHandle HeartbeatHandle;

	// Drained serially; RecoveryInFlightFilename also guards DispatchNextRecovery re-entry.
	TArray<FString> PendingRecoveryQueue;
	FString RecoveryInFlightFilename;

	// Set by FireEndSession's rotate; consumed by the end callback to delete-or-keep.
	FString LastRotatedPendingFile;
};
