// Project Settings → Plugins → Flock. Backed by Config/DefaultGame.ini.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "QwackSettings.generated.h"

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Flock"))
class QWACK_UE_SDK_API UQwackSettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
	virtual FName GetSectionName() const override { return FName(TEXT("Flock")); }

	// =========== API Credentials — Required ===========

	/** Base URL for the Flock API. Paths from QwackGameEndpoints are appended. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "API Credentials|Required Settings", meta = (DisplayName = "Api Url"))
	FString ApiUrl = TEXT("https://api-flock.qwacks.com");

	/** Sent as the X-Flock-API-Key header on every request. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "API Credentials|Required Settings", meta = (DisplayName = "Api Key"))
	FString ApiKey;

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "API Credentials|Required Settings", meta = (DisplayName = "Game Id"))
	FString GameId;

	/** Sent as the X-Game-Version-ID header (optional). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "API Credentials|Required Settings", meta = (DisplayName = "Game Version"))
	FString GameVersion;

	// =========== Optional ===========

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Optional|Optional Settings")
	bool EnableDebugLogs = false;

	// =========== Analytics ===========

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics")
	bool AnalyticsEnabled = true;

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics")
	bool AnalyticsAutoStartSession = true;

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics")
	bool AnalyticsAutoEndOnQuit = true;

	/** Seconds of inactivity before a session is considered ended. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics", meta = (ClampMin = "1", UIMin = "1"))
	int32 AnalyticsSessionTimeoutSeconds = 30;

	/** Seconds between session heartbeat pings. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics", meta = (ClampMin = "1", UIMin = "1"))
	int32 AnalyticsHeartbeatIntervalSeconds = 60;

	/** Sessions shorter than this many seconds are flagged as bounces. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics", meta = (ClampMin = "0", UIMin = "0"))
	int32 AnalyticsBounceThresholdSeconds = 10;

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics")
	bool AnalyticsPersistSession = true;

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics")
	bool AnalyticsTrackFps = true;

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics", meta = (ClampMin = "1", UIMin = "1"))
	int32 AnalyticsFpsSampleIntervalSeconds = 1;

	// =========== Analytics → Caching ===========

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics|Caching")
	bool AnalyticsCacheFailedEvents = true;

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics|Caching", meta = (ClampMin = "0", UIMin = "0"))
	int32 AnalyticsMaxCachedEvents = 1000;

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics|Caching", meta = (ClampMin = "1", UIMin = "1"))
	int32 AnalyticsCacheFlushBatchSize = 50;

	// =========== Tools / Links ===========

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Tools|Links")
	FString DocumentationUrl = TEXT("https://docs.qwacks.sa/flock");

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Tools|Links")
	FString SupportUrl = TEXT("https://qwacks.sa/support");
};
