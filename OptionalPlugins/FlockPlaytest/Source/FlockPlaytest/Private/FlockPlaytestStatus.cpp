// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestStatus.h"

EFlockPlaytestStatus DecidePlaytestStatus(const FFlockPlaytestStatusInputs& Inputs)
{
	if (!Inputs.bPlaytestingEnabled)
	{
		return EFlockPlaytestStatus::TurnedOff;
	}
	if (Inputs.ProtokiteApiUrl.IsEmpty())
	{
		return EFlockPlaytestStatus::ProtokiteApiUrlMissing;
	}
	if (!IsUsableProtokiteApiUrl(Inputs.ProtokiteApiUrl))
	{
		return EFlockPlaytestStatus::ProtokiteApiUrlUnusable;
	}
	if (!Inputs.bFlockInitialized)
	{
		return EFlockPlaytestStatus::WaitingForFlock;
	}
	switch (Inputs.ConfigState)
	{
	case EFlockPlaytestConfigState::Loaded:
		return EFlockPlaytestStatus::Ready;
	case EFlockPlaytestConfigState::PlaytestNotLinked:
		return EFlockPlaytestStatus::PlaytestNotLinked;
	case EFlockPlaytestConfigState::ApiKeyRefused:
		return EFlockPlaytestStatus::ProtokiteRefusedApiKey;
	case EFlockPlaytestConfigState::Unavailable:
		return EFlockPlaytestStatus::PlaytestConfigUnavailable;
	case EFlockPlaytestConfigState::ForAnotherVersion:
		return EFlockPlaytestStatus::PlaytestConfigForAnotherVersion;
	case EFlockPlaytestConfigState::NotFetched:
	case EFlockPlaytestConfigState::Fetching:
		break;
	}
	return EFlockPlaytestStatus::FetchingPlaytestConfig;
}

EFlockPlaytestConfigState DecidePlaytestConfigState(const TFlockResult<FFlockPlaytestConfig>& Result,
	const FString& SentGameVersionId)
{
	if (Result.bSuccess)
	{
		// The server only omits the version when the playtest has none, and then there is nothing to compare.
		// Compared letter for letter, the way the server matches the id it is sent.
		const FString& AnsweredGameVersionId = Result.Value.FlockGameVersionId;
		return !AnsweredGameVersionId.IsEmpty() && !AnsweredGameVersionId.Equals(SentGameVersionId, ESearchCase::CaseSensitive)
			? EFlockPlaytestConfigState::ForAnotherVersion
			: EFlockPlaytestConfigState::Loaded;
	}
	switch (Result.Error.StatusCode)
	{
	case 404:
		return EFlockPlaytestConfigState::PlaytestNotLinked;
	case 401:
	case 422:
		return EFlockPlaytestConfigState::ApiKeyRefused;
	default:
		// Includes 403: Protokite never answers this route with one, so it came from a proxy or firewall on the way
		// and says nothing about the key.
		return EFlockPlaytestConfigState::Unavailable;
	}
}

bool IsUsableProtokiteApiUrl(const FString& Url)
{
	for (const TCHAR Character : Url)
	{
		if (FChar::IsWhitespace(Character))
		{
			return false;
		}
	}

	static const TCHAR* const Schemes[] = { TEXT("https://"), TEXT("http://") };
	for (const TCHAR* Scheme : Schemes)
	{
		if (!Url.StartsWith(Scheme, ESearchCase::IgnoreCase))
		{
			continue;
		}

		// A host has to follow the scheme. "http://" alone, "http:///path" and "http://:8020" name nowhere.
		const int32 SchemeLength = FCString::Strlen(Scheme);
		if (Url.Len() == SchemeLength)
		{
			return false;
		}
		const TCHAR FirstAfterScheme = Url[SchemeLength];
		return FirstAfterScheme != TEXT('/') && FirstAfterScheme != TEXT(':') && FirstAfterScheme != TEXT('?')
			&& FirstAfterScheme != TEXT('#');
	}

	return false;
}

FString DescribePlaytestStatus(EFlockPlaytestStatus Status)
{
	switch (Status)
	{
	case EFlockPlaytestStatus::TurnedOff:
		return TEXT("Playtesting is turned off. Turn on Enable Playtesting in Project Settings > Plugins > Flock Playtest Settings to collect playtest data.");
	case EFlockPlaytestStatus::ProtokiteApiUrlMissing:
		return TEXT("Enable Playtesting is on, but Protokite API URL is empty. Set it in Project Settings > Plugins > Flock Playtest Settings.");
	case EFlockPlaytestStatus::ProtokiteApiUrlUnusable:
		return TEXT("Protokite API URL cannot be used: it must start with http:// or https://, name a host, and contain no spaces or line breaks. Fix it in Project Settings > Plugins > Flock Playtest Settings.");
	case EFlockPlaytestStatus::WaitingForFlock:
		return TEXT("Playtesting is set up and waiting for the Flock SDK to initialize.");
	case EFlockPlaytestStatus::FetchingPlaytestConfig:
		return TEXT("Playtesting is set up and fetching this build's playtest from Protokite.");
	case EFlockPlaytestStatus::PlaytestNotLinked:
		return TEXT("No Protokite playtest is linked to this build's Game Version ID, so playtesting stays off. Point Game Version in Project Settings > Plugins > Flock SDK Settings at the playtest's version (Protokite names it pt-<test id>) and resolve it.");
	case EFlockPlaytestStatus::ProtokiteRefusedApiKey:
		return TEXT("Protokite refused the Flock API key, so playtesting stays off. Check API Key in Project Settings > Plugins > Flock SDK Settings.");
	case EFlockPlaytestStatus::PlaytestConfigUnavailable:
		return TEXT("Could not fetch this build's playtest from Protokite, so playtesting is off for now. The game carries on, and the playtest is fetched again when the next Flock session starts.");
	case EFlockPlaytestStatus::PlaytestConfigForAnotherVersion:
		return TEXT("Protokite answered with the playtest of a different Game Version ID than this build sent, so playtesting stays off. A proxy that drops the X-Game-Version-ID header causes this.");
	case EFlockPlaytestStatus::Ready:
		return TEXT("Playtesting is ready: this build's playtest is loaded.");
	case EFlockPlaytestStatus::Stopped:
		return TEXT("Playtesting has stopped because its game instance shut down.");
	}
	return FString();
}
