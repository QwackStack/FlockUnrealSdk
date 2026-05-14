#include "QwackSessionSubsystem.h"

#include "Engine/GameInstance.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "Qwack_ue_Sdk/Analytics/QwackAnalyticsSubsystem.h"
#include "Qwack_ue_Sdk/Auth/QwackAuthSubsystem.h"
#include "Qwack_ue_Sdk/Config/QwackConfigSubsystem.h"
#include "Qwack_ue_Sdk/Config/QwackSettings.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogFlockSession, Log, All);

namespace
{
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

	// Screens are counted whether or not we end up auto-starting, so a manual
	// StartSession later in the run still gets the running tally.
	PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
		this, &UQwackSessionSubsystem::OnPostLoadMapWithWorld);

	if (!bAutoStart)
	{
		return;
	}

	// Idle/background hooks — both pairs route through the same handlers so desktop
	// focus loss and mobile background both contribute. bInBackground guards re-entry.
	DeactivateHandle = FCoreDelegates::ApplicationWillDeactivateDelegate.AddUObject(this, &UQwackSessionSubsystem::OnEnterBackground);
	ReactivateHandle = FCoreDelegates::ApplicationHasReactivatedDelegate.AddUObject(this, &UQwackSessionSubsystem::OnEnterForeground);
	BackgroundHandle = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(this, &UQwackSessionSubsystem::OnEnterBackground);
	ForegroundHandle = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(this, &UQwackSessionSubsystem::OnEnterForeground);

	// Quit path: OnPreExit fires before UObject teardown so the HTTP module is still alive;
	// ApplicationWillTerminate is the OS-side equivalent and catches kill paths that bypass
	// OnPreExit. Both route through the same bEndDispatched-guarded handler.
	PreExitHandle = FCoreDelegates::OnPreExit.AddUObject(this, &UQwackSessionSubsystem::OnPreExit);
	TerminateHandle = FCoreDelegates::ApplicationWillTerminateDelegate.AddUObject(this, &UQwackSessionSubsystem::OnPreExit);

	if (UQwackAuthSubsystem* Auth = GetAuth())
	{
		Auth->OnAccessTokenChanged.AddDynamic(this, &UQwackSessionSubsystem::HandleAccessTokenChanged);
	}

	// Kick off game-version resolution if it hasn't been already — TryStartSession
	// will be re-fired through the resolve callback once the UUID lands.
	if (Config)
	{
		Config->EnsureGameVersionResolved();
	}

	// Maybe everything is ready already (cached token from a refresh, version
	// resolved synchronously, etc.). One shot at start before delegates fire.
	TryStartSession();
}

void UQwackSessionSubsystem::Deinitialize()
{
	// Best-effort end on graceful shutdown when OnPreExit didn't catch us
	// (e.g. PIE stop in the editor doesn't always fire OnPreExit).
	if (bSessionActive && !bEndDispatched)
	{
		FireEndSession(/*bChainStartAfter*/ false);
	}

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

void UQwackSessionSubsystem::HandleAccessTokenChanged(const FString& /*Token*/)
{
	const UQwackAuthSubsystem* Auth = GetAuth();
	const FString CurrentPlayerId = Auth ? Auth->GetPlayerId() : FString();

	// Logout: end the active session and stop here.
	if (CurrentPlayerId.IsEmpty())
	{
		if (bSessionActive && !bEndDispatched)
		{
			FireEndSession(/*bChainStartAfter*/ false);
		}
		return;
	}

	// Re-login as a different player: cycle the session.
	if (bSessionActive && CurrentPlayerId != SessionPlayerId)
	{
		FireEndSession(/*bChainStartAfter*/ true);
		return;
	}

	// First login / token refresh under the same player.
	TryStartSession();
}

void UQwackSessionSubsystem::TryStartSession()
{
	if (bSessionActive || bStartInFlight) return;

	const UQwackAuthSubsystem* Auth = GetAuth();
	const FString PlayerId = Auth ? Auth->GetPlayerId() : FString();
	if (PlayerId.IsEmpty())
	{
		// Wait for auth — HandleAccessTokenChanged will re-fire us.
		return;
	}

	UQwackConfigSubsystem* Config = GetConfig();
	if (!Config)
	{
		return;
	}
	if (!Config->IsGameVersionResolved())
	{
		// Register a one-shot callback. Guard so we don't pile up callbacks on
		// repeated TryStartSession calls before resolution lands.
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
					else UE_LOG(LogFlockSession, Warning,
						TEXT("Game version resolution failed — auto-session skipped."));
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

	// Dynamic delegate types are declared inside UQwackAnalyticsSubsystem — qualify
	// when constructing from another class.
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
			TEXT("Auto StartSession failed (code=%d) — session inactive."),
			Response.Meta.StatusCode);
		bSessionActive = false;
		ActiveSessionId.Reset();
		return;
	}
	bSessionActive = true;
	ActiveSessionId = Response.session_id;
	// Context's session_id was already set by UQwackAnalyticsSubsystem::StartSession
	// in the same response handling — no need to duplicate here.
}

// =====================================================================
// End-of-session
// =====================================================================

void UQwackSessionSubsystem::FireEndSession(bool bChainStartAfter)
{
	UQwackAnalyticsSubsystem* Analytics = GetAnalytics();
	if (!Analytics) return;

	// Snapshot before clearing so the EndSession payload reflects the session we're closing,
	// even if a new StartSession is queued behind it.
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

	// Local state goes inactive immediately — late events shouldn't be tagged to a
	// session we're closing, and a chained restart needs a clean slate.
	bSessionActive = false;
	bEndDispatched = true;
	ActiveSessionId.Reset();
	SessionPlayerId.Reset();
	ScreensViewed = 0;

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

void UQwackSessionSubsystem::HandleAutoEndCompleted(const FFlockGenericResponse& /*Response*/)
{
	// Nothing further — analytics subsystem cleared ContextSubsystem::SessionId.
}

void UQwackSessionSubsystem::HandleEndForRestartCompleted(const FFlockGenericResponse& /*Response*/)
{
	// Chain a fresh session regardless of end-call outcome. If end failed (network drop)
	// we still want a new session on the front; the orphan will time out server-side.
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
		// End the stale session and chain a new one — keeps Unity's "after timeout,
		// new session_id" behavior so per-event analytics correctly attribute play.
		FireEndSession(/*bChainStartAfter*/ true);
	}
}

void UQwackSessionSubsystem::OnPreExit()
{
	if (bEndDispatched || !bSessionActive) return;
	FireEndSession(/*bChainStartAfter*/ false);
	// HTTP dispatched but likely won't complete before process exit — that's accepted loss.
	// A future enhancement is a session heartbeat persisted to disk so the next launch
	// can finalize an unterminated session server-side.
}
