// Copyright 2022, Qwacks. All Rights Reserved.

#include "FlockInitConfig.h"
#include "Config/FlockConfig.h"

FFlockInitConfig FFlockInitConfig::FromSettings(const UFlockConfig& Settings)
{
	FFlockInitConfig Config;
	Config.ApiUrl = Settings.ApiUrl;
	Config.ApiKey = Settings.ApiKey;
	Config.GameId = Settings.GameId;
	Config.GameVersion = Settings.GameVersion;
	Config.GameVersionId = Settings.GameVersionId;
	Config.bEnableDebugLogs = Settings.bEnableDebugLogs;
	return Config;
}

TMap<FString, FString> FFlockInitConfig::GetBaseHeaders() const
{
	TMap<FString, FString> Headers;
	Headers.Add(TEXT("X-Flock-API-Key"), ApiKey);
	Headers.Add(TEXT("X-Game-Version-ID"), GameVersionId);
	return Headers;
}
