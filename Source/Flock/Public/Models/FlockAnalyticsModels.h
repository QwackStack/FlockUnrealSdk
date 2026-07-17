// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FlockAnalyticsModels.generated.h"

/**
 * Device facts captured once per session (the wire `device_info` object). Field names map from the
 * snake_case wire via FFlockJsonUtils (operating_system -> OperatingSystem). Populated by the
 * analytics feature when it lands; declared here so FFlockSessionSnapshot is complete.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockDeviceInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Platform;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString OperatingSystem;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString DeviceModel;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString DeviceType;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString AppVersion;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 ScreenWidth = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 ScreenHeight = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	float ScreenDpi = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString SystemLanguage;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString GraphicsDeviceName;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 SystemMemoryMb = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString SdkVersion;
};

/**
 * Final metrics for one gameplay/analytics session (duration, screens, pauses, FPS). Carried by
 * UFlockEvents::OnSessionEnded and persisted for crash recovery once the session feature lands.
 * Dates are raw ISO-8601 strings for now (empty when absent), matching the other wire models.
 *
 * Bool fields have no `b` prefix on purpose: these are wire models, and the snake_case mapping is
 * bijective on the plain name (is_active <-> IsActive); the JSON layer has no per-field overrides yet.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockSessionSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString SessionId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString ServerSessionId;

	/** Lets recovery register a session that never obtained a server id. Empty in older snapshots. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 SessionNumber = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString StartTimeUtc;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString EndTimeUtc;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString LastHeartbeatUtc;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	float DurationSeconds = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	float TotalPauseDurationSeconds = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 PauseCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 ScreensViewed = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FString> ScreenNames;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	float AverageFps = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	float MinFps = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	float MaxFps = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockDeviceInfo DeviceInfo;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool IsActive = false;

	/** True when the session was shorter than the configured bounce threshold. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool IsBounce = false;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool IsFirstSession = false;
};
