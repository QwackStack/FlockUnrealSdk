// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "FlockConfig.generated.h"

/**
 * What a schema sync emits.
 *
 * Blueprint is the default because it is the one that costs nothing: assets appear immediately, with no
 * C++ toolchain, no compile, and no editor restart. C++ adds compile-time typing and IntelliSense, and
 * Blueprint still sees the generated types afterwards — but only once the project has been rebuilt and
 * the editor reopened, because a running editor cannot adopt new reflection data.
 */
UENUM()
enum class EFlockCodegenTarget : uint8
{
	/** Blueprint structs, enums, and one-node macros. No compile step. */
	Blueprint UMETA(DisplayName = "Blueprint (assets, no compile)"),

	/** A generated C++ module. Requires a rebuild and an editor restart after each sync. */
	Cpp UMETA(DisplayName = "C++ (generated module, needs a rebuild)"),
};

/**
 * Flock SDK settings for this project.
 *
 * Set your API credentials and game identity under "Required", then adjust the analytics,
 * caching, HTTP, and initialization options as needed. Values are saved to the project's
 * DefaultGame.ini and read by the SDK at runtime.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Flock SDK Settings"))
class FLOCK_API UFlockConfig : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Groups this panel under Project Settings > Plugins. */
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	/**
	 * True when the required credentials/identity fields (API URL, API Key, Game Name, Game Version)
	 * are all present. Does NOT check the baked
	 * GameVersionId — that gate lives in SDK init. On failure, OutError describes the first missing field.
	 */
	bool IsValid(FString& OutError) const;

	// ──────────────────────────────── Required ────────────────────────────────

	/** API endpoint URL. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Required", meta = (DisplayName = "API URL"))
	FString ApiUrl = TEXT("https://api-flock.qwacks.com");

	/** Your Flock API Key. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Required", meta = (DisplayName = "API Key"))
	FString ApiKey;

	/** Your game's name (from the Flock dashboard). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Required", meta = (DisplayName = "Game Name"))
	FString GameId;

	/** Your Game Version name. The matching version ID is resolved from the backend on SDK init. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Required")
	FString GameVersion;

	// ─────────────────────── Resolved (do not edit by hand) ────────────────────

	/**
	 * Game Version ID, resolved from the dashboard at edit time and baked here.
	 * Read-only: set automatically by "Resolve Game Version", not by hand.
	 * Runtime init uses this directly and never contacts the server.
	 */
	UPROPERTY(Config, VisibleAnywhere, BlueprintReadOnly, Category = "Resolved")
	FString GameVersionId;

	// ──────────────────────────────── Codegen ─────────────────────────────────

	/**
	 * Project-relative folder where the Codegen step writes generated code (and Clean wipes).
	 * Created automatically if missing. Treat this folder as Flock-owned — files in it will be
	 * deleted on regen/clean.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Codegen")
	FString GeneratedCodePath = TEXT("Source/FlockGenerated");

	/**
	 * Content folder where the Codegen step writes generated *assets* — the content catalog, and (for a
	 * Blueprint target) the generated structs, enums, and function library. Separate from
	 * GeneratedCodePath because assets have to live under Content to be assets at all, while generated
	 * C++ has to live under Source to be compiled.
	 *
	 * Flock-owned in the same way: regenerating replaces what is in here.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Codegen")
	FString GeneratedContentPath = TEXT("/Game/Flock/Generated");

	/**
	 * What a schema sync generates.
	 *
	 * The two targets overlap — a generated `UFUNCTION` and a generated Blueprint library function both
	 * appear in the action menu under the same name — so this picks one rather than emitting both, and
	 * changing it removes the other target's output on the next sync.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Codegen")
	EFlockCodegenTarget CodegenTarget = EFlockCodegenTarget::Blueprint;

	// ──────────────────────────────── General ─────────────────────────────────

	/** Enable detailed debug logging. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "General")
	bool bEnableDebugLogs = false;

	// ─────────────────────────────── Analytics ────────────────────────────────

	/** Enable analytics tracking. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics")
	bool bAnalyticsEnabled = true;

	/** Automatically start a session on SDK init. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics")
	bool bAnalyticsAutoStartSession = true;

	/** Automatically end the session on app quit. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics")
	bool bAnalyticsAutoEndOnQuit = true;

	/** Background duration (seconds) before a new session starts. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics", meta = (ClampMin = "0.0", Units = "s"))
	float AnalyticsSessionTimeout = 30.f;

	/** Heartbeat interval in seconds (0 to disable). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics", meta = (ClampMin = "0.0", Units = "s"))
	float AnalyticsHeartbeatInterval = 60.f;

	/** Sessions shorter than this are marked as bounces. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics", meta = (ClampMin = "0.0", Units = "s"))
	float AnalyticsBounceThreshold = 10.f;

	/** Persist session to disk for crash recovery. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics")
	bool bAnalyticsPersistSession = true;

	/** Track FPS metrics. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics")
	bool bAnalyticsTrackFps = true;

	/** FPS sample interval in seconds. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics", meta = (ClampMin = "0.0", Units = "s"))
	float AnalyticsFpsSampleInterval = 1.f;

	/**
	 * Require the game to explicitly grant consent before any analytics collection starts.
	 * When OFF (default), analytics collects once authenticated. Turn ON for a GDPR-style opt-in flow.
	 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics")
	bool bAnalyticsRequireExplicitConsent = false;

	// ─────────────────────────── Analytics | Caching ──────────────────────────

	/** Cache failed analytics events on disk and retry on the next session. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics|Caching")
	bool bAnalyticsCacheFailedEvents = true;

	/** Maximum number of failed events kept on disk. Oldest entries are dropped when the cap is hit. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics|Caching", meta = (ClampMin = "0"))
	int32 AnalyticsMaxCachedEvents = 1000;

	/** How many cached events are flushed per batch when retrying. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics|Caching", meta = (ClampMin = "1"))
	int32 AnalyticsCacheFlushBatchSize = 50;

	/** Interval (seconds) for the periodic event-buffer flush. Set to 0 to disable interval-based flushing. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics|Caching", meta = (ClampMin = "0.0", Units = "s"))
	float AnalyticsEventBufferFlushInterval = 10.f;

	// ─────────────────────────────── Asset Cache ──────────────────────────────

	/** Cache asset downloads on disk, keyed by asset ID + UpdatedAt. Disable on WebGL. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Asset Cache")
	bool bEnableAssetCache = true;

	/** Absolute path for the asset cache. Leave empty to default under the project's persistent data path. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Asset Cache")
	FString AssetCacheDirectory;

	/** Maximum size of the on-disk asset cache, in MB. 0 means unlimited; LRU eviction otherwise. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Asset Cache", meta = (ClampMin = "0"))
	int32 AssetCacheMaxSizeMB = 100;

	/** Per-download timeout (seconds) for asset downloads. 0 = no timeout so large assets aren't aborted mid-transfer. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Asset Cache", meta = (ClampMin = "0", Units = "s"))
	int32 AssetDownloadTimeoutSeconds = 0;

	/** Retry attempts for a failed asset download, independent of the HTTP retry policy. 0 disables. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Asset Cache", meta = (ClampMin = "0"))
	int32 AssetDownloadRetryCount = 3;

	// ────────────────────────────── Offline Cache ─────────────────────────────

	/** Snapshot read-API responses to disk and serve them when the network is unavailable. Disable on WebGL. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Offline Cache")
	bool bEnableOfflineCache = true;

	/** Absolute path for snapshot storage. Leave empty to default under the project's persistent data path. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Offline Cache")
	FString OfflineCacheDirectory;

	// ────────────────────────────── HTTP Retry Policy ─────────────────────────

	/** How many times to retry after the initial attempt fails. 0 disables retries. Exponential backoff with jitter. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "HTTP", meta = (ClampMin = "0"))
	int32 RetryMaxRetries = 3;

	/** Adds ±25% randomness to each retry delay to avoid thundering-herd reconnects after an outage. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "HTTP")
	bool bRetryUseJitter = true;

	/** Per-request timeout in seconds for SDK HTTP calls. Caps how long one attempt can hang before failing. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "HTTP", meta = (ClampMin = "0.0", Units = "s"))
	float HttpTimeoutSeconds = 30.f;

	// ────────────────────────────── Initialization ────────────────────────────

	/**
	 * When ON, the SDK initializes itself automatically at startup from these settings.
	 * Leave OFF if you initialize the SDK yourself (e.g. after a splash screen or EULA).
	 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Initialization")
	bool bAutoInitializeOnLoad = true;

	// ──────────────────────────────── Editor ──────────────────────────────────

	/**
	 * When ON, entering Play In Editor with Flock not set up (missing/invalid settings) shows a
	 * fixable notification instead of failing at runtime. Editor-only; no effect in builds.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Editor")
	bool bPlayModeGuardEnabled = true;

	/**
	 * Fail the packaged build if the Game Version ID has not been resolved, so a build that
	 * cannot initialize the SDK can't ship. Editor-only; no effect at runtime.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Editor")
	bool bFailBuildIfVersionUnresolved = true;
};
