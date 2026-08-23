// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockHttpAdapter.h"

/**
 * Header-only IFlockHttpAdapter fake for automation tests. Routes by
 * URL fragment, supports one-shot sequences (fail-then-succeed), records every request, and completes
 * synchronously so tests can assert right after the call. Include only from guarded test TUs.
 * Route rule: the last On/OnSequence for a fragment wins.
 */
class FFlockFakeTransport : public IFlockHttpAdapter
{
public:
	/** Route a URL fragment to a single reused response (replaces any prior route for the same fragment). */
	/**
	 * Registers a route, removing any existing one with the same fragment and **appending** the new one.
	 *
	 * The append is load-bearing in both directions, which is why it has not been "fixed":
	 *  - fixtures that re-register a whole table rely on it to *establish* specificity order (the analytics
	 *    fixture's ApplyRoutes does exactly this — re-registering the broad `analytics/sessions` route is
	 *    what moves it behind the per-session ones);
	 *  - but a test that overrides a *single* route silently moves it to the end, behind any broader route
	 *    that also matches — the override becomes unreachable and its assertions pass against the wrong
	 *    fixture.
	 *
	 * So: **override a whole table, never one route of several that can match the same URL.** Fixtures that
	 * need single-route overrides provide a helper that re-registers the table (see the leaderboard fixture's
	 * RouteStandings/RouteRank and the notification fixture's RouteCancel/RouteTemplateByName).
	 */
	FFlockFakeTransport& On(const FString& UrlFragment, const FFlockHttpResponse& Response)
	{
		Routes.RemoveAll([&](const FRoute& R) { return R.Fragment == UrlFragment; });
		FRoute Route;
		Route.Fragment = UrlFragment;
		Route.Sticky = Response;
		Routes.Add(Route);
		return *this;
	}

	/** Route a fragment to a sequence (e.g. fail-then-succeed); the last entry sticks after the queue drains. */
	FFlockFakeTransport& OnSequence(const FString& UrlFragment, const TArray<FFlockHttpResponse>& Responses)
	{
		Routes.RemoveAll([&](const FRoute& R) { return R.Fragment == UrlFragment; });
		FRoute Route;
		Route.Fragment = UrlFragment;
		Route.bHasSequence = true;
		Route.Sequence = Responses;
		Route.Sticky = Responses.Num() > 0 ? Responses.Last() : Ok(TEXT("{}"));
		Routes.Add(Route);
		return *this;
	}

	virtual FFlockRequestHandle SendAsync(const FFlockHttpRequest& Request, TFunction<void(FFlockHttpResponse)> OnComplete) override
	{
		Requests.Add(Request);
		if (!OnComplete)
		{
			return FFlockRequestHandle();
		}
		// Deferred mode records the completion instead of firing it, so a test can issue several calls
		// before any resolves — the only way to exercise in-flight de-duplication with a fake that would
		// otherwise complete synchronously. FlushPending() delivers them. Default (immediate) is unchanged.
		const FFlockHttpResponse Response = Resolve(Request);
		if (bDeferred)
		{
			Pending.Add([OnComplete, Response]() { OnComplete(Response); });
		}
		else
		{
			OnComplete(Response);
		}
		return FFlockRequestHandle();
	}

	/** When true, SendAsync queues completions instead of firing them. Flush with FlushPending(). */
	bool bDeferred = false;

	/** Delivers every queued completion (deferred mode) in order, then clears the queue. */
	void FlushPending()
	{
		TArray<TFunction<void()>> ToRun = MoveTemp(Pending);
		Pending.Reset();
		for (TFunction<void()>& Run : ToRun)
		{
			Run();
		}
	}

	// ── request-log query helpers ──
	int32 CountTo(const FString& UrlFragment) const
	{
		int32 Count = 0;
		for (const FFlockHttpRequest& R : Requests)
		{
			if (R.Url.Contains(UrlFragment)) { ++Count; }
		}
		return Count;
	}

	TArray<FFlockHttpRequest> Requests;

	// ── response builders ──
	static FFlockHttpResponse Ok(const FString& Body)
	{
		FFlockHttpResponse R; R.Result = EFlockHttpResult::Success; R.StatusCode = 200; R.Body = Body; return R;
	}
	static FFlockHttpResponse Status(int32 StatusCode, const FString& Body)
	{
		FFlockHttpResponse R; R.Result = EFlockHttpResult::Success; R.StatusCode = StatusCode; R.Body = Body; return R;
	}
	static FFlockHttpResponse Coded(int32 StatusCode, const FString& Code)
	{
		FFlockHttpResponse R; R.Result = EFlockHttpResult::Success; R.StatusCode = StatusCode;
		R.Body = FString::Printf(TEXT("{\"detail\":{\"code\":\"%s\",\"message\":\"test\"}}"), *Code);
		return R;
	}
	static FFlockHttpResponse Offline()
	{
		FFlockHttpResponse R; R.Result = EFlockHttpResult::ConnectionError; return R;
	}
	static FFlockHttpResponse Timeout()
	{
		FFlockHttpResponse R; R.Result = EFlockHttpResult::Timeout; return R;
	}

private:
	struct FRoute
	{
		FString Fragment;
		bool bHasSequence = false;
		TArray<FFlockHttpResponse> Sequence;
		int32 Cursor = 0;
		FFlockHttpResponse Sticky;
	};

	TArray<FRoute> Routes;
	TArray<TFunction<void()>> Pending;

	/**
	 * First registered fragment the URL contains, matched **case-sensitively**.
	 *
	 * First-match, so **registration order is specificity order** and fixtures register narrow routes before
	 * broad ones. Longest-match is *not* a safe substitute and was tried: `leaderboard/by-name/` is longer
	 * than `/standings`, so preferring length answers a standings read with a board config — exactly
	 * backwards. See `On()` for the override rule that keeps the order meaningful.
	 *
	 * Case-sensitivity is not optional. `FString::Contains` defaults to IgnoreCase, so a board legitimately
	 * named "Medals" makes `leaderboard/by-name/Medals` contain "/me" — and a `/me` route would answer the
	 * board-config request, with nothing in the test looking wrong.
	 */
	FFlockHttpResponse Resolve(const FFlockHttpRequest& Request)
	{
		for (FRoute& Route : Routes)
		{
			if (!Request.Url.Contains(Route.Fragment, ESearchCase::CaseSensitive))
			{
				continue;
			}
			if (Route.bHasSequence && Route.Cursor < Route.Sequence.Num())
			{
				return Route.Sequence[Route.Cursor++];
			}
			return Route.Sticky;
		}
		return Ok(TEXT("{}"));
	}
};
