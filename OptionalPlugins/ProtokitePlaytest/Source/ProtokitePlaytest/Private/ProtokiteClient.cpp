// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "ProtokiteClient.h"

#include "Http/FlockEndpoints.h"
#include "Http/FlockErrorHints.h"

namespace
{
	/** Every outcome is reported by the playtest subsystem, which knows what it means for playtesting. */
	bool LeaveReportingToTheCaller(const FFlockError&)
	{
		return true;
	}

	const TCHAR* const ProtokiteApiUrlSetting =
		TEXT("Protokite API URL in Project Settings > Plugins > Protokite Playtest Settings must be the Protokite server's address, such as https://api-protokite.qwacks.com.");

	/** Hands each attempt's failure over with Protokite's fix, before the retry handling and the caller read it. */
	template <typename T>
	TFunction<void(TFlockResult<T>)> WithProtokiteFix(TFunction<void(TFlockResult<T>)> Done)
	{
		return [Done = MoveTemp(Done)](TFlockResult<T> Result)
		{
			if (!Result.bSuccess)
			{
				Result.Error.Hint = FProtokiteClient::FixFor(Result.Error);
			}
			Done(MoveTemp(Result));
		};
	}
}

FString FProtokiteClient::FixFor(const FFlockError& Error)
{
	const FString NotProtokitesAnswer = FString(TEXT("The answer did not come from Protokite's API: a captive portal (a Wi-Fi sign-in page), a proxy, or a Protokite API URL that points at another server answers like this. Sign in to the network if it asks, and check the address. ")) + ProtokiteApiUrlSetting;
	switch (Error.Type)
	{
	case EFlockErrorType::Cancelled:
		return TEXT("Nothing needs fixing: the launch ended, or playtesting stopped, before Protokite answered.");
	case EFlockErrorType::Timeout:
	case EFlockErrorType::Connection:
		return FString(TEXT("Check the device is online and can reach Protokite: a firewall or a VPN stops every call, and a slow network leaves calls unanswered. ")) + ProtokiteApiUrlSetting;
	case EFlockErrorType::Serialization:
		// With a status Protokite's address answered something unreadable; with none the plugin could not build its request.
		return Error.StatusCode > 0 ? NotProtokitesAnswer : FFlockErrorHints::ForFailure(Error);
	default:
		break;
	}
	if (Error.StatusCode == 0)
	{
		return FFlockErrorHints::ForFailure(Error);
	}
	if (Error.StatusCode >= 500)
	{
		return TEXT("Protokite, or Flock behind it, failed while handling the request; the game did not cause it. Try again later: a feedback form that was not sent is kept for a later launch.");
	}
	if (Error.StatusCode == 408 || Error.StatusCode == 429)
	{
		return TEXT("Something between the game and Protokite is asking it to slow down. Wait a moment and try again.");
	}
	// Protokite gives a reason with every refusal, so one without came from something else on the way.
	if (Error.ServerMessage.IsEmpty())
	{
		return NotProtokitesAnswer;
	}
	switch (Error.StatusCode)
	{
	case 400:
		return TEXT("Protokite refused the request; the reason above is its own. A playtest that is no longer collecting sessions has closed, and nothing in the build reopens it: start a new playtest in the Protokite dashboard if more sessions are needed.");
	case 401:
		return TEXT("Protokite did not accept this build's Flock API key. Set API Key under Project Settings > Flock SDK to the key, from the Flock dashboard, of the game the playtest belongs to.");
	case 403:
		return TEXT("Protokite holds this playtest session for another game, so this build may not change it. Check API Key under Project Settings > Flock SDK is the key of the game the playtest belongs to; the next launch starts a session of its own.");
	case 404:
		return TEXT("Protokite has nothing for this request; the reason above says what. No playtest linked to this Flock SDK version: link the playtest in the Protokite dashboard to the Game Version this build runs (Game Version under Project Settings > Flock SDK), or build with the version the playtest names. No published feedback form: publish the playtest's form in the Protokite dashboard. Playtest session not found: the session this was for is gone, and the next launch starts another.");
	case 422:
		return TEXT("Protokite refused a value the plugin sent; the reason above names it. A form answer it names does not fit the form as published now: the next launch fetches the form again. A missing steam_id or device_id means this device gave the player no id to send.");
	default:
		return TEXT("Protokite refused the request; the reason above is its own.");
	}
}

FString FProtokiteClient::JoinProtokiteUrl(const FString& ProtokiteApiUrl, const FString& Route)
{
	FString Base = ProtokiteApiUrl;
	while (Base.EndsWith(TEXT("/")))
	{
		Base.LeftChopInline(1);
	}
	return Base + Route;
}

FString FProtokiteClient::MakePlaytestConfigUrl(const FString& ProtokiteApiUrl)
{
	return JoinProtokiteUrl(ProtokiteApiUrl, TEXT("/game/sdk/playtest-config"));
}

FString FProtokiteClient::MakePlaytestSessionStartUrl(const FString& ProtokiteApiUrl)
{
	return JoinProtokiteUrl(ProtokiteApiUrl, TEXT("/game/sdk/playtest-session"));
}

FString FProtokiteClient::MakePlaytestSessionEndUrl(const FString& ProtokiteApiUrl, const FString& PlaytestSessionId)
{
	return JoinProtokiteUrl(ProtokiteApiUrl,
		FString::Printf(TEXT("/game/sdk/playtest-session/%s/end"), *FlockEndpoints::Encode(PlaytestSessionId)));
}

FFlockRequestHandle FProtokiteClient::FetchPlaytestConfig(const FString& ProtokiteApiUrl,
	const TMap<FString, FString>& RequestHeaders, TFunction<void(TFlockResult<FProtokitePlaytestConfig>)> OnComplete)
{
	const TSharedRef<FFlockHttpClient> HttpClient = Client;
	const FString Url = MakePlaytestConfigUrl(ProtokiteApiUrl);
	return Execute<FProtokitePlaytestConfig>(
		[HttpClient, Url, RequestHeaders](TFunction<void(TFlockResult<FProtokitePlaytestConfig>)> Done)
		{
			return HttpClient->Get<FProtokitePlaytestConfig>(Url, RequestHeaders, WithProtokiteFix(MoveTemp(Done)));
		},
		MoveTemp(OnComplete), TEXT("Playtest config"), /*bIdempotent*/ true, /*MaxRetriesOverride*/ -1,
		/*bAllowAuthRetry*/ false, &LeaveReportingToTheCaller);
}

FString FProtokiteClient::MakeFeedbackFormUrl(const FString& ProtokiteApiUrl)
{
	return JoinProtokiteUrl(ProtokiteApiUrl, TEXT("/game/sdk/feedback-form"));
}

FFlockRequestHandle FProtokiteClient::SubmitFeedbackForm(const TMap<FString, FString>& RequestHeaders,
	const FProtokitePlaytestFormSubmission& Submission,
	TFunction<void(TFlockResult<FProtokitePlaytestFormSubmitResult>)> OnComplete)
{
	const TSharedRef<FFlockHttpClient> HttpClient = Client;
	const FString Url = MakeFeedbackFormUrl(Submission.ProtokiteApiUrl);
	const FString Body = Submission.ToJson();
	const bool bSafeToSendAgain = Submission.CanBeSentAgainSafely();

	return Execute<FProtokitePlaytestFormSubmitResult>(
		[HttpClient, Url, RequestHeaders, Body](TFunction<void(TFlockResult<FProtokitePlaytestFormSubmitResult>)> Done)
		{
			// Enveloped: the route answers GenericResponse[PlaytestFormResponseSchema].
			return HttpClient->PostJson<FProtokitePlaytestFormSubmitResult>(Url, RequestHeaders, Body, WithProtokiteFix(MoveTemp(Done)));
		},
		MoveTemp(OnComplete), TEXT("Playtest feedback form"), bSafeToSendAgain,
		/*MaxRetriesOverride*/ bSafeToSendAgain ? -1 : 0, /*bAllowAuthRetry*/ false, &LeaveReportingToTheCaller);
}

FString FProtokiteClient::MakeRecordingUploadUrl(const FString& ProtokiteApiUrl, const FString& PlaytestSessionId)
{
	return JoinProtokiteUrl(ProtokiteApiUrl,
		FString::Printf(TEXT("/game/sdk/playtest-session/%s/recording-upload"), *FlockEndpoints::Encode(PlaytestSessionId)));
}

FFlockRequestHandle FProtokiteClient::RequestRecordingUploadLink(const FString& ProtokiteApiUrl,
	const TMap<FString, FString>& RequestHeaders, const FString& PlaytestSessionId, const FString& ContentType,
	TFunction<void(TFlockResult<FProtokitePlaytestRecordingUploadLink>)> OnComplete)
{
	const TSharedRef<FFlockHttpClient> HttpClient = Client;
	const FString Url = MakeRecordingUploadUrl(ProtokiteApiUrl, PlaytestSessionId);

	// The route's other two members, has_webcam and has_voice, are left out: nothing here records either, and the
	// server's own defaults say so. content_type is sent even though it matches the default, because the link is
	// signed for whatever is sent and the PUT has to carry the same one.
	const FString Body = FString::Printf(TEXT("{\"content_type\":\"%s\"}"), *ContentType);

	return Execute<FProtokitePlaytestRecordingUploadLink>(
		[HttpClient, Url, RequestHeaders, Body](TFunction<void(TFlockResult<FProtokitePlaytestRecordingUploadLink>)> Done)
		{
			// Enveloped: the route answers GenericResponse_PlaytestRecordingUploadResponse_, so the link is under
			// result. Read at the root it would parse into an empty link against a real server while every
			// enveloped-looking fixture passed.
			return HttpClient->PostJson<FProtokitePlaytestRecordingUploadLink>(Url, RequestHeaders, Body, WithProtokiteFix(MoveTemp(Done)));
		},
		MoveTemp(OnComplete), TEXT("Playtest recording upload link"), /*bIdempotent*/ true, /*MaxRetriesOverride*/ -1,
		/*bAllowAuthRetry*/ false, &LeaveReportingToTheCaller);
}

FFlockRequestHandle FProtokiteClient::StartPlaytestSession(const FString& ProtokiteApiUrl,
	const TMap<FString, FString>& RequestHeaders, const FProtokitePlaytestSessionStartRequest& Request,
	TFunction<void(TFlockResult<FProtokitePlaytestSessionStartResult>)> OnComplete)
{
	const TSharedRef<FFlockHttpClient> HttpClient = Client;
	const FString Url = MakePlaytestSessionStartUrl(ProtokiteApiUrl);
	const FString Body = Request.ToJson();
	return Execute<FProtokitePlaytestSessionStartResult>(
		[HttpClient, Url, RequestHeaders, Body](TFunction<void(TFlockResult<FProtokitePlaytestSessionStartResult>)> Done)
		{
			// The answer's own parse refuses one without a usable session id.
			return HttpClient->PostJson<FProtokitePlaytestSessionStartResult>(Url, RequestHeaders, Body, WithProtokiteFix(MoveTemp(Done)));
		},
		MoveTemp(OnComplete), TEXT("Playtest session start"), /*bIdempotent*/ false, /*MaxRetriesOverride*/ 0,
		/*bAllowAuthRetry*/ false, &LeaveReportingToTheCaller);
}

FFlockRequestHandle FProtokiteClient::EndPlaytestSession(const FString& ProtokiteApiUrl,
	const TMap<FString, FString>& RequestHeaders, const FString& PlaytestSessionId,
	TFunction<void(TFlockResult<FProtokitePlaytestSessionEndResult>)> OnComplete)
{
	const TSharedRef<FFlockHttpClient> HttpClient = Client;
	const FString Url = MakePlaytestSessionEndUrl(ProtokiteApiUrl, PlaytestSessionId);
	return Execute<FProtokitePlaytestSessionEndResult>(
		[HttpClient, Url, RequestHeaders](TFunction<void(TFlockResult<FProtokitePlaytestSessionEndResult>)> Done)
		{
			// The route takes no body; an empty object keeps the POST well formed. Protokite answers a finished
			// end with 204 and no body.
			return HttpClient->PostJsonAcceptingNoContent<FProtokitePlaytestSessionEndResult>(Url, RequestHeaders, TEXT("{}"),
				WithProtokiteFix(MoveTemp(Done)));
		},
		MoveTemp(OnComplete), TEXT("Playtest session end"), /*bIdempotent*/ true, /*MaxRetriesOverride*/ -1,
		/*bAllowAuthRetry*/ false, &LeaveReportingToTheCaller);
}
