// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockProtokiteClient.h"

#include "Http/FlockEndpoints.h"

namespace
{
	/** Every outcome is reported by the playtest subsystem, which knows what it means for playtesting. */
	bool LeaveReportingToTheCaller(const FFlockError&)
	{
		return true;
	}

	/**
	 * Protokite answers a finished end with 204 and no body. The Flock SDK's HTTP client expects a body on every
	 * success, so it reports that answer as an empty one; only this case is turned back into the success it is. A 2xx
	 * with a body that cannot be read still fails.
	 */
	bool IsSuccessWithNoContent(const TFlockResult<FFlockPlaytestSessionEndResult>& Result)
	{
		return !Result.bSuccess && Result.Error.Type == EFlockErrorType::Serialization
			&& Result.Error.StatusCode >= 200 && Result.Error.StatusCode < 300 && Result.Error.Body.IsEmpty();
	}
}

FString FFlockProtokiteClient::JoinProtokiteUrl(const FString& ProtokiteApiUrl, const FString& Route)
{
	FString Base = ProtokiteApiUrl;
	while (Base.EndsWith(TEXT("/")))
	{
		Base.LeftChopInline(1);
	}
	return Base + Route;
}

FString FFlockProtokiteClient::MakePlaytestConfigUrl(const FString& ProtokiteApiUrl)
{
	return JoinProtokiteUrl(ProtokiteApiUrl, TEXT("/game/sdk/playtest-config"));
}

FString FFlockProtokiteClient::MakePlaytestSessionStartUrl(const FString& ProtokiteApiUrl)
{
	return JoinProtokiteUrl(ProtokiteApiUrl, TEXT("/game/sdk/playtest-session"));
}

FString FFlockProtokiteClient::MakePlaytestSessionEndUrl(const FString& ProtokiteApiUrl, const FString& PlaytestSessionId)
{
	return JoinProtokiteUrl(ProtokiteApiUrl,
		FString::Printf(TEXT("/game/sdk/playtest-session/%s/end"), *FlockEndpoints::Encode(PlaytestSessionId)));
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
		/*bAllowAuthRetry*/ false, &LeaveReportingToTheCaller);
}

FString FFlockProtokiteClient::MakeRecordingUploadUrl(const FString& ProtokiteApiUrl, const FString& PlaytestSessionId)
{
	return JoinProtokiteUrl(ProtokiteApiUrl,
		FString::Printf(TEXT("/game/sdk/playtest-session/%s/recording-upload"), *FlockEndpoints::Encode(PlaytestSessionId)));
}

FFlockRequestHandle FFlockProtokiteClient::RequestRecordingUploadLink(const FString& ProtokiteApiUrl,
	const TMap<FString, FString>& RequestHeaders, const FString& PlaytestSessionId, const FString& ContentType,
	TFunction<void(TFlockResult<FFlockPlaytestRecordingUploadLink>)> OnComplete)
{
	const TSharedRef<FFlockHttpClient> HttpClient = Client;
	const FString Url = MakeRecordingUploadUrl(ProtokiteApiUrl, PlaytestSessionId);

	// The route's other two members, has_webcam and has_voice, are left out: nothing here records either, and the
	// server's own defaults say so. content_type is sent even though it matches the default, because the link is
	// signed for whatever is sent and the PUT has to carry the same one.
	const FString Body = FString::Printf(TEXT("{\"content_type\":\"%s\"}"), *ContentType);

	return Execute<FFlockPlaytestRecordingUploadLink>(
		[HttpClient, Url, RequestHeaders, Body](TFunction<void(TFlockResult<FFlockPlaytestRecordingUploadLink>)> Done)
		{
			// Enveloped: the route answers GenericResponse_PlaytestRecordingUploadResponse_, so the link is under
			// result. Read at the root it would parse into an empty link against a real server while every
			// enveloped-looking fixture passed.
			return HttpClient->PostJson<FFlockPlaytestRecordingUploadLink>(Url, RequestHeaders, Body, MoveTemp(Done));
		},
		MoveTemp(OnComplete), TEXT("Playtest recording upload link"), /*bIdempotent*/ true, /*MaxRetriesOverride*/ -1,
		/*bAllowAuthRetry*/ false, &LeaveReportingToTheCaller);
}

FFlockRequestHandle FFlockProtokiteClient::StartPlaytestSession(const FString& ProtokiteApiUrl,
	const TMap<FString, FString>& RequestHeaders, const FFlockPlaytestSessionStartRequest& Request,
	TFunction<void(TFlockResult<FFlockPlaytestSessionStartResult>)> OnComplete)
{
	const TSharedRef<FFlockHttpClient> HttpClient = Client;
	const FString Url = MakePlaytestSessionStartUrl(ProtokiteApiUrl);
	const FString Body = Request.ToJson();
	return Execute<FFlockPlaytestSessionStartResult>(
		[HttpClient, Url, RequestHeaders, Body](TFunction<void(TFlockResult<FFlockPlaytestSessionStartResult>)> Done)
		{
			// The answer's own parse refuses one without a usable session id.
			return HttpClient->PostJson<FFlockPlaytestSessionStartResult>(Url, RequestHeaders, Body, MoveTemp(Done));
		},
		MoveTemp(OnComplete), TEXT("Playtest session start"), /*bIdempotent*/ false, /*MaxRetriesOverride*/ 0,
		/*bAllowAuthRetry*/ false, &LeaveReportingToTheCaller);
}

FFlockRequestHandle FFlockProtokiteClient::EndPlaytestSession(const FString& ProtokiteApiUrl,
	const TMap<FString, FString>& RequestHeaders, const FString& PlaytestSessionId,
	TFunction<void(TFlockResult<FFlockPlaytestSessionEndResult>)> OnComplete)
{
	const TSharedRef<FFlockHttpClient> HttpClient = Client;
	const FString Url = MakePlaytestSessionEndUrl(ProtokiteApiUrl, PlaytestSessionId);
	return Execute<FFlockPlaytestSessionEndResult>(
		[HttpClient, Url, RequestHeaders](TFunction<void(TFlockResult<FFlockPlaytestSessionEndResult>)> Done)
		{
			// The route takes no body; an empty object keeps the POST well formed.
			return HttpClient->PostJsonRaw<FFlockPlaytestSessionEndResult>(Url, RequestHeaders, TEXT("{}"),
				[Done](TFlockResult<FFlockPlaytestSessionEndResult> Result)
				{
					Done(IsSuccessWithNoContent(Result)
						? TFlockResult<FFlockPlaytestSessionEndResult>::Ok(FFlockPlaytestSessionEndResult())
						: Result);
				});
		},
		MoveTemp(OnComplete), TEXT("Playtest session end"), /*bIdempotent*/ true, /*MaxRetriesOverride*/ -1,
		/*bAllowAuthRetry*/ false, &LeaveReportingToTheCaller);
}
