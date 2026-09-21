// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "FlockEventModels.h"
#include "FlockLogger.h"
#include "Models/FlockNotificationModels.h"
#include "FlockEvents.generated.h"

// ── Multicast events (Assign in Blueprint, AddDynamic in C++) ──
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFlockOnInitialized);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockOnInitializationFailed, const FString&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFlockOnShutdown);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockOnAuthenticated, const FFlockAuthInfo&, Info);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFlockOnTokenRefreshed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFlockOnAuthExpired);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFlockOnLoggedOut);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockOnSessionRestored, bool, bRestored);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockOnAccountLinked, EFlockCredentialProvider, Provider);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockOnAccountUnlinked, EFlockCredentialProvider, Provider);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockOnSessionStarted, const FString&, SessionId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockOnSessionRegistered, const FString&, SessionId, const FString&, ServerSessionId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockOnSessionEnded, const FFlockSessionEndedArgs&, Args);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFlockOnSessionPaused);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFlockOnSessionResumed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockOnUnreadCountChanged, int32, UnreadCount);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockOnNotificationReceived, const FFlockNotification&, Notification);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockOnConsentChanged, bool, bGranted);

// ── One-shot callbacks for the CallOrRegister entry points ──
DECLARE_DYNAMIC_DELEGATE(FFlockInitializedCallback);
DECLARE_DYNAMIC_DELEGATE_OneParam(FFlockInitializationFailedCallback, const FString&, Error);

/**
 * Hub for SDK lifecycle/auth/session/consent events — get it via UFlockSubsystem::GetEvents(). Every
 * event is raised on the game thread and debug-logged (subscriber count included) when Enable Debug
 * Logs is on. The lifecycle events are live; auth, session, and consent events are declared now and
 * raised by their features as they land.
 *
 * Auto-init completes during GameInstance startup — before any Blueprint can bind — so a plain
 * OnInitialized binding misses it. Use CallOrRegister_OnInitialized, which fires immediately when the
 * SDK is already initialized.
 *
 * Subscriptions are not cleared by ShutdownSdk(): they persist across re-initialization and are
 * released with the owning GameInstance (dynamic delegates hold weak references, so a destroyed
 * subscriber is skipped safely).
 */
UCLASS()
class FLOCK_API UFlockEvents : public UObject
{
	GENERATED_BODY()

public:
	// ── Lifecycle ──

	/** SDK initialized. Under auto-init this fires before Blueprints can bind — see CallOrRegister_OnInitialized. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnInitialized OnInitialized;

	/** Initialization failed; carries the error message. The "already initialized" misuse guard does not raise it. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnInitializationFailed OnInitializationFailed;

	/** ShutdownSdk() completed. Subscriptions survive shutdown (they clear with the GameInstance). */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnShutdown OnShutdown;

	// ── Auth (raised by the authentication feature when it lands) ──

	/** A player signed in (login, register, or restored session) — see FFlockAuthInfo::Method. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnAuthenticated OnAuthenticated;

	/** The access token was refreshed successfully. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnTokenRefreshed OnTokenRefreshed;

	/** Token refresh failed — the player must log in again. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnAuthExpired OnAuthExpired;

	/** Logout completed (local-only: nothing revoked server-side). */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnLoggedOut OnLoggedOut;

	/** Session restore finished; payload is whether a session was restored (also fires false when there was none). */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnSessionRestored OnSessionRestored;

	/** A credential was attached to the signed-in player. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnAccountLinked OnAccountLinked;

	/** A credential was removed from the signed-in player. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnAccountUnlinked OnAccountUnlinked;

	// ── Session (raised by the session/analytics features when they land) ──

	/**
	 * A gameplay/analytics session began; payload is the local session id. The server's id for the session arrives
	 * later, with OnSessionRegistered.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnSessionStarted OnSessionStarted;

	/**
	 * The running session reached the server, which gave it the id its records are filed under (the id
	 * GetAnalyticsSessionId returns). SessionId is the local id OnSessionStarted carried. Raised once per session,
	 * when its start call succeeds or the heartbeat's later retry registers it; never for a session that ended before
	 * that. With Analytics Heartbeat Interval at 0 a failed start call is not retried while the session runs, so this
	 * does not fire for that session. It is not replayed: code that binds after a session registered reads
	 * GetAnalyticsSessionId instead.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnSessionRegistered OnSessionRegistered;

	/** A session ended (any path); payload carries the final snapshot and the reason. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnSessionEnded OnSessionEnded;

	/** The active session was paused (app backgrounded). */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnSessionPaused OnSessionPaused;

	/** The paused session resumed (app foregrounded). */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnSessionResumed OnSessionResumed;

	// ── Notifications ──

	/**
	 * The player's unread count changed. Raised only when the server reports a count — an unread-count or
	 * summary fetch, or a mark-all-read — never from a background poll, because the SDK does not run one.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnUnreadCountChanged OnUnreadCountChanged;

	/**
	 * A notification the SDK has not surfaced before came back from an inbox or summary fetch — schedule,
	 * trigger and campaign deliveries alike.
	 *
	 * "Received" means first seen by a read, not the instant the server created it: there is no realtime
	 * channel and the SDK never polls. The first fetch for a player seeds silently, so an existing inbox
	 * does not arrive as a burst; after that each new notification raises once, oldest first. Inspect
	 * CampaignId (campaign) or `trigger_id` in Data (trigger) to tell the sources apart.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnNotificationReceived OnNotificationReceived;

	// ── Consent ──

	/** Analytics consent was granted or revoked; payload is the new state. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Events")
	FFlockOnConsentChanged OnConsentChanged;

	// ── Late-binder-safe registration ──

	/**
	 * Runs Callback once the SDK is initialized: immediately when it already is (the auto-init case),
	 * otherwise on the next successful initialization. One-shot — it does not fire again on re-init.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Events")
	void CallOrRegister_OnInitialized(FFlockInitializedCallback Callback);

	/**
	 * Runs Callback with the initialization error: immediately when init already failed, otherwise on
	 * the next failure. One-shot.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Events")
	void CallOrRegister_OnInitializationFailed(FFlockInitializationFailedCallback Callback);

	// ── Raise entry points (C++ — the subsystem and feature providers call these) ──

	void InvokeInitialized();
	void InvokeInitializationFailed(const FString& Error);
	void InvokeShutdown();
	void InvokeAuthenticated(const FFlockAuthInfo& Info);
	void InvokeTokenRefreshed();
	void InvokeAuthExpired();
	void InvokeLoggedOut();
	void InvokeSessionRestored(bool bRestored);
	void InvokeAccountLinked(EFlockCredentialProvider Provider);
	void InvokeAccountUnlinked(EFlockCredentialProvider Provider);
	void InvokeSessionStarted(const FString& SessionId);
	void InvokeSessionRegistered(const FString& SessionId, const FString& ServerSessionId);
	void InvokeSessionEnded(const FFlockSessionEndedArgs& Args);
	void InvokeSessionPaused();
	void InvokeSessionResumed();
	void InvokeUnreadCountChanged(int32 UnreadCount);
	void InvokeNotificationReceived(const FFlockNotification& Notification);
	void InvokeConsentChanged(bool bGranted);

	/** Debug-logs every raise through this logger; kept in sync with the subsystem's logger. */
	void SetLogger(const TSharedPtr<IFlockLogger>& InLogger) { Logger = InLogger; }

private:
	/** Logs "<Event> fired -> N subscriber(s)" so raises are visible with Enable Debug Logs on. */
	void LogRaise(const TCHAR* EventName, int32 SubscriberCount) const;

	/** Replay state for CallOrRegister, kept current by the Invoke entry points. */
	bool bSdkInitialized = false;
	FString LastInitializationError;

	// No UPROPERTY needed: dynamic delegates hold weak object references, so a dead
	// subscriber is skipped by ExecuteIfBound rather than kept alive or crashed into.
	TArray<FFlockInitializedCallback> PendingInitialized;
	TArray<FFlockInitializationFailedCallback> PendingInitializationFailed;

	TSharedPtr<IFlockLogger> Logger;
};
