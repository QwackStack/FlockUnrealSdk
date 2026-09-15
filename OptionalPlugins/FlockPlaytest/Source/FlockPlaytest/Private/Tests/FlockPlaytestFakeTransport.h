// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockHttpAdapter.h"

/**
 * A stand-in for the network in the playtest plugin's tests. Answers by URL fragment, records every request,
 * and can hold replies so a test decides when, and in which order, each one lands. Register one fragment per
 * test: when two fragments match the same URL, which one answers is not defined.
 */
class FFlockPlaytestFakeTransport : public IFlockHttpAdapter
{
public:
	/** Answers every request whose URL contains UrlFragment with Response. */
	void Answer(const FString& UrlFragment, const FFlockHttpResponse& Response)
	{
		AnswerInOrder(UrlFragment, { Response });
	}

	/** Answers with each response in turn; the last one keeps answering once the others are used up. */
	void AnswerInOrder(const FString& UrlFragment, const TArray<FFlockHttpResponse>& Responses)
	{
		Answers.FindOrAdd(UrlFragment) = Responses;
	}

	virtual FFlockRequestHandle SendAsync(const FFlockHttpRequest& Request, TFunction<void(FFlockHttpResponse)> OnComplete) override
	{
		Requests.Add(Request);
		const FFlockHttpResponse Response = Resolve(Request.Url);
		if (bHoldReplies)
		{
			HeldReplies.Add([OnComplete, Response]()
			{
				if (OnComplete)
				{
					OnComplete(Response);
				}
			});
		}
		else if (OnComplete)
		{
			OnComplete(Response);
		}
		return FFlockRequestHandle();
	}

	/** Delivers the oldest held reply. Returns false when none was held. */
	bool ReleaseOldestHeldReply()
	{
		if (HeldReplies.Num() == 0)
		{
			return false;
		}
		const TFunction<void()> Reply = HeldReplies[0];
		HeldReplies.RemoveAt(0);
		Reply();
		return true;
	}

	void ReleaseAllHeldReplies()
	{
		while (ReleaseOldestHeldReply())
		{
		}
	}

	static FFlockHttpResponse Status(int32 StatusCode, const FString& Body)
	{
		FFlockHttpResponse Response;
		Response.Result = EFlockHttpResult::Success;
		Response.StatusCode = StatusCode;
		Response.Body = Body;
		return Response;
	}

	static FFlockHttpResponse ConnectionFailure()
	{
		FFlockHttpResponse Response;
		Response.Result = EFlockHttpResult::ConnectionError;
		Response.Body = TEXT("Connection refused");
		return Response;
	}

	/** When true, replies wait until a Release call delivers them. */
	bool bHoldReplies = false;

	/** Every request sent, in order. */
	TArray<FFlockHttpRequest> Requests;

private:
	FFlockHttpResponse Resolve(const FString& Url)
	{
		for (TPair<FString, TArray<FFlockHttpResponse>>& Pair : Answers)
		{
			if (Url.Contains(Pair.Key) && Pair.Value.Num() > 0)
			{
				const FFlockHttpResponse Response = Pair.Value[0];
				if (Pair.Value.Num() > 1)
				{
					Pair.Value.RemoveAt(0);
				}
				return Response;
			}
		}
		return Status(500, TEXT("{\"detail\":\"The fake transport has no answer for this URL\"}"));
	}

	TMap<FString, TArray<FFlockHttpResponse>> Answers;
	TArray<TFunction<void()>> HeldReplies;
};

/**
 * Protokite bodies for the playtest plugin's tests. The refusals are copied from the local Protokite API
 * (2026-09-14). The playtest-config answer is shaped from the server's schema and its form serializer until a
 * real playtest's answer can be copied instead.
 */
namespace FlockPlaytestFixtures
{
	inline constexpr const TCHAR* PlaytestConfigRoute = TEXT("/game/sdk/playtest-config");
	inline constexpr const TCHAR* TestId = TEXT("01KX0PLAYTEST00000000000000");
	inline constexpr const TCHAR* GameVersionId = TEXT("pt-test-version");

	inline constexpr const TCHAR* NotLinkedBody = TEXT("{\"detail\":\"No playtest is linked to this Flock SDK version\"}");
	inline constexpr const TCHAR* InvalidApiKeyBody = TEXT("{\"detail\":\"Invalid API Key\"}");
	inline constexpr const TCHAR* MissingApiKeyBody = TEXT("{\"detail\":[{\"type\":\"missing\",\"loc\":[\"header\",\"X-Flock-API-Key\"],\"msg\":\"Field required\",\"input\":null}]}");
	inline constexpr const TCHAR* FlockUnreachableBody = TEXT("{\"detail\":\"Could not reach Flock to verify SDK key\"}");

	/** A playtest-config answer for the given Flock game version, with three features and a three-question form. */
	inline FString ConfigBody(const FString& FlockGameVersionId = GameVersionId)
	{
		return FString::Printf(TEXT("{\"error\":{\"code\":null},\"response\":{\"message\":null,\"code\":null},\"result\":{")
			TEXT("\"session_started_event\":\"session_started\",\"test_id\":\"%s\",\"flock_game_version_id\":\"%s\",")
			TEXT("\"features\":{\"video_recording\":true,\"exception_capturing\":true,\"heavy_analytics\":false},")
			TEXT("\"form\":{\"id\":\"01KX0FORM000000000000000000\",\"test_id\":\"%s\",\"game_id\":\"01KX0GAME000000000000000000\",")
			TEXT("\"title\":\"Playtest feedback\",\"description\":null,\"is_published\":true,\"fields\":[")
			TEXT("{\"id\":\"rating\",\"type\":\"rating\",\"label\":\"How was it?\",\"required\":true,\"help_text\":null,\"options\":[]},")
			TEXT("{\"id\":\"category\",\"type\":\"select\",\"label\":\"What is this about?\",\"required\":true,\"help_text\":null,\"options\":[\"Bug\",\"Crash\",\"Feedback\",\"Other\"]},")
			TEXT("{\"id\":\"steps\",\"type\":\"textarea\",\"label\":\"Steps to reproduce\",\"required\":false,\"help_text\":\"What were you doing when it happened?\",\"options\":[]}],")
			TEXT("\"created_at\":\"2026-09-14T10:00:00Z\",\"updated_at\":\"2026-09-14T10:00:00Z\"}}}"),
			TestId, *FlockGameVersionId, TestId);
	}

	/** The envelope around a hand-written result object, for tests that vary one member of the config. */
	inline FString Envelope(const FString& ResultJson)
	{
		return FString::Printf(TEXT("{\"error\":{\"code\":null},\"response\":{\"message\":null,\"code\":null},\"result\":%s}"), *ResultJson);
	}
}

#endif
