// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockProtokiteClient.h"

FString FFlockProtokiteClient::MakePlaytestConfigUrl(const FString& ProtokiteApiUrl)
{
	FString Base = ProtokiteApiUrl;
	// Only trailing slashes are removed: the setting is refused when it holds whitespace, so nothing else
	// needs trimming.
	while (Base.EndsWith(TEXT("/")))
	{
		Base.LeftChopInline(1);
	}
	return Base + TEXT("/game/sdk/playtest-config");
}

FFlockRequestHandle FFlockProtokiteClient::FetchPlaytestConfig(const FString& ProtokiteApiUrl,
	const TMap<FString, FString>& RequestHeaders, TFunction<void(TFlockResult<FFlockPlaytestConfig>)> OnComplete)
{
	const TSharedRef<FFlockHttpClient> HttpClient = Client;
	const FString Url = MakePlaytestConfigUrl(ProtokiteApiUrl);
	return Execute<FFlockPlaytestConfig>(
		[HttpClient, Url, RequestHeaders](TFunction<void(TFlockResult<FFlockPlaytestConfig>)> Done)
		{
			return HttpClient->Get<FFlockPlaytestConfig>(Url, RequestHeaders, MoveTemp(Done));
		},
		MoveTemp(OnComplete), TEXT("Playtest config"), /*bIdempotent*/ true, /*MaxRetriesOverride*/ -1,
		/*bAllowAuthRetry*/ false,
		// Every outcome is reported by the playtest subsystem, which knows what it means for playtesting.
		[](const FFlockError&) { return true; });
}
