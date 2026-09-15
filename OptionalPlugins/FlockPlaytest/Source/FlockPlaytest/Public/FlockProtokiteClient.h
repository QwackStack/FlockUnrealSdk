// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockPlaytestConfig.h"
#include "Http/FlockProviderBase.h"

/**
 * Calls the Protokite API for the playtest plugin, over the Flock SDK's HTTP client, retry handling and error
 * model. Every call carries the Flock SDK's own request headers: Protokite identifies the game and the playtest
 * from the game's API key and version id, and never from a player's sign-in.
 */
class FLOCKPLAYTEST_API FFlockProtokiteClient : public FFlockProviderBase
{
public:
	using FFlockProviderBase::FFlockProviderBase;

	/** The playtest-config address for a Protokite API base URL, with or without trailing slashes. */
	static FString MakePlaytestConfigUrl(const FString& ProtokiteApiUrl);

	/**
	 * Fetches the playtest this build's version is linked to.
	 *
	 * Retried when Protokite or the network fails, never when Protokite refuses (401, 404, 422): those answers
	 * do not change on a second try. Outcomes are not logged here; the caller reports each one at the level
	 * its meaning for playtesting deserves.
	 */
	FFlockRequestHandle FetchPlaytestConfig(const FString& ProtokiteApiUrl, const TMap<FString, FString>& RequestHeaders,
		TFunction<void(TFlockResult<FFlockPlaytestConfig>)> OnComplete);
};
