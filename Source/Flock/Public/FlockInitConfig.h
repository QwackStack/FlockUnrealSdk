// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FlockInitConfig.generated.h"

class UFlockConfig;

/**
 * Runtime init parameters for the Flock SDK.
 *
 * Built from the project's UFlockConfig via FromSettings(), or filled directly for a manual/test
 * init path. This is the identity/init subset the SDK needs, and it grows as providers land. It exists so the manual init
 * path (and tests) stay decoupled from the UDeveloperSettings object.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockInitConfig
{
	GENERATED_BODY()

	/** API endpoint URL (without the version segment). */
	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString ApiUrl;

	/** Flock API key (game secret). */
	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString ApiKey;

	/** Flock game identifier. */
	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString GameId;

	/** Game Version name. The matching ID is resolved at edit time and baked into GameVersionId. */
	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString GameVersion;

	/** Game Version ID, baked at edit time. Runtime init uses this directly and never contacts the server. */
	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString GameVersionId;

	/** Enable detailed debug logging. */
	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	bool bEnableDebugLogs = false;

	/** Builds a runtime config from the project's UFlockConfig settings. */
	static FFlockInitConfig FromSettings(const UFlockConfig& Settings);

	/**
	 * Base HTTP headers every SDK request carries: X-Flock-API-Key and X-Game-Version-ID. The
	 * Authorization: Bearer header is added by the auth
	 * layer once it lands, not here.
	 */
	TMap<FString, FString> GetBaseHeaders() const;
};
