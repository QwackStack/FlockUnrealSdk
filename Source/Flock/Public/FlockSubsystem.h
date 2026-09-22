// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Auth/FlockAuthSession.h"
#include "Auth/FlockTokenStore.h"
#include "FlockInitConfig.h"
#include "FlockLogger.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockHttpClient.h"
// Included from the SDK's front door so an unsupported engine fails with the compat header's message
// first, rather than somewhere in the middle of a wall of unrelated errors.
#include "Misc/FlockEngineCompat.h"
#include "Providers/FlockAnalyticsProvider.h"
#include "Providers/FlockAuthProvider.h"
#include "Providers/FlockCommandProvider.h"
#include "Providers/FlockConfigProvider.h"
#include "Providers/FlockGameProvider.h"
#include "Providers/FlockPlayerProvider.h"
#include "Providers/FlockAssetProvider.h"
#include "Providers/FlockLeaderboardProvider.h"
#include "Providers/FlockNotificationProvider.h"
#include "Providers/FlockShopProvider.h"
#include "FlockSubsystem.generated.h"

class UFlockEvents;

/**
 * The global Flock SDK accessor.
 *
 * A GameInstanceSubsystem is created automatically when the game starts (PIE and packaged) and lives
 * for the whole game session. Fetch it with UFlockSubsystem::Get(WorldContext) or the standard
 * GetGameInstance()->GetSubsystem<UFlockSubsystem>().
 *
 * Initialization is synchronous and needs no network: the Game Version ID is resolved and baked at
 * edit time (Tools > Flock > Resolve Game Version), and runtime init uses it directly.
 *
 * The Authentication provider is wired (with automatic session restore after init), along with the
 * Config, Game, Player, Shop, Commands, and Analytics providers; Asset lands in a later release.
 */
UCLASS()
class FLOCK_API UFlockSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/**
	 * API version segment appended to the API URL for all SDK HTTP calls. Single source of truth —
	 * bump here when the backend cuts a new major API version.
	 */
	static const FString ApiVersion;

	/**
	 * This plugin's SDK version. Keep in sync with Flock.uplugin's VersionName and CHANGELOG.md —
	 * bump all three together. Reported to the backend when an SDK-version request header is added.
	 */
	static const FString SdkVersion;

	/** Returns the Flock subsystem for the given world context, or null if unavailable. */
	UFUNCTION(BlueprintPure, Category = "Flock", meta = (WorldContext = "WorldContextObject"))
	static UFlockSubsystem* Get(const UObject* WorldContextObject);

	//~ Begin USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem interface

	/** Validates the project's UFlockConfig and initializes the SDK from it. No-op (with warning) if already initialized. */
	UFUNCTION(BlueprintCallable, Category = "Flock")
	void InitializeFromSettings();

	/** Initializes the SDK from an explicit config. No-op (with warning) if already initialized; fails cleanly if the Game Version ID is missing. */
	UFUNCTION(BlueprintCallable, Category = "Flock")
	void InitializeWithConfig(const FFlockInitConfig& Config);

	/** Tears down SDK state, allowing a later re-initialization. */
	UFUNCTION(BlueprintCallable, Category = "Flock")
	void ShutdownSdk();

	/**
	 * Injects a custom logger so SDK breadcrumbs/errors flow into your own telemetry or debugger.
	 * C++ only (IFlockLogger is not a UObject).
	 * If never called, a default logger is used (verbose when Enable Debug Logs is on).
	 */
	void SetLogger(const TSharedRef<IFlockLogger>& InLogger);

	/** True once initialization has succeeded. */
	UFUNCTION(BlueprintPure, Category = "Flock")
	bool IsInitialized() const { return bInitialized; }

	/** The last initialization error; empty when none, or after a success. */
	UFUNCTION(BlueprintPure, Category = "Flock")
	FString GetInitializationError() const { return InitializationError; }

	UFUNCTION(BlueprintPure, Category = "Flock")
	FString GetGameId() const { return ActiveConfig.GameId; }

	UFUNCTION(BlueprintPure, Category = "Flock")
	FString GetGameVersionId() const { return ActiveConfig.GameVersionId; }

	UFUNCTION(BlueprintPure, Category = "Flock")
	FString GetApiUrl() const { return ActiveConfig.ApiUrl; }

	/** API base URL with the ApiVersion segment appended (e.g. https://api-flock.qwacks.com/v1). */
	UFUNCTION(BlueprintPure, Category = "Flock")
	FString GetVersionedApiUrl() const;

	/**
	 * The headers every SDK request carries (X-Flock-API-Key and X-Game-Version-ID), exactly as the SDK
	 * initialized with them, for code that calls another Qwacks service on the game's behalf. Empty before
	 * initialization and after shutdown. C++ only, so the API key is not handed to Blueprint.
	 */
	TMap<FString, FString> GetRequestHeaders() const;

	/**
	 * The SDK event hub (lifecycle/auth/session/consent). Bind its events or use its
	 * CallOrRegister entry points; always valid.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock")
	UFlockEvents* GetEvents();

	// ── Authentication ──

	/**
	 * The authentication provider — every login/register/account call lives here. Null before
	 * initialization and after shutdown. C++ API; Blueprint uses the Flock auth async nodes.
	 */
	FFlockAuthProvider* GetAuthProvider() const { return AuthProvider.Get(); }

	/** True when a player is signed in. */
	UFUNCTION(BlueprintPure, Category = "Flock|Auth")
	bool IsAuthenticated() const;

	/** The signed-in player's id from the access-token claims; empty when signed out. */
	UFUNCTION(BlueprintPure, Category = "Flock|Auth")
	FString GetPlayerId() const;

	/** True while a persisted session is being restored (kicked automatically after init). */
	UFUNCTION(BlueprintPure, Category = "Flock|Auth")
	bool IsRestoringSession() const;

	/** Logs the current player out locally; safe when signed out. Server-side revocation is separate (RevokeToken). */
	UFUNCTION(BlueprintCallable, Category = "Flock|Auth")
	void Logout();

	// ── Analytics ──

	/**
	 * The analytics provider. Null before initialization, after shutdown, and when analytics is
	 * switched off in settings. C++ API; Blueprint uses the passthroughs below and the Flock
	 * analytics async nodes.
	 */
	FFlockAnalyticsProvider* GetAnalyticsProvider() const { return AnalyticsProvider.Get(); }

	/**
	 * Records a diagnostic message. Spooled to disk and delivered on the next flush, so it costs
	 * nothing at the call site and survives a crash. Silently ignored without consent.
	 *
	 * Custom data is a map of strings: build it with FFlockMetadata, which converts every value for you.
	 *
	 * Surface: log_event — read on Diagnostics → Events. Not for gameplay: a level completed or an item
	 * bought is Track Analytics Event, which the Game Metrics dashboards read.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Diagnostics", meta = (AutoCreateRefTerm = "ExtraData"))
	void LogDiagnosticEvent(const FString& Message, const TMap<FString, FString>& ExtraData);

	/**
	 * Records a recoverable logic fault. Leave Details at its default if you have nothing to add.
	 *
	 * Surface: log_event — read on Diagnostics → Errors.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Diagnostics", meta = (AutoCreateRefTerm = "Details"))
	void LogDiagnosticError(const FString& Message, const FFlockLogDetails& Details);

	/**
	 * Records an exception. Unhandled engine errors are captured automatically; this is for ones you
	 * report yourself. Leave Stack Trace empty and the SDK captures the callstack for you.
	 *
	 * Surface: log_event — read on Diagnostics → Errors.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Diagnostics", meta = (AutoCreateRefTerm = "Details"))
	void LogDiagnosticException(const FString& Message, const FString& StackTrace,
		const FFlockLogDetails& Details);

	/**
	 * The former name of Log Diagnostic Event, kept so existing graphs and code keep working.
	 *
	 * It was named for the wrong surface: everything it writes is read on Diagnostics → Events, never on the
	 * Game Metrics dashboards, and the name sent people to it for gameplay.
	 *
	 * Surface: log_event — read on Diagnostics → Events.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Diagnostics", meta = (AutoCreateRefTerm = "ExtraData",
		DeprecatedFunction, DeprecationMessage = "Renamed to Log Diagnostic Event, which is the surface it writes to. Track Analytics Event is the one the Game Metrics dashboards read."))
	void LogAnalyticsEvent(const FString& Message, const TMap<FString, FString>& ExtraData);

	/**
	 * The former name of Log Diagnostic Error, kept so existing graphs and code keep working.
	 *
	 * Surface: log_event — read on Diagnostics → Errors.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Diagnostics", meta = (AutoCreateRefTerm = "Details",
		DeprecatedFunction, DeprecationMessage = "Renamed to Log Diagnostic Error, which is the surface it writes to."))
	void LogAnalyticsError(const FString& Message, const FFlockLogDetails& Details);

	/**
	 * The former name of Log Diagnostic Exception, kept so existing graphs and code keep working.
	 *
	 * Surface: log_event — read on Diagnostics → Errors.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Diagnostics", meta = (AutoCreateRefTerm = "Details",
		DeprecatedFunction, DeprecationMessage = "Renamed to Log Diagnostic Exception, which is the surface it writes to."))
	void LogAnalyticsException(const FString& Message, const FString& StackTrace,
		const FFlockLogDetails& Details);

	/**
	 * Counts a screen/menu view against the current session.
	 *
	 * Surface: analytics — read on Dashboards → Game Metrics.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics")
	void RecordAnalyticsScreenView(const FString& ScreenName);

	/**
	 * Records a gameplay event (level_complete, purchase_view) for the Game Metrics dashboards — unlike Log
	 * Analytics Event, which writes a diagnostic entry. Spooled and delivered on the next flush while a player
	 * is signed in; recorded with nobody signed in, it is credited to whoever signs in next. Build Properties
	 * with the Set Command nodes: keys stay verbatim and values keep their type. Returns false when refused —
	 * analytics off, no consent, an empty name, or session_started, which the server records itself when a
	 * session starts.
	 *
	 * Surface: analytics — read on Dashboards → Game Metrics.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics", meta = (AutoCreateRefTerm = "Properties"))
	bool TrackAnalyticsEvent(const FString& EventName, const FFlockCommandData& Properties, const FString& EventCategory);

	/**
	 * What automatic exception capture can see in this build. The engine compiles error log lines and ensures out
	 * of Shipping and Test, so those builds report crashes and Blueprint exceptions only.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock|Diagnostics")
	FFlockExceptionCaptureCoverage GetExceptionCaptureCoverage() const;

	/**
	 * Grants or withdraws analytics consent, persisted across runs. Withdrawing ends the session and
	 * drops anything still queued. Raises OnConsentChanged.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics")
	void SetAnalyticsConsent(bool bGranted);

	UFUNCTION(BlueprintPure, Category = "Flock|Analytics")
	bool HasAnalyticsConsent() const;

	UFUNCTION(BlueprintPure, Category = "Flock|Analytics")
	bool HasActiveAnalyticsSession() const;

	/** The backend's id for the running session; empty until the session start call returns, which OnSessionRegistered announces. */
	UFUNCTION(BlueprintPure, Category = "Flock|Analytics")
	FString GetAnalyticsSessionId() const;

	/** Live metrics for the running session (duration, screens, pauses, FPS). */
	UFUNCTION(BlueprintPure, Category = "Flock|Analytics")
	FFlockSessionSnapshot GetAnalyticsSnapshot() const;

	/** Drops the local spool, the consent decision, and any crash tombstone. */
	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics")
	void EraseLocalAnalyticsData();

	// ── Config & Game ──

	/**
	 * Game config + patches provider. Null before initialization and after shutdown. C++ API; Blueprint
	 * uses the Flock config async nodes and UFlockStructuredDataLibrary.
	 */
	FFlockConfigProvider* GetConfigProvider() const { return ConfigProvider.Get(); }

	/** Game + version info provider. Null before initialization and after shutdown. */
	FFlockGameProvider* GetGameProvider() const { return GameProvider.Get(); }

	// ── Shop ──

	/**
	 * Shop catalog + purchase + inventory provider. Null before initialization and after shutdown.
	 * C++ API; Blueprint uses the Flock shop async nodes.
	 */
	FFlockShopProvider* GetShopProvider() const { return ShopProvider.Get(); }

	// ── Assets ──

	/**
	 * Asset metadata + binary downloads with an on-disk cache. Null before initialization and after
	 * shutdown. C++ API; Blueprint uses the Flock asset async nodes.
	 */
	FFlockAssetProvider* GetAssetProvider() const { return AssetProvider.Get(); }

	// ── Leaderboards ──

	/**
	 * Read-only leaderboard access, addressed by board name. Null before initialization and after
	 * shutdown. C++ API; Blueprint uses the Flock leaderboard async nodes.
	 *
	 * There is no score-submit call: a board projects over a player-data field, so scores move through
	 * GetCommandProvider()'s update calls.
	 */
	FFlockLeaderboardProvider* GetLeaderboardProvider() const { return LeaderboardProvider.Get(); }

	// ── Notifications ──

	/**
	 * The signed-in player's notification inbox — list, unread count, summary, mark read/all-read. Null
	 * before initialization and after shutdown. C++ API; Blueprint uses the Flock notification async nodes.
	 *
	 * Every call here requires a signed-in player and fails Auth without one, because each route answers a
	 * player-keyed schema. Push delivery is a separate concern: this is the in-app half, and it works
	 * whether or not the game ever registers a device token.
	 */
	FFlockNotificationProvider* GetNotificationProvider() const { return NotificationProvider.Get(); }

	// ── Player ──

	/**
	 * Player data + templates + bans provider. Null before initialization and after shutdown. C++ API;
	 * Blueprint uses the Flock player async nodes.
	 */
	FFlockPlayerProvider* GetPlayerProvider() const { return PlayerProvider.Get(); }

	// ── Commands ──

	/**
	 * Game commands (player-data mutations) provider. Null before initialization and after shutdown. C++
	 * API; Blueprint uses the Flock command async nodes and UFlockCommandDataLibrary.
	 */
	FFlockCommandProvider* GetCommandProvider() const { return CommandProvider.Get(); }

	/**
	 * How many offline commands are waiting to be replayed for the signed-in player — for a "syncing…"
	 * indicator. Zero before initialization. Money commands are never queued, so this never counts one.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock|Commands")
	int32 GetPendingCommandCount() const;

	// ── Connectivity ──

	/**
	 * Whether the last SDK request failed to reach the server at all — for an "offline" indicator.
	 *
	 * The HTTP client's offline latch, which only a request that never reached the server sets, and any
	 * completed exchange clears (error statuses included: a 500 took a round trip). Self-healing, so it
	 * can never stick. False before initialization.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock")
	bool IsLikelyOffline() const;

	// ── Test seams (call before initialization; unused by games) ──

	/** Routes SDK HTTP through the given adapter instead of the engine HTTP module. */
	void SetHttpAdapterForTesting(const TSharedPtr<IFlockHttpAdapter>& InAdapter) { TestHttpAdapter = InAdapter; }

	/** Persists tokens through the given store instead of the encrypted file store. */
	void SetTokenStoreForTesting(const TSharedPtr<IFlockTokenStore>& InStore) { TestTokenStore = InStore; }

	/**
	 * Saves every file under the given folder instead of the project's Saved folder: the sign-in, the offline cache, the asset cache,
	 * and the analytics folder with each launch's queues, crash marker and session record, the consent decision and the
	 * coverage notice. A test's SDK is otherwise one more launch of the game: a real launch takes over what the test left
	 * and sends it, and the test takes over, sends or deletes what a real launch left.
	 */
	void SetSavedFilesFolderForTesting(const FString& InFolder) { TestSavedFilesFolder = InFolder; }

private:
	/** Applies the baked-version gate and adopts the config. Returns false with OutError on failure. */
	bool TryInitialize(const FFlockInitConfig& Config, FString& OutError);

	/**
	 * A session needs a player id, so it opens on sign-in rather than at init. Bound to the event hub
	 * only while analytics is live.
	 */
	UFUNCTION()
	void HandleAnalyticsAuthenticated(const FFlockAuthInfo& Info);

	UFUNCTION()
	void HandleAnalyticsLoggedOut();

	/**
	 * Signing in is the third auto-flush trigger (the provider's own pump covers foreground and reconnect):
	 * a queue is player-scoped, so the moment a player is known is the moment their writes can replay.
	 */
	UFUNCTION()
	void HandleCommandsAuthenticated(const FFlockAuthInfo& Info);

	/** The active logger, lazily defaulted from the project's Enable Debug Logs setting when unset. */
	IFlockLogger& GetLogger();

	bool bInitialized = false;
	FString InitializationError;
	FFlockInitConfig ActiveConfig;
	TSharedPtr<IFlockLogger> Logger;

	/** Backing store for GetEvents(); created on first use so events can be bound before init. */
	UPROPERTY(Transient)
	TObjectPtr<UFlockEvents> Events;

	// Auth stack, built per-initialization and dropped on shutdown. Everything is shared-ptr
	// backed; in-flight callbacks hold what they need, so teardown mid-request stays safe.
	TSharedPtr<FFlockHttpClient> HttpClient;
	TSharedPtr<IFlockTokenStore> TokenStore;
	TSharedPtr<FFlockAuthSession> AuthSession;
	TUniquePtr<FFlockAuthProvider> AuthProvider;

	/** Shared, not unique: the provider pins a weak reference to itself across flush continuations. */
	TSharedPtr<FFlockAnalyticsProvider> AnalyticsProvider;

	// Read-side offline cache, shared by the config and game providers. Built before them, pruned to the
	// current game version at init. Null when the offline cache is disabled in settings.
	TSharedPtr<FFlockSnapshotStore> SnapshotStore;

	// Shared, not unique: continuations pin a weak self, matching the analytics provider.
	TSharedPtr<FFlockConfigProvider> ConfigProvider;
	TSharedPtr<FFlockGameProvider> GameProvider;
	TSharedPtr<FFlockShopProvider> ShopProvider;
	TSharedPtr<FFlockAssetProvider> AssetProvider;
	TSharedPtr<FFlockPlayerProvider> PlayerProvider;
	TSharedPtr<FFlockCommandProvider> CommandProvider;
	TSharedPtr<FFlockLeaderboardProvider> LeaderboardProvider;
	TSharedPtr<FFlockNotificationProvider> NotificationProvider;

	TSharedPtr<IFlockHttpAdapter> TestHttpAdapter;
	TSharedPtr<IFlockTokenStore> TestTokenStore;
	FString TestSavedFilesFolder;
};
