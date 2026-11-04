// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

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

FString FFlockInitConfig::TrimApiUrl(const FString& ApiUrl)
{
	FString Trimmed = ApiUrl.TrimStartAndEnd();
	while (Trimmed.EndsWith(TEXT("/")))
	{
		Trimmed.LeftChopInline(1);
	}
	return Trimmed;
}

bool FFlockInitConfig::IsSchemeAlone(const FString& TrimmedApiUrl)
{
	// A scheme is a letter, then letters, digits, "+", "-" or "." (RFC 3986), and here it is followed by ":" and nothing else.
	if (TrimmedApiUrl.Len() < 2 || !TrimmedApiUrl.EndsWith(TEXT(":")) || !FChar::IsAlpha(TrimmedApiUrl[0]))
	{
		return false;
	}
	for (int32 Index = 1; Index < TrimmedApiUrl.Len() - 1; ++Index)
	{
		const TCHAR Letter = TrimmedApiUrl[Index];
		if (!FChar::IsAlnum(Letter) && Letter != TEXT('+') && Letter != TEXT('-') && Letter != TEXT('.'))
		{
			return false;
		}
	}
	return true;
}

TMap<FString, FString> FFlockInitConfig::GetBaseHeaders() const
{
	TMap<FString, FString> Headers;
	Headers.Add(TEXT("X-Flock-API-Key"), ApiKey);
	Headers.Add(TEXT("X-Game-Version-ID"), GameVersionId);
	return Headers;
}
