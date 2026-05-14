// Automatic session lifecycle for the Flock analytics pipeline. Mirrors the Unity SDK
// behavior:
//   - StartSession fires once both player_id (auth) and game_version_id (config) are
//     ready. Subsequent login changes that swap player_id end the old session and
//     start a new one.
//   - On backgrounding (desktop focus loss or mobile background), a timer starts.
//     If foreground returns before AnalyticsSessionTimeoutSeconds elapses, the
//     session continues. Past the timeout, EndSession fires and a fresh
//     StartSession runs on return.
//   - On graceful quit (FCoreDelegates::OnPreExit / Deinitialize), EndSession is
//     dispatched best-effort. Crashes are not handled here.
//   - screens_viewed increments on every PostLoadMapWithWorld. is_bounce is true
//     when the session was shorter than AnalyticsBounceThresholdSeconds OR fewer
//     than two screens were visited.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
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
	// Auth's dynamic multicast — must be a UFUNCTION to bind via AddDynamic.
	UFUNCTION()
	void HandleAccessTokenChanged(const FString& Token);

	// FFlockOnSessionStart / FFlockOnGenericResponse are dynamic delegates declared
	// on UQwackAnalyticsSubsystem — handler signatures must match and be UFUNCTIONs.
	UFUNCTION()
	void HandleAutoStartCompleted(const FFlockSessionStartResponse& Response);

	UFUNCTION()
	void HandleAutoEndCompleted(const FFlockGenericResponse& Response);

	UFUNCTION()
	void HandleEndForRestartCompleted(const FFlockGenericResponse& Response);

	void TryStartSession();
	void FireStartSession();
	void FireEndSession(bool bChainStartAfter);

	void OnPostLoadMapWithWorld(UWorld* LoadedWorld);
	void OnEnterBackground();
	void OnEnterForeground();
	void OnPreExit();

	UQwackAnalyticsSubsystem* GetAnalytics() const;
	UQwackAuthSubsystem* GetAuth() const;
	UQwackConfigSubsystem* GetConfig() const;

	// Delegate handles for the non-dynamic multicasts so we can unbind in Deinitialize.
	FDelegateHandle DeactivateHandle;
	FDelegateHandle ReactivateHandle;
	FDelegateHandle BackgroundHandle;
	FDelegateHandle ForegroundHandle;
	FDelegateHandle PostLoadMapHandle;
	FDelegateHandle PreExitHandle;
	FDelegateHandle TerminateHandle;

	bool bSessionActive = false;
	bool bStartInFlight = false;
	bool bEndDispatched = false;       // true once OnPreExit/Deinitialize end fired
	bool bGameVersionCallbackRegistered = false;

	// Snapshot of the player_id we used to start the active session. Used to detect
	// re-login (player change) so we can end the old session and start a new one.
	FString SessionPlayerId;

	FString ActiveSessionId;
	FDateTime SessionStartUtc;
	double SessionStartMonotonic = 0.0;
	int32 ScreensViewed = 0;

	// Background timing — we use monotonic time so a wall-clock change doesn't
	// confuse the timeout calculation.
	bool bInBackground = false;
	double BackgroundEnterMonotonic = 0.0;
};
