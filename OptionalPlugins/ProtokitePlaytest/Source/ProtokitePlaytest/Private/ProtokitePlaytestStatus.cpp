// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "ProtokitePlaytestStatus.h"

#include "ProtokitePlaytestText.h"

EProtokitePlaytestStatus DecidePlaytestStatus(const FProtokitePlaytestStatusInputs& Inputs)
{
	if (!Inputs.bPlaytestingEnabled)
	{
		return EProtokitePlaytestStatus::TurnedOff;
	}
	if (Inputs.ProtokiteApiUrl.IsEmpty())
	{
		return EProtokitePlaytestStatus::ProtokiteApiUrlMissing;
	}
	if (!IsUsableProtokiteApiUrl(Inputs.ProtokiteApiUrl))
	{
		return EProtokitePlaytestStatus::ProtokiteApiUrlUnusable;
	}
	if (Inputs.bPlaytestNoLongerCollecting)
	{
		return EProtokitePlaytestStatus::PlaytestNoLongerCollecting;
	}
	if (!Inputs.bFlockInitialized)
	{
		return EProtokitePlaytestStatus::WaitingForFlock;
	}
	switch (Inputs.ConfigState)
	{
	case EProtokitePlaytestConfigState::Loaded:
		if (!ProtokitePlaytestConsent::IsAnswered(Inputs.PlayerConsent))
		{
			return EProtokitePlaytestStatus::WaitingForPlayerConsent;
		}
		if (!ProtokitePlaytestConsent::CollectsAnything(Inputs.PlayerConsent))
		{
			return EProtokitePlaytestStatus::PlayerRefusedPlaytest;
		}
		return EProtokitePlaytestStatus::Ready;
	case EProtokitePlaytestConfigState::PlaytestNotLinked:
		return EProtokitePlaytestStatus::PlaytestNotLinked;
	case EProtokitePlaytestConfigState::ApiKeyRefused:
		return EProtokitePlaytestStatus::ProtokiteRefusedApiKey;
	case EProtokitePlaytestConfigState::Unavailable:
		return EProtokitePlaytestStatus::PlaytestConfigUnavailable;
	case EProtokitePlaytestConfigState::ForAnotherVersion:
		return EProtokitePlaytestStatus::PlaytestConfigForAnotherVersion;
	case EProtokitePlaytestConfigState::NotFetched:
	case EProtokitePlaytestConfigState::Fetching:
		break;
	}
	return EProtokitePlaytestStatus::FetchingPlaytestConfig;
}

EProtokitePlaytestConfigState DecidePlaytestConfigState(const TFlockResult<FProtokitePlaytestConfig>& Result,
	const FString& SentGameVersionId)
{
	if (Result.bSuccess)
	{
		// The server only omits the version when the playtest has none, and then there is nothing to compare.
		// Compared letter for letter, the way the server matches the id it is sent.
		const FString& AnsweredGameVersionId = Result.Value.FlockGameVersionId;
		return !AnsweredGameVersionId.IsEmpty() && !AnsweredGameVersionId.Equals(SentGameVersionId, ESearchCase::CaseSensitive)
			? EProtokitePlaytestConfigState::ForAnotherVersion
			: EProtokitePlaytestConfigState::Loaded;
	}
	switch (Result.Error.StatusCode)
	{
	case 404:
		return EProtokitePlaytestConfigState::PlaytestNotLinked;
	case 401:
	case 422:
		return EProtokitePlaytestConfigState::ApiKeyRefused;
	default:
		// Includes 403: Protokite never answers this route with one, so it came from a proxy or firewall on the way
		// and says nothing about the key.
		return EProtokitePlaytestConfigState::Unavailable;
	}
}

bool IsUsableProtokiteApiUrl(const FString& Url)
{
	if (ProtokitePlaytestText::ContainsWhitespace(Url))
	{
		return false;
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

FString DescribePlaytestStatus(EProtokitePlaytestStatus Status)
{
	switch (Status)
	{
	case EProtokitePlaytestStatus::TurnedOff:
		return TEXT("Playtesting is turned off. Turn on Enable Playtesting in Project Settings > Plugins > Protokite Playtest Settings to collect playtest data.");
	case EProtokitePlaytestStatus::ProtokiteApiUrlMissing:
		return TEXT("Enable Playtesting is on, but Protokite API URL is empty. Set it in Project Settings > Plugins > Protokite Playtest Settings.");
	case EProtokitePlaytestStatus::ProtokiteApiUrlUnusable:
		return TEXT("Protokite API URL cannot be used: it must start with http:// or https://, name a host, and contain no spaces or line breaks. Fix it in Project Settings > Plugins > Protokite Playtest Settings.");
	case EProtokitePlaytestStatus::WaitingForFlock:
		return TEXT("Playtesting is set up and waiting for the Flock SDK to initialize.");
	case EProtokitePlaytestStatus::FetchingPlaytestConfig:
		return TEXT("Playtesting is set up and fetching this build's playtest from Protokite.");
	case EProtokitePlaytestStatus::PlaytestNotLinked:
		return TEXT("No Protokite playtest is linked to this build's Game Version ID, so playtesting stays off. Point Game Version in Project Settings > Plugins > Flock SDK Settings at the playtest's version (Protokite names it pt-<test id>) and resolve it.");
	case EProtokitePlaytestStatus::ProtokiteRefusedApiKey:
		return TEXT("Protokite refused the Flock API key, so playtesting stays off. Check API Key in Project Settings > Plugins > Flock SDK Settings.");
	case EProtokitePlaytestStatus::PlaytestConfigUnavailable:
		return TEXT("Could not fetch this build's playtest from Protokite, so playtesting is off for now. The game carries on, and the playtest is fetched again when the next Flock session starts.");
	case EProtokitePlaytestStatus::PlaytestConfigForAnotherVersion:
		return TEXT("Protokite answered with the playtest of a different Game Version ID than this build sent, so playtesting stays off. A proxy that drops the X-Game-Version-ID header causes this.");
	case EProtokitePlaytestStatus::PlaytestNoLongerCollecting:
		return TEXT("This playtest has closed and takes no more sessions (Protokite answered HTTP 400), so playtesting is off until the game is launched again. Reopen the playtest in Protokite, or point Game Version at a playtest that is still running.");
	case EProtokitePlaytestStatus::WaitingForPlayerConsent:
		return TEXT("This build's playtest is loaded, and nothing is collected until the player says what it may collect. The question is put to them once the game has a viewport; a game can ask it itself with Protokite Ask For Playtest Consent, or answer it with Protokite Set Playtest Consent. Turn off Ask The Player For Playtest Consent in Project Settings > Plugins > Protokite Playtest Settings to collect without asking.");
	case EProtokitePlaytestStatus::PlayerRefusedPlaytest:
		return TEXT("The player asked this playtest to collect nothing, so nothing is recorded, nothing is sent and no session is started, exactly as if Enable Playtesting were off. They can be asked again with Protokite Ask For Playtest Consent.");
	case EProtokitePlaytestStatus::Ready:
		return TEXT("Playtesting is ready: this build's playtest is loaded.");
	case EProtokitePlaytestStatus::Stopped:
		return TEXT("Playtesting has stopped because its game instance shut down.");
	}
	return FString();
}
