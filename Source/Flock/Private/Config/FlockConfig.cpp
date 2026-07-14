// Copyright 2022, Qwack. All Rights Reserved.

#include "Config/FlockConfig.h"

// UFlockConfig is a UDeveloperSettings container; most fields live in the header.
// Registration into Project Settings > Plugins is handled automatically by the
// DeveloperSettings module — no manual ISettingsModule wiring required.

bool UFlockConfig::IsValid(FString& OutError) const
{
	if (ApiUrl.IsEmpty())
	{
		OutError = TEXT("API URL is required.");
		return false;
	}
	if (ApiKey.IsEmpty())
	{
		OutError = TEXT("API Key is required.");
		return false;
	}
	if (GameId.IsEmpty())
	{
		OutError = TEXT("Game Name is required.");
		return false;
	}
	if (GameVersion.IsEmpty())
	{
		OutError = TEXT("Game Version is required.");
		return false;
	}

	OutError.Empty();
	return true;
}
