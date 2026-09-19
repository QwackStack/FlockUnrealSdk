// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockHttpAdapter.h"

/**
 * A stand-in for the network in the playtest plugin's tests. Answers by URL fragment, records every request,
 * and can hold replies so a test decides when, and in which order, each one lands, or loses them altogether.
 */
class FFlockPlaytestFakeTransport : public IFlockHttpAdapter
{
public:
	/**
	 * Answers every request whose URL contains UrlFragment with Response. When several fragments are part of one URL,
	 * the longest one answers, so a route can be answered separately from a shorter route its address contains.
	 */
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
		const bool bHoldThisOne = bHoldReplies || HoldRepliesToUrlsContaining.ContainsByPredicate(
			[&Request](const FString& Fragment) { return Request.Url.Contains(Fragment); });
		if (bHoldThisOne)
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

	/** Delivers every held reply, including any held while delivering them. */
	void ReleaseAllHeldReplies()
	{
		while (ReleaseOldestHeldReply())
		{
		}
	}

	/** Forgets every held reply without delivering it, the way an answer lost on the way never arrives. */
	void DropAllHeldReplies()
	{
		HeldReplies.Reset();
	}

	/** How many requests went to an address ending with Suffix. */
	int32 CountRequestsEndingWith(const FString& Suffix) const
	{
		int32 Count = 0;
		for (const FFlockHttpRequest& Request : Requests)
		{
			Count += Request.Url.EndsWith(Suffix) ? 1 : 0;
		}
		return Count;
	}

	/** The latest request to an address ending with Suffix; null when there was none. */
	const FFlockHttpRequest* FindLastRequestEndingWith(const FString& Suffix) const
	{
		for (int32 Index = Requests.Num() - 1; Index >= 0; --Index)
		{
			if (Requests[Index].Url.EndsWith(Suffix))
			{
				return &Requests[Index];
			}
		}
		return nullptr;
	}

	static FFlockHttpResponse Status(int32 StatusCode, const FString& Body)
	{
		FFlockHttpResponse Response;
		Response.Result = EFlockHttpResult::Success;
		Response.StatusCode = StatusCode;
		Response.Body = Body;
		return Response;
	}

	/** A 204 with no body, the way Protokite answers a session end. */
	static FFlockHttpResponse NoContent()
	{
		return Status(204, FString());
	}

	static FFlockHttpResponse ConnectionFailure()
	{
		FFlockHttpResponse Response;
		Response.Result = EFlockHttpResult::ConnectionError;
		Response.Body = TEXT("Connection refused");
		return Response;
	}

	static FFlockHttpResponse TimedOut()
	{
		FFlockHttpResponse Response;
		Response.Result = EFlockHttpResult::Timeout;
		return Response;
	}

	/** When true, replies wait until a Release call delivers them, or a Drop call loses them. */
	bool bHoldReplies = false;

	/** Holds only the replies to addresses containing one of these, the way bHoldReplies holds every reply. */
	TArray<FString> HoldRepliesToUrlsContaining;

	/** Every request sent, in order. */
	TArray<FFlockHttpRequest> Requests;

private:
	FFlockHttpResponse Resolve(const FString& Url)
	{
		TArray<FFlockHttpResponse>* Best = nullptr;
		int32 BestLength = -1;
		for (TPair<FString, TArray<FFlockHttpResponse>>& Pair : Answers)
		{
			if (Pair.Value.Num() > 0 && Pair.Key.Len() > BestLength && Url.Contains(Pair.Key))
			{
				Best = &Pair.Value;
				BestLength = Pair.Key.Len();
			}
		}
		if (Best == nullptr)
		{
			return Status(500, TEXT("{\"detail\":\"The fake transport has no answer for this URL\"}"));
		}
		const FFlockHttpResponse Response = (*Best)[0];
		if (Best->Num() > 1)
		{
			Best->RemoveAt(0);
		}
		return Response;
	}

	TMap<FString, TArray<FFlockHttpResponse>> Answers;
	TArray<TFunction<void()>> HeldReplies;
};

/**
 * Protokite bodies for the playtest plugin's tests. The config refusals are copied from the local Protokite API
 * (2026-09-14). The playtest-config answer is shaped from the server's schema and its form serializer, and the
 * session answers from its routes and services (sdk_routes.py, playtest_sessions.py), until a real playtest's
 * answers can be copied instead.
 */
namespace FlockPlaytestFixtures
{
	inline constexpr const TCHAR* PlaytestConfigRoute = TEXT("/game/sdk/playtest-config");
	inline constexpr const TCHAR* PlaytestSessionStartRoute = TEXT("/game/sdk/playtest-session");

	/** Part of every session end's address and of no session start's, so it answers ends only. */
	inline constexpr const TCHAR* PlaytestSessionEndRoute = TEXT("/game/sdk/playtest-session/");

	inline constexpr const TCHAR* TestId = TEXT("01KX0PLAYTEST00000000000000");
	inline constexpr const TCHAR* GameVersionId = TEXT("pt-test-version");

	/** Server ids, 26 characters long like the ones Protokite and the Flock backend hand out. */
	inline constexpr const TCHAR* PlaytestSessionId = TEXT("01KX0PLAYTESTSESSION000000");
	inline constexpr const TCHAR* FirstFlockSessionId = TEXT("01KX0FLOCKSESSION000000001");
	inline constexpr const TCHAR* SecondFlockSessionId = TEXT("01KX0FLOCKSESSION000000002");
	inline constexpr const TCHAR* ThirdFlockSessionId = TEXT("01KX0FLOCKSESSION000000003");

	/** The Flock player the heavy analytics tests sign in, 26 characters long like a Flock player id. */
	inline constexpr const TCHAR* FlockPlayerId = TEXT("01KX0FLOCKPLAYER0000000001");

	inline constexpr const TCHAR* NotLinkedBody = TEXT("{\"detail\":\"No playtest is linked to this Flock SDK version\"}");
	inline constexpr const TCHAR* InvalidApiKeyBody = TEXT("{\"detail\":\"Invalid API Key\"}");
	inline constexpr const TCHAR* MissingApiKeyBody = TEXT("{\"detail\":[{\"type\":\"missing\",\"loc\":[\"header\",\"X-Flock-API-Key\"],\"msg\":\"Field required\",\"input\":null}]}");
	inline constexpr const TCHAR* FlockUnreachableBody = TEXT("{\"detail\":\"Could not reach Flock to verify SDK key\"}");
	inline constexpr const TCHAR* NoLongerCollectingBody = TEXT("{\"detail\":\"This playtest is no longer collecting SDK sessions\"}");
	inline constexpr const TCHAR* NoPlayerIdentityBody = TEXT("{\"detail\":\"Provide steam_id or device_id so the session can be tied to a Flock player\"}");

	/**
	 * A playtest-config answer for the given Flock game version, with three features and a three-question form. Heavy
	 * analytics and video recording are off unless a test turns them on; exception capturing is on unless one turns it off.
	 */
	inline FString ConfigBody(const FString& FlockGameVersionId = GameVersionId, bool bHeavyAnalytics = false, bool bVideoRecording = false,
		bool bExceptionCapturing = true)
	{
		return FString::Printf(TEXT("{\"error\":{\"code\":null},\"response\":{\"message\":null,\"code\":null},\"result\":{")
			TEXT("\"session_started_event\":\"session_started\",\"test_id\":\"%s\",\"flock_game_version_id\":\"%s\",")
			TEXT("\"features\":{\"video_recording\":%s,\"exception_capturing\":%s,\"heavy_analytics\":%s},")
			TEXT("\"form\":{\"id\":\"01KX0FORM000000000000000000\",\"test_id\":\"%s\",\"game_id\":\"01KX0GAME000000000000000000\",")
			TEXT("\"title\":\"Playtest feedback\",\"description\":null,\"is_published\":true,\"fields\":[")
			TEXT("{\"id\":\"rating\",\"type\":\"rating\",\"label\":\"How was it?\",\"required\":true,\"help_text\":null,\"options\":[]},")
			TEXT("{\"id\":\"category\",\"type\":\"select\",\"label\":\"What is this about?\",\"required\":true,\"help_text\":null,\"options\":[\"Bug\",\"Crash\",\"Feedback\",\"Other\"]},")
			TEXT("{\"id\":\"steps\",\"type\":\"textarea\",\"label\":\"Steps to reproduce\",\"required\":false,\"help_text\":\"What were you doing when it happened?\",\"options\":[]}],")
			TEXT("\"created_at\":\"2026-09-14T10:00:00Z\",\"updated_at\":\"2026-09-14T10:00:00Z\"}}}"),
			TestId, *FlockGameVersionId, bVideoRecording ? TEXT("true") : TEXT("false"), bExceptionCapturing ? TEXT("true") : TEXT("false"),
			bHeavyAnalytics ? TEXT("true") : TEXT("false"), TestId);
	}

	/** The envelope around a hand-written result object, for tests that vary one member of an answer. */
	inline FString Envelope(const FString& ResultJson)
	{
		return FString::Printf(TEXT("{\"error\":{\"code\":null},\"response\":{\"message\":null,\"code\":null},\"result\":%s}"), *ResultJson);
	}

	/** A session-start answer naming the given session. */
	inline FString SessionStartBody(const FString& SessionId = PlaytestSessionId)
	{
		return Envelope(FString::Printf(TEXT("{\"session_id\":\"%s\"}"), *SessionId));
	}
}

#endif
