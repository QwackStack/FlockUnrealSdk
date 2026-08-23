// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "FlockLogger.h"
#include "HAL/FileManager.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockSnapshotStore.h"
#include "Misc/Base64.h"
#include "Misc/Paths.h"
#include "Providers/FlockLeaderboardProvider.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"

namespace FlockLeaderboardProviderTestHelpers
{
	inline FFlockRetryPolicy NoRetry()
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = 0;
		return Policy;
	}

	inline FString MakeTestJwt(const FString& PlayerId)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + 3600;
		FString Payload = FBase64::Encode(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp));
		Payload.ReplaceInline(TEXT("+"), TEXT("-"));
		Payload.ReplaceInline(TEXT("/"), TEXT("_"));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Payload);
	}

	inline FString TempRoot()
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("lb_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	// ── Real wire shapes. All four leaderboard routes are enveloped (GenericResponse_X_), so every
	// fixture here carries {error,response,result}. A bare fixture would pass against the raw verbs and
	// prove nothing about the ones the provider actually uses. ──

	inline FString Env(const FString& Inner)
	{
		return FString::Printf(TEXT("{\"error\":null,\"response\":null,\"result\":%s}"), *Inner);
	}

	inline FString BoardBody(const FString& Id = TEXT("lb-1"))
	{
		return Env(FString::Printf(
			TEXT("{\"id\":\"%s\",\"name\":\"HighScoreTest\",\"value_type\":\"integer\",\"direction\":\"higher\",")
			TEXT("\"aggregation\":\"best\",\"window_type\":\"never\",\"scope\":\"global\"}"), *Id));
	}

	inline FString StandingsBody()
	{
		return Env(
			TEXT("{\"window\":\"all\",\"total\":2,\"items\":[")
			TEXT("{\"rank\":1,\"player_id\":\"p1\",\"player_name\":\"Ada\",\"score\":10,\"country\":\"SA\",\"achieved_at\":\"\"}")
			TEXT("]}"));
	}

	inline FString RankBody(int32 Rank = 3)
	{
		return Env(FString::Printf(
			TEXT("{\"player_id\":\"p1\",\"window\":\"all\",\"rank\":%d,\"score\":42}"), Rank));
	}

	struct FFixture
	{
		FString Dir;
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockAuthSession> Session;
		TSharedPtr<FFlockSnapshotStore> Snapshot;
		TSharedPtr<FFlockLeaderboardProvider> Provider;

		explicit FFixture(const FString& ExistingDir = FString())
			: Dir(ExistingDir.IsEmpty() ? TempRoot() : ExistingDir)
			, Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
		{
			Snapshot = MakeShared<FFlockSnapshotStore>(Dir, MakeShared<FFlockNullLogger>(), TEXT("9.9.9"));
			Provider = MakeShared<FFlockLeaderboardProvider>(Client, NoRetry(), MakeShared<FFlockNullLogger>(),
				Session, TEXT("http://x/v1"), Snapshot, TEXT("ver-1"));
			RouteAll();
		}

		/**
		 * The four responses in play, held as fixture state rather than registered ad hoc.
		 *
		 * All four live routes hang off `leaderboard/by-name/{name}`, so the bare board-config fragment is
		 * a **prefix of the other three**. The fake answers the first route whose fragment the URL contains
		 * and `On()` removes-then-**appends**, so any test that overrides one route with a bare `On()`
		 * silently drops it behind the board-config route — and a standings read comes back answered by a
		 * board config. Green, and proving nothing.
		 *
		 * Owning the table here makes that unreachable: every override reassigns one of these and re-runs
		 * RouteAll(), which always registers most-specific-first.
		 */
		FFlockHttpResponse BoardResponse = FFlockFakeTransport::Ok(BoardBody());
		FFlockHttpResponse StandingsResponse = FFlockFakeTransport::Ok(StandingsBody());
		FFlockHttpResponse RankResponse = FFlockFakeTransport::Ok(RankBody());
		FFlockHttpResponse AroundResponse = FFlockFakeTransport::Ok(StandingsBody());

		/**
		 * Re-registers all four in specificity order. Also the assertion that routing is by name: no
		 * fragment in this file mentions a board id, because no URL the provider builds contains one.
		 */
		void RouteAll()
		{
			Fake->On(TEXT("/around-me"), AroundResponse);
			Fake->On(TEXT("/me"), RankResponse);
			Fake->On(TEXT("/standings"), StandingsResponse);
			Fake->On(TEXT("leaderboard/by-name/"), BoardResponse);
		}

		void RouteBoard(const FFlockHttpResponse& R) { BoardResponse = R; RouteAll(); }
		void RouteStandings(const FFlockHttpResponse& R) { StandingsResponse = R; RouteAll(); }
		void RouteRank(const FFlockHttpResponse& R) { RankResponse = R; RouteAll(); }
		void RouteAround(const FFlockHttpResponse& R) { AroundResponse = R; RouteAll(); }

		void SignIn(const FString& PlayerId = TEXT("player-a"))
		{
			FString Error;
			Session->SetTokens(MakeTestJwt(PlayerId), TEXT("r-1"), Error);
		}

		/**
		 * The real offline state: the probe reports unreachable *and* every route fails to connect. In
		 * production the probe is driven by the HTTP client's offline latch, which only latches because
		 * requests came back with a connection error — so a false probe over a working transport is not a
		 * state that can occur, and testing it would prove nothing.
		 *
		 * All four responses are swapped and the table re-registered in one go — see RouteAll() for why
		 * patching a single route with a bare On() is a trap.
		 */
		void GoOffline()
		{
			Provider->SetReachabilityProbe([]() { return false; });
			BoardResponse = FFlockFakeTransport::Offline();
			StandingsResponse = FFlockFakeTransport::Offline();
			RankResponse = FFlockFakeTransport::Offline();
			AroundResponse = FFlockFakeTransport::Offline();
			RouteAll();
		}

		/** Offline for the cache-serving branch only: the probe says no, but nothing should reach the fake. */
		void GoOfflineExpectingNoCalls() { Provider->SetReachabilityProbe([]() { return false; }); }

		/** The URL of the last request whose path contains Fragment, for asserting query strings. */
		FString LastUrlContaining(const FString& Fragment) const
		{
			for (int32 Index = Fake->Requests.Num() - 1; Index >= 0; --Index)
			{
				if (Fake->Requests[Index].Url.Contains(Fragment))
				{
					return Fake->Requests[Index].Url;
				}
			}
			return FString();
		}
	};

	inline void Cleanup(const FString& Dir)
	{
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
	}
}

using namespace FlockLeaderboardProviderTestHelpers;

// ── LB-01: a read goes straight to the by-name route — the board name is what goes on the wire ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardReadsByNameTest, "Flock.Leaderboard.Provider.ReadsAddressTheBoardByName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardReadsByNameTest::RunTest(const FString& Parameters)
{
	// The three read routes are `by-name/{name}/standings|me|around-me`. There is no id-addressed read on
	// the `/v1` surface, so a read must spend exactly one request and that request must carry the name.
	// This assertion is written against the API rather than against the provider: the earlier version of
	// this test derived its expectation from the code, which is how a whole read surface shipped 404ing.
	FFixture F;
	F.SignIn();

	bool bDone = false;
	F.Provider->GetStandings(TEXT("HighScoreTest"), [&](TFlockResult<FFlockStandings> Result)
	{
		bDone = true;
		TestTrue(TEXT("standings succeed"), Result.bSuccess);
		TestEqual(TEXT("total"), Result.Value.Total, 2);
		TestEqual(TEXT("rows"), Result.Value.Items.Num(), 1);
	});
	TestTrue(TEXT("completed"), bDone);
	TestEqual(TEXT("standings cost exactly one request"), F.Fake->Requests.Num(), 1);
	TestTrue(TEXT("and it was the by-name standings route"),
		F.LastUrlContaining(TEXT("/standings")).Contains(TEXT("leaderboard/by-name/HighScoreTest/standings")));

	F.Provider->GetMyRank(TEXT("HighScoreTest"), [](TFlockResult<FFlockPlayerRank>) {});
	TestTrue(TEXT("my rank uses by-name/me"),
		F.LastUrlContaining(TEXT("/me")).Contains(TEXT("leaderboard/by-name/HighScoreTest/me")));

	F.Provider->GetAroundMe(TEXT("HighScoreTest"), [](TFlockResult<FFlockStandings>) {});
	TestTrue(TEXT("around me uses by-name/around-me"),
		F.LastUrlContaining(TEXT("/around-me")).Contains(TEXT("leaderboard/by-name/HighScoreTest/around-me")));

	// No request may carry a board id: the id routes belong to the unversioned dashboard API.
	for (const FFlockHttpRequest& Request : F.Fake->Requests)
	{
		TestTrue(FString::Printf(TEXT("'%s' is addressed by name"), *Request.Url),
			Request.Url.Contains(TEXT("leaderboard/by-name/"), ESearchCase::CaseSensitive));
		TestFalse(FString::Printf(TEXT("'%s' carries no board id"), *Request.Url),
			Request.Url.Contains(TEXT("leaderboard/lb-1"), ESearchCase::CaseSensitive));
	}

	// Three reads, three requests: no name-to-id resolve is spent on any of them.
	TestEqual(TEXT("three reads, three requests"), F.Fake->Requests.Num(), 3);
	Cleanup(F.Dir);
	return true;
}

// ── LB-02: resolving a name must not cost a round trip on every read ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardMemoizesTest, "Flock.Leaderboard.Provider.MemoizesBoardLookup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardMemoizesTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Provider->GetByName(TEXT("HighScoreTest"), [](TFlockResult<FFlockLeaderboard>) {});
	F.Provider->GetByName(TEXT("HighScoreTest"), [](TFlockResult<FFlockLeaderboard>) {});

	// The memo serves GetByName itself. It is deliberately *not* a routing step any more: a standings read
	// carries the name, so nothing about it consults this memo (LB-01 pins that it costs one request).
	TestEqual(TEXT("board config fetched once for two lookups"), F.Fake->Requests.Num(), 1);

	// ResolveId is the same lookup, so it must not spend another call either.
	bool bResolved = false;
	F.Provider->ResolveId(TEXT("HighScoreTest"), [&](TFlockResult<FString> Result)
	{
		bResolved = true;
		TestTrue(TEXT("resolve succeeds"), Result.bSuccess);
		TestEqual(TEXT("id"), Result.Value, FString(TEXT("lb-1")));
	});
	TestTrue(TEXT("resolve completed"), bResolved);
	TestEqual(TEXT("still one board-config call"), F.Fake->Requests.Num(), 1);
	Cleanup(F.Dir);
	return true;
}

// ── LB-03: an unknown board is a caller mistake, not an empty leaderboard ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardUnknownNameTest, "Flock.Leaderboard.Provider.UnknownNameFailsValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardUnknownNameTest::RunTest(const FString& Parameters)
{
	// The server is the only thing that knows a name is unknown now that the name goes on the wire, and it
	// says so with a 404. Handing that back as a 404 would be technically honest and practically useless —
	// the caller mistyped a board name — so all three reads translate it into Validation naming the board.
	// Standings-with-no-rows would be worse still: it sends someone hunting for a data problem.
	{
		FFixture F;
		F.SignIn();
		F.RouteStandings(FFlockFakeTransport::Status(404, TEXT("{\"detail\":\"missing\"}")));

		bool bDone = false;
		F.Provider->GetStandings(TEXT("Nope"), [&](TFlockResult<FFlockStandings> Result)
		{
			bDone = true;
			TestFalse(TEXT("fails"), Result.bSuccess);
			TestTrue(TEXT("as a validation error"), Result.Error.Type == EFlockErrorType::Validation);
			TestTrue(TEXT("error names the board"), Result.Error.Message.Contains(TEXT("Nope")));
			// The status is kept so the snapshot layer still reads this as an authoritative answer.
			TestEqual(TEXT("keeps the 404 status"), Result.Error.StatusCode, 404);
		});
		TestTrue(TEXT("completed"), bDone);
		Cleanup(F.Dir);
	}

	// The same translation on the two bearer routes — a 404 there is the board, not the player.
	{
		FFixture F;
		F.SignIn();
		F.RouteRank(FFlockFakeTransport::Status(404, TEXT("{\"detail\":\"missing\"}")));

		bool bDone = false;
		F.Provider->GetMyRank(TEXT("Nope"), [&](TFlockResult<FFlockPlayerRank> Result)
		{
			bDone = true;
			TestFalse(TEXT("fails"), Result.bSuccess);
			TestTrue(TEXT("as a validation error"), Result.Error.Type == EFlockErrorType::Validation);
			TestTrue(TEXT("error names the board"), Result.Error.Message.Contains(TEXT("Nope")));
		});
		TestTrue(TEXT("completed"), bDone);
		Cleanup(F.Dir);
	}

	// A 404 must not be mistaken for a cache-worthy failure: with a warm snapshot in hand it still
	// propagates, because the board really is gone.
	{
		FFixture F;
		F.Provider->GetStandings(TEXT("HighScoreTest"), [](TFlockResult<FFlockStandings>) {});
		F.RouteStandings(FFlockFakeTransport::Status(404, TEXT("{\"detail\":\"missing\"}")));

		bool bDone = false;
		F.Provider->GetStandings(TEXT("HighScoreTest"), [&](TFlockResult<FFlockStandings> Result)
		{
			bDone = true;
			TestFalse(TEXT("propagates despite the cache"), Result.bSuccess);
			TestTrue(TEXT("as a validation error"), Result.Error.Type == EFlockErrorType::Validation);
		});
		TestTrue(TEXT("completed"), bDone);
		Cleanup(F.Dir);
	}
	return true;
}

// ── LB-04: the two bearer routes fail fast, without spending a request ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardRequiresSignInTest, "Flock.Leaderboard.Provider.BearerRoutesRequireSignIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardRequiresSignInTest::RunTest(const FString& Parameters)
{
	FFixture F; // deliberately not signed in

	bool bRankDone = false;
	F.Provider->GetMyRank(TEXT("HighScoreTest"), [&](TFlockResult<FFlockPlayerRank> Result)
	{
		bRankDone = true;
		TestFalse(TEXT("my rank fails"), Result.bSuccess);
		TestTrue(TEXT("with an auth error"), Result.Error.Type == EFlockErrorType::Auth);
	});

	bool bAroundDone = false;
	F.Provider->GetAroundMe(TEXT("HighScoreTest"), [&](TFlockResult<FFlockStandings> Result)
	{
		bAroundDone = true;
		TestFalse(TEXT("around me fails"), Result.bSuccess);
		TestTrue(TEXT("with an auth error"), Result.Error.Type == EFlockErrorType::Auth);
	});

	TestTrue(TEXT("my rank completed"), bRankDone);
	TestTrue(TEXT("around me completed"), bAroundDone);
	// The point of gating client-side: a guaranteed 401 should not cost a request at all.
	TestEqual(TEXT("no request spent"), F.Fake->Requests.Num(), 0);

	// Signed in, the same calls go through.
	F.SignIn();
	bool bOk = false;
	F.Provider->GetMyRank(TEXT("HighScoreTest"), [&](TFlockResult<FFlockPlayerRank> Result)
	{
		bOk = true;
		TestTrue(TEXT("my rank succeeds signed in"), Result.bSuccess);
		TestTrue(TEXT("ranked"), Result.Value.Ranked);
		TestEqual(TEXT("rank"), Result.Value.Rank, 3);
	});
	TestTrue(TEXT("completed"), bOk);
	Cleanup(F.Dir);
	return true;
}

// ── LB-05: optional filters are omitted when empty and encoded when set ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardQueryParamsTest, "Flock.Leaderboard.Provider.QueryParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardQueryParamsTest::RunTest(const FString& Parameters)
{
	// Current() sends no window at all — that absence is what the API reads as the board's live window.
	{
		FFixture F;
		F.Provider->GetStandings(TEXT("HighScoreTest"), [](TFlockResult<FFlockStandings>) {});
		const FString Url = F.LastUrlContaining(TEXT("/standings"));
		TestFalse(TEXT("no window parameter"), Url.Contains(TEXT("window=")));
		TestFalse(TEXT("no country parameter"), Url.Contains(TEXT("country=")));
		TestTrue(TEXT("page always sent"), Url.Contains(TEXT("page=1")));
		TestTrue(TEXT("limit always sent"), Url.Contains(TEXT("limit=50")));
		Cleanup(F.Dir);
	}

	// Set values are sent and percent-encoded; a raw space would make an invalid URL.
	{
		FFixture F;
		F.Provider->GetStandings(TEXT("HighScoreTest"), FFlockLeaderboardWindow::Period(TEXT("2026 W31")),
			TEXT("SA"), 2, 25, [](TFlockResult<FFlockStandings>) {});
		const FString Url = F.LastUrlContaining(TEXT("/standings"));
		TestTrue(TEXT("window sent"), Url.Contains(TEXT("window=2026%20W31")));
		TestFalse(TEXT("no raw space"), Url.Contains(TEXT(" ")));
		TestTrue(TEXT("country sent"), Url.Contains(TEXT("country=SA")));
		TestTrue(TEXT("page sent"), Url.Contains(TEXT("page=2")));
		TestTrue(TEXT("limit sent"), Url.Contains(TEXT("limit=25")));
		Cleanup(F.Dir);
	}

	// The season maker's key reaches the wire intact.
	{
		FFixture F;
		F.SignIn();
		F.Provider->GetAroundMe(TEXT("HighScoreTest"), 3, FFlockLeaderboardWindow::Season(TEXT("s7")),
			FString(), [](TFlockResult<FFlockStandings>) {});
		const FString Url = F.LastUrlContaining(TEXT("around-me"));
		TestTrue(TEXT("season window sent"), Url.Contains(TEXT("window=season%3As7")));
		TestTrue(TEXT("neighbours sent"), Url.Contains(TEXT("n=3")));
		TestFalse(TEXT("no country parameter"), Url.Contains(TEXT("country=")));
		Cleanup(F.Dir);
	}
	return true;
}

// ── LB-06: a board UI shows the last-known standings offline rather than an error screen ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardOfflineTest, "Flock.Leaderboard.Provider.ServesCacheWhenOffline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardOfflineTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Provider->GetStandings(TEXT("HighScoreTest"), [](TFlockResult<FFlockStandings>) {});
	const int32 CallsWhileWarm = F.Fake->Requests.Num();

	// Same provider, now unreachable: the cached copy is served without touching the network at all. The
	// fake still answers 200 here on purpose — the request-count assertion below is what proves the
	// cache-serving branch fired before any call, rather than the call simply having failed.
	F.GoOfflineExpectingNoCalls();
	bool bDone = false;
	F.Provider->GetStandings(TEXT("HighScoreTest"), [&](TFlockResult<FFlockStandings> Result)
	{
		bDone = true;
		TestTrue(TEXT("served from cache"), Result.bSuccess);
		TestEqual(TEXT("cached total"), Result.Value.Total, 2);
		TestEqual(TEXT("cached rows"), Result.Value.Items.Num(), 1);
	});
	TestTrue(TEXT("completed"), bDone);
	TestEqual(TEXT("no request issued while offline"), F.Fake->Requests.Num(), CallsWhileWarm);

	// Without a cache to fall back on, the failure propagates instead of inventing an empty board.
	F.GoOffline();
	bool bColdDone = false;
	F.Provider->GetStandings(TEXT("HighScoreTest"), FFlockLeaderboardWindow::Period(TEXT("2026-W02")),
		FString(), 1, 50, [&](TFlockResult<FFlockStandings> Result)
	{
		bColdDone = true;
		TestFalse(TEXT("cold read offline fails"), Result.bSuccess);
	});
	TestTrue(TEXT("completed"), bColdDone);
	Cleanup(F.Dir);
	return true;
}

// ── LB-07: an authoritative 4xx is an answer, so it propagates even with a cache in hand ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardPermanentFailureTest, "Flock.Leaderboard.Provider.PropagatesPermanentFailureDespiteCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardPermanentFailureTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Provider->GetStandings(TEXT("HighScoreTest"), [](TFlockResult<FFlockStandings>) {});

	// A permanent 4xx that is *not* the unknown-board 404 — that one has its own translation and its own
	// test (LB-03), so this pins the general rule rather than riding on the one status it special-cases.
	// Serving the cache here would hide a real, permanent answer.
	F.RouteStandings(FFlockFakeTransport::Status(410, TEXT("{\"detail\":\"gone\"}")));
	bool bDone = false;
	F.Provider->GetStandings(TEXT("HighScoreTest"), [&](TFlockResult<FFlockStandings> Result)
	{
		bDone = true;
		TestFalse(TEXT("a permanent 4xx propagates despite the cache"), Result.bSuccess);
		TestEqual(TEXT("and keeps its status"), Result.Error.StatusCode, 410);
	});
	TestTrue(TEXT("completed"), bDone);
	Cleanup(F.Dir);
	return true;
}

// ── LB-08: one player's placement must never be served to the next on a shared device ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardPlayerScopedCacheTest, "Flock.Leaderboard.Provider.PlayerScopedRankCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardPlayerScopedCacheTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Provider->GetMyRank(TEXT("HighScoreTest"), [](TFlockResult<FFlockPlayerRank>) {});

	// Player B on the same device, offline. Player A's cached rank is keyed to A, so B must not get it —
	// with the network down there is nothing else to answer with, which is what makes the leak visible.
	F.SignIn(TEXT("player-b"));
	F.GoOffline();
	bool bDone = false;
	F.Provider->GetMyRank(TEXT("HighScoreTest"), [&](TFlockResult<FFlockPlayerRank> Result)
	{
		bDone = true;
		TestFalse(TEXT("player B does not inherit player A's cached rank"), Result.bSuccess);
	});
	TestTrue(TEXT("completed"), bDone);
	Cleanup(F.Dir);
	return true;
}

// ── LB-09: ClearCache drops both halves, so the next read really does hit the backend ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardClearCacheTest, "Flock.Leaderboard.Provider.ClearCacheDropsMemoAndSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardClearCacheTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Provider->GetByName(TEXT("HighScoreTest"), [](TFlockResult<FFlockLeaderboard>) {});
	F.Provider->GetStandings(TEXT("HighScoreTest"), [](TFlockResult<FFlockStandings>) {});
	TestEqual(TEXT("a lookup and a standings read"), F.Fake->Requests.Num(), 2);

	F.Provider->ClearCache();

	// The memo is gone, so the board config is fetched again.
	F.Provider->GetByName(TEXT("HighScoreTest"), [](TFlockResult<FFlockLeaderboard>) {});
	TestEqual(TEXT("board config refetched after clear"), F.Fake->Requests.Num(), 3);

	// And so is the snapshot: offline after a clear has nothing to serve.
	F.Provider->ClearCache();
	F.GoOffline();
	bool bDone = false;
	F.Provider->GetStandings(TEXT("HighScoreTest"), [&](TFlockResult<FFlockStandings> Result)
	{
		bDone = true;
		TestFalse(TEXT("nothing cached to serve"), Result.bSuccess);
	});
	TestTrue(TEXT("completed"), bDone);
	Cleanup(F.Dir);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
