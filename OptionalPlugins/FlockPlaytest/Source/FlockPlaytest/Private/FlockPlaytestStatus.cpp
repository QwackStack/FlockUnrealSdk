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
	return EFlockPlaytestStatus::Ready;
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
	case EFlockPlaytestStatus::Ready:
		return TEXT("Playtesting is set up and the Flock SDK is initialized.");
	case EFlockPlaytestStatus::Stopped:
		return TEXT("Playtesting has stopped because its game instance shut down.");
	}
	return FString();
}
