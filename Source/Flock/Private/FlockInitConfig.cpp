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
