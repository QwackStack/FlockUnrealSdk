// Copyright 2022, Qwack. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "QwackConfig.generated.h"

/**
 * Flock SDK settings, surfaced under Project Settings > Plugins > Flock SDK Settings.
 *
 * Mirrors the developer-facing parameters of the Flock Unity SDK's FlockConfigAsset,
 * adapted to Unreal idioms (UDeveloperSettings + Config=Game). Grouped by category to
 * match Unity's inspector headers. Features backing many of these settings (Analytics,
 * Asset/Offline cache, Codegen) are rebuilt per their own tickets; the parameters live
 * here up front so the config surface stays at parity with Unity.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Flock SDK Settings"))
class QWACK_UE_SDK_API UQwackConfig : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Groups this panel under Project Settings > Plugins. */
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	// ──────────────────────────────── Required ────────────────────────────────

	/** API endpoint URL. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Required", meta = (DisplayName = "API URL"))
	FString ApiUrl = TEXT("https://api-flock.qwacks.com");

	/** Your Flock API Key. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Required", meta = (DisplayName = "API Key"))
	FString ApiKey;

	/** Your game's name (from the Flock dashboard). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Required")
	FString GameId;

	/** Your Game Version name. The matching version ID is resolved from the backend on SDK init. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Required")
	FString GameVersion;

	// ─────────────────────── Resolved (do not edit by hand) ────────────────────

	/**
	 * Game Version ID, resolved from the dashboard at edit time and baked here.
	 * Runtime init uses this directly and never contacts the server.
	 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Resolved")
	FString GameVersionId;

	// ──────────────────────────────── Codegen ─────────────────────────────────

	/**
	 * Project-relative folder where the Codegen step writes generated code (and Clean wipes).
	 * Created automatically if missing. Treat this folder as Flock-owned — files in it will be
	 * deleted on regen/clean.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Codegen")
	FString GeneratedCodePath = TEXT("Source/FlockGenerated");

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
	 * fixable dialog instead of failing at runtime. Editor-only; no effect in builds.
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
