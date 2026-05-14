#include "QwackSessionSubsystem.h"

#include "Engine/GameInstance.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Qwack_ue_Sdk/Analytics/QwackAnalyticsSubsystem.h"
#include "Qwack_ue_Sdk/Auth/QwackAuthSubsystem.h"
#include "Qwack_ue_Sdk/Config/QwackConfigSubsystem.h"
#include "Qwack_ue_Sdk/Config/QwackSettings.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogFlockSession, Log, All);

namespace
{
	bool IsHttpSuccess(int32 Code) { return Code >= 200 && Code < 300; }

	// 4xx (except 408/429) won't change on retry — safe to drop the marker.
	bool IsHttpPermanent(int32 Code)
	{
		return Code >= 400 && Code < 500 && Code != 408 && Code != 429;
	}

	const TCHAR* PlatformLabel()
	{
#if PLATFORM_WINDOWS
		return TEXT("Windows");
#elif PLATFORM_MAC
		return TEXT("Mac");
#elif PLATFORM_LINUX
		return TEXT("Linux");
#elif PLATFORM_IOS
		return TEXT("iOS");
#elif PLATFORM_ANDROID
		return TEXT("Android");
#else
		return TEXT("Unknown");
#endif
	}

	const TCHAR* DeviceTypeLabel()
	{
#if PLATFORM_IOS || PLATFORM_ANDROID
		return TEXT("mobile");
#elif PLATFORM_DESKTOP
		return TEXT("desktop");
#else
		return TEXT("console");
#endif
	}
}

void UQwackSessionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UQwackConfigSubsystem* Config = GetConfig();
	const UQwackSettings* Settings = Config ? Config->GetSettings() : GetDefault<UQwackSettings>();
	const bool bAutoStart = Settings ? Settings->AnalyticsAutoStartSession : true;
	const bool bPersist = Settings ? Settings->AnalyticsPersistSession : true;

	// Count screens regardless so a manual StartSession later still has the tally.
	PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
		this, &UQwackSessionSubsystem::OnPostLoadMapWithWorld);

	if (!bAutoStart)
	{
		return;
	}

	// A leftover active.json means the previous run died without EndSession.
	if (bPersist)
	{
		Store = MakeUnique<FFlockSessionStore>(
			FPaths::ProjectSavedDir() / TEXT("Flock") / TEXT("sessions"));

		const FString Rotated = Store->RotateActiveToPending();
		if (!Rotated.IsEmpty())
		{
			UE_LOG(LogFlockSession, Log, TEXT("Recovered orphan session marker."));
		}
		PendingRecoveryQueue = Store->ListPending();
	}

	// Desktop deactivate + mobile background both go through OnEnterBackground;
	// bInBackground guards overlapping pairs.
	DeactivateHandle = FCoreDelegates::ApplicationWillDeactivateDelegate.AddUObject(this, &UQwackSessionSubsystem::OnEnterBackground);
	ReactivateHandle = FCoreDelegates::ApplicationHasReactivatedDelegate.AddUObject(this, &UQwackSessionSubsystem::OnEnterForeground);
	BackgroundHandle = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(this, &UQwackSessionSubsystem::OnEnterBackground);
	ForegroundHandle = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(this, &UQwackSessionSubsystem::OnEnterForeground);

	// OnPreExit covers normal quit; WillTerminate catches OS-initiated kills.
	PreExitHandle = FCoreDelegates::OnPreExit.AddUObject(this, &UQwackSessionSubsystem::OnPreExit);
	TerminateHandle = FCoreDelegates::ApplicationWillTerminateDelegate.AddUObject(this, &UQwackSessionSubsystem::OnPreExit);

	if (UQwackAuthSubsystem* Auth = GetAuth())
	{
		Auth->OnAccessTokenChanged.AddDynamic(this, &UQwackSessionSubsystem::HandleAccessTokenChanged);
	}

	if (Config)
	{
		Config->EnsureGameVersionResolved();
	}

	// Cached token bypasses HandleAccessTokenChanged — kick recovery + start manually.
	if (UQwackAuthSubsystem* Auth = GetAuth(); Auth && !Auth->GetAccessToken().IsEmpty())
	{
		DispatchNextRecovery();
	}

	TryStartSession();
}

void UQwackSessionSubsystem::Deinitialize()
{
	// Catches PIE stop and other paths that skip OnPreExit.
	if (bSessionActive && !bEndDispatched)
	{
		FireEndSession(/*bChainStartAfter*/ false);
	}

	StopHeartbeatTicker();

	FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
	FCoreDelegates::ApplicationWillDeactivateDelegate.Remove(DeactivateHandle);
	FCoreDelegates::ApplicationHasReactivatedDelegate.Remove(ReactivateHandle);
	FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(BackgroundHandle);
	FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(ForegroundHandle);
	FCoreDelegates::OnPreExit.Remove(PreExitHandle);
	FCoreDelegates::ApplicationWillTerminateDelegate.Remove(TerminateHandle);

	if (UQwackAuthSubsystem* Auth = GetAuth())
	{
		Auth->OnAccessTokenChanged.RemoveDynamic(this, &UQwackSessionSubsystem::HandleAccessTokenChanged);
	}

	Store.Reset();
	Super::Deinitialize();
}

// =====================================================================
// Subsystem lookups
// =====================================================================

UQwackAnalyticsSubsystem* UQwackSessionSubsystem::GetAnalytics() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<UQwackAnalyticsSubsystem>() : nullptr;
}

UQwackAuthSubsystem* UQwackSessionSubsystem::GetAuth() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<UQwackAuthSubsystem>() : nullptr;
}

UQwackConfigSubsystem* UQwackSessionSubsystem::GetConfig() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<UQwackConfigSubsystem>() : nullptr;
}

// =====================================================================
// Readiness gate + auto start
// =====================================================================

void UQwackSessionSubsystem::HandleAccessTokenChanged(const FString& Token)
{
	const UQwackAuthSubsystem* Auth = GetAuth();
	const FString CurrentPlayerId = Auth ? Auth->GetPlayerId() : FString();

	// Logout.
	if (CurrentPlayerId.IsEmpty())
	{
		if (bSessionActive && !bEndDispatched)
		{
			FireEndSession(/*bChainStartAfter*/ false);
		}
		return;
	}

	if (!Token.IsEmpty())
	{
		DispatchNextRecovery();
	}

	// Re-login as a different player cycles the session.
	if (bSessionActive && CurrentPlayerId != SessionPlayerId)
	{
		FireEndSession(/*bChainStartAfter*/ true);
		return;
	}

	TryStartSession();
}

void UQwackSessionSubsystem::TryStartSession()
{
	if (bSessionActive || bStartInFlight) return;

	const UQwackAuthSubsystem* Auth = GetAuth();
	const FString PlayerId = Auth ? Auth->GetPlayerId() : FString();
	if (PlayerId.IsEmpty()) return;

	UQwackConfigSubsystem* Config = GetConfig();
	if (!Config) return;

	if (!Config->IsGameVersionResolved())
	{
		// One-shot callback; guard against piling up callbacks on repeated calls.
		if (!bGameVersionCallbackRegistered)
		{
			bGameVersionCallbackRegistered = true;
			TWeakObjectPtr<UQwackSessionSubsystem> WeakThis(this);
			Config->OnGameVersionResolved([WeakThis](bool bSuccess)
			{
				if (UQwackSessionSubsystem* Self = WeakThis.Get())
				{
					Self->bGameVersionCallbackRegistered = false;
					if (bSuccess) Self->TryStartSession();
					else UE_LOG(LogFlockSession, Warning, TEXT("Game version unresolved; auto-session skipped."));
				}
			});
		}
		Config->EnsureGameVersionResolved();
		return;
	}

	FireStartSession();
}

void UQwackSessionSubsystem::FireStartSession()
{
	UQwackAnalyticsSubsystem* Analytics = GetAnalytics();
	const UQwackAuthSubsystem* Auth = GetAuth();
	const UQwackConfigSubsystem* Config = GetConfig();
	if (!Analytics || !Auth || !Config) return;

	bStartInFlight = true;
	SessionPlayerId = Auth->GetPlayerId();
	SessionStartUtc = FDateTime::UtcNow();
	SessionStartMonotonic = FPlatformTime::Seconds();
	ScreensViewed = 0;

	FFlockSessionStartRequest Req;
	Req.player_id = SessionPlayerId;
	Req.platform = PlatformLabel();
	Req.device_type = DeviceTypeLabel();
	Req.game_version_id = Config->GetGameVersionId();
	Req.started_at = SessionStartUtc.ToIso8601();

	UQwackAnalyticsSubsystem::FFlockOnSessionStart Cb;
	Cb.BindDynamic(this, &UQwackSessionSubsystem::HandleAutoStartCompleted);
	Analytics->StartSession(Req, Cb);
}

void UQwackSessionSubsystem::HandleAutoStartCompleted(const FFlockSessionStartResponse& Response)
{
	bStartInFlight = false;
	if (!Response.Meta.bSuccess || Response.session_id.IsEmpty())
	{
		UE_LOG(LogFlockSession, Warning,
			TEXT("Auto StartSession failed (code=%d)."),
			Response.Meta.StatusCode);
		bSessionActive = false;
		ActiveSessionId.Reset();
		return;
	}
	bSessionActive = true;
	ActiveSessionId = Response.session_id;
	// Context session_id is already set by UQwackAnalyticsSubsystem::StartSession.

	PersistActiveMarker();
	StartHeartbeatTicker();
}

// =====================================================================
// End-of-session
// =====================================================================

void UQwackSessionSubsystem::FireEndSession(bool bChainStartAfter)
{
	UQwackAnalyticsSubsystem* Analytics = GetAnalytics();
	if (!Analytics) return;

	// Snapshot before clearing so a chained restart can't trample the closing id.
	const FString ClosingSessionId = ActiveSessionId;
	if (ClosingSessionId.IsEmpty()) return;

	const UQwackConfigSubsystem* Config = GetConfig();
	const UQwackSettings* Settings = Config ? Config->GetSettings() : GetDefault<UQwackSettings>();
	const int32 BounceThreshold = Settings ? Settings->AnalyticsBounceThresholdSeconds : 10;

	const double DurationSec = FMath::Max(0.0, FPlatformTime::Seconds() - SessionStartMonotonic);
	const FDateTime EndUtc = FDateTime::UtcNow();

	FFlockSessionEndRequest Req;
	Req.duration_seconds = FMath::RoundToInt(static_cast<float>(DurationSec));
	Req.screens_viewed = ScreensViewed;
	Req.is_bounce = (DurationSec < BounceThreshold) || (ScreensViewed <= 1);
	Req.ended_at = EndUtc.ToIso8601();

	// Clear state up-front so late events don't tag the closing session and a chained
	// restart starts clean.
	bSessionActive = false;
	bEndDispatched = true;
	ActiveSessionId.Reset();
	SessionPlayerId.Reset();
	ScreensViewed = 0;
	StopHeartbeatTicker();

	// Rotate the marker to pending/ before HTTP. End-callback deletes on success;
	// transient failure (or process exit before callback) leaves it for next launch.
	LastRotatedPendingFile = Store ? Store->RotateActiveToPending() : FString();

	UQwackAnalyticsSubsystem::FFlockOnGenericResponse Cb;
	if (bChainStartAfter)
	{
		Cb.BindDynamic(this, &UQwackSessionSubsystem::HandleEndForRestartCompleted);
	}
	else
	{
		Cb.BindDynamic(this, &UQwackSessionSubsystem::HandleAutoEndCompleted);
	}
	Analytics->EndSession(ClosingSessionId, Req, Cb);
}

void UQwackSessionSubsystem::HandleAutoEndCompleted(const FFlockGenericResponse& Response)
{
	if (Store && !LastRotatedPendingFile.IsEmpty())
	{
		if (IsHttpSuccess(Response.Meta.StatusCode) || IsHttpPermanent(Response.Meta.StatusCode))
		{
			Store->DeletePending(LastRotatedPendingFile);
		}
		LastRotatedPendingFile.Reset();
	}
}

void UQwackSessionSubsystem::HandleEndForRestartCompleted(const FFlockGenericResponse& Response)
{
	if (Store && !LastRotatedPendingFile.IsEmpty())
	{
		if (IsHttpSuccess(Response.Meta.StatusCode) || IsHttpPermanent(Response.Meta.StatusCode))
		{
			Store->DeletePending(LastRotatedPendingFile);
		}
		LastRotatedPendingFile.Reset();
	}

	// Always chain; if end failed the orphan stays in pending/ for retry.
	bEndDispatched = false;
	TryStartSession();
}

// =====================================================================
// Screen counting + background timeout
// =====================================================================

void UQwackSessionSubsystem::OnPostLoadMapWithWorld(UWorld* /*LoadedWorld*/)
{
	++ScreensViewed;
}

void UQwackSessionSubsystem::OnEnterBackground()
{
	if (bInBackground) return;
	bInBackground = true;
	BackgroundEnterMonotonic = FPlatformTime::Seconds();

	// Ticker won't fire while paused; flush a final marker for recovery.
	if (bSessionActive) PersistActiveMarker();
}

void UQwackSessionSubsystem::OnEnterForeground()
{
	if (!bInBackground) return;
	const double BackgroundedSec = FPlatformTime::Seconds() - BackgroundEnterMonotonic;
	bInBackground = false;

	if (!bSessionActive) return;

	const UQwackConfigSubsystem* Config = GetConfig();
	const UQwackSettings* Settings = Config ? Config->GetSettings() : GetDefault<UQwackSettings>();
	const int32 TimeoutSec = Settings ? Settings->AnalyticsSessionTimeoutSeconds : 30;

	if (BackgroundedSec >= TimeoutSec)
	{
		FireEndSession(/*bChainStartAfter*/ true);
	}
}

void UQwackSessionSubsystem::OnPreExit()
{
	if (bEndDispatched || !bSessionActive) return;
	// HTTP likely won't land before exit; FireEndSession already rotated to pending/
	// so next launch replays it.
	FireEndSession(/*bChainStartAfter*/ false);
}

// =====================================================================
// Disk persistence
// =====================================================================

void UQwackSessionSubsystem::PersistActiveMarker()
{
	if (!Store || ActiveSessionId.IsEmpty()) return;

	FFlockSessionMarker M;
	M.SessionId = ActiveSessionId;
	M.PlayerId = SessionPlayerId;
	if (const UQwackConfigSubsystem* Config = GetConfig())
	{
		M.GameVersionId = Config->GetGameVersionId();
	}
	M.StartedAt = SessionStartUtc;
	M.LastSeenAt = FDateTime::UtcNow();
	M.ScreensViewed = ScreensViewed;

	Store->WriteActive(M);
}

void UQwackSessionSubsystem::StartHeartbeatTicker()
{
	if (HeartbeatHandle.IsValid()) return;

	const UQwackConfigSubsystem* Config = GetConfig();
	const UQwackSettings* Settings = Config ? Config->GetSettings() : GetDefault<UQwackSettings>();
	const int32 IntervalSec = Settings ? Settings->AnalyticsHeartbeatIntervalSeconds : 60;
	if (IntervalSec <= 0 || !Store) return;

	HeartbeatHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UQwackSessionSubsystem::OnHeartbeatTick),
		static_cast<float>(IntervalSec));
}

void UQwackSessionSubsystem::StopHeartbeatTicker()
{
	if (HeartbeatHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(HeartbeatHandle);
		HeartbeatHandle.Reset();
	}
}

bool UQwackSessionSubsystem::OnHeartbeatTick(float /*DeltaSeconds*/)
{
	// false stops the ticker.
	if (!bSessionActive) return false;
	PersistActiveMarker();
	return true;
}

// =====================================================================
// Crash recovery — drain pending markers from previous runs
// =====================================================================

void UQwackSessionSubsystem::DispatchNextRecovery()
{
	if (!Store) return;
	if (!RecoveryInFlightFilename.IsEmpty()) return;       // one at a time
	if (PendingRecoveryQueue.Num() == 0) return;

	UQwackAnalyticsSubsystem* Analytics = GetAnalytics();
	const UQwackAuthSubsystem* Auth = GetAuth();
	// Wait for auth; HandleAccessTokenChanged retriggers.
	if (!Analytics || !Auth || Auth->GetAccessToken().IsEmpty()) return;

	const FString Filename = PendingRecoveryQueue[0];
	PendingRecoveryQueue.RemoveAt(0);

	FFlockSessionMarker Marker;
	if (!Store->TryReadPending(Filename, Marker))
	{
		UE_LOG(LogFlockSession, Warning, TEXT("Unreadable marker, dropping: %s"), *Filename);
		Store->DeletePending(Filename);
		DispatchNextRecovery();
		return;
	}

	const UQwackSettings* Settings = GetConfig() ? GetConfig()->GetSettings() : GetDefault<UQwackSettings>();
	const int32 BounceThreshold = Settings ? Settings->AnalyticsBounceThresholdSeconds : 10;

	const FTimespan Span = Marker.LastSeenAt - Marker.StartedAt;
	const double DurationSec = FMath::Max(0.0, Span.GetTotalSeconds());

	FFlockSessionEndRequest Req;
	Req.duration_seconds = FMath::RoundToInt(static_cast<float>(DurationSec));
	Req.screens_viewed = Marker.ScreensViewed;
	Req.is_bounce = (DurationSec < BounceThreshold) || (Marker.ScreensViewed <= 1);
	Req.ended_at = Marker.LastSeenAt.ToIso8601();

	RecoveryInFlightFilename = Filename;

	UQwackAnalyticsSubsystem::FFlockOnGenericResponse Cb;
	Cb.BindDynamic(this, &UQwackSessionSubsystem::HandleRecoveryEndCompleted);
	Analytics->EndSession(Marker.SessionId, Req, Cb);
}

void UQwackSessionSubsystem::HandleRecoveryEndCompleted(const FFlockGenericResponse& Response)
{
	if (Store && !RecoveryInFlightFilename.IsEmpty())
	{
		if (IsHttpSuccess(Response.Meta.StatusCode) || IsHttpPermanent(Response.Meta.StatusCode))
		{
			Store->DeletePending(RecoveryInFlightFilename);
		}
		// Transient (5xx/0/408/429) keeps the marker for next launch.
	}
	RecoveryInFlightFilename.Reset();
	DispatchNextRecovery();
}
