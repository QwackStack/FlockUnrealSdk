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
		 * Routes every leaderboard shape. Order matters and is load-bearing: the fake returns the first
		 * route whose fragment the URL contains, and "leaderboard/lb-1" is a substring of both the /me and
		 * /around-me URLs — so the specific ones have to be registered first.
		 */
		void RouteAll()
		{
			Fake->On(TEXT("lb-1/around-me"), FFlockFakeTransport::Ok(StandingsBody()));
			Fake->On(TEXT("lb-1/me"), FFlockFakeTransport::Ok(RankBody()));
			Fake->On(TEXT("leaderboard/by-name/"), FFlockFakeTransport::Ok(BoardBody()));
			Fake->On(TEXT("leaderboard/lb-1"), FFlockFakeTransport::Ok(StandingsBody()));
		}

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
		 * All four routes are re-registered rather than patched one at a time, because On() appends and the
		 * fake returns the first fragment match: overriding "lb-1/me" alone would drop it behind the broader
		 * "leaderboard/lb-1" route and quietly answer a rank read with standings.
		 */
		void GoOffline()
		{
			Provider->SetReachabilityProbe([]() { return false; });
			Fake->On(TEXT("lb-1/around-me"), FFlockFakeTransport::Offline());
			Fake->On(TEXT("lb-1/me"), FFlockFakeTransport::Offline());
			Fake->On(TEXT("leaderboard/by-name/"), FFlockFakeTransport::Offline());
			Fake->On(TEXT("leaderboard/lb-1"), FFlockFakeTransport::Offline());
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

// ── LB-01: a read takes a name; the id comes from the by-name route, then the id route is called ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardResolvesNameTest, "Flock.Leaderboard.Provider.ResolvesNameThenReads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardResolvesNameTest::RunTest(const FString& Parameters)
{
	FFixture F;
	bool bDone = false;
	F.Provider->GetStandings(TEXT("HighScoreTest"), [&](TFlockResult<FFlockStandings> Result)
	{
		bDone = true;
		TestTrue(TEXT("standings succeed"), Result.bSuccess);
		TestEqual(TEXT("total"), Result.Value.Total, 2);
		TestEqual(TEXT("rows"), Result.Value.Items.Num(), 1);
	});

	TestTrue(TEXT("completed"), bDone);
	TestEqual(TEXT("resolved by name once"), F.Fake->CountTo(TEXT("leaderboard/by-name/")), 1);
	TestEqual(TEXT("then read the id route"), F.Fake->CountTo(TEXT("leaderboard/lb-1")), 1);
	Cleanup(F.Dir);
	return true;
}

// ── LB-02: resolving a name must not cost a round trip on every read ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardMemoizesTest, "Flock.Leaderboard.Provider.MemoizesBoardLookup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardMemoizesTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Provider->GetStandings(TEXT("HighScoreTest"), [](TFlockResult<FFlockStandings>) {});
	F.Provider->GetStandings(TEXT("HighScoreTest"), [](TFlockResult<FFlockStandings>) {});

	TestEqual(TEXT("by-name fetched once for two reads"), F.Fake->CountTo(TEXT("leaderboard/by-name/")), 1);

	// ResolveId is the same lookup, so it must not spend another call either.
	bool bResolved = false;
	F.Provider->ResolveId(TEXT("HighScoreTest"), [&](TFlockResult<FString> Result)
	{
		bResolved = true;
		TestTrue(TEXT("resolve succeeds"), Result.bSuccess);
		TestEqual(TEXT("id"), Result.Value, FString(TEXT("lb-1")));
	});
	TestTrue(TEXT("resolve completed"), bResolved);
	TestEqual(TEXT("still one by-name call"), F.Fake->CountTo(TEXT("leaderboard/by-name/")), 1);
	Cleanup(F.Dir);
	return true;
}

// ── LB-03: an unknown board is a caller mistake, not an empty leaderboard ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardUnknownNameTest, "Flock.Leaderboard.Provider.UnknownNameFailsValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardUnknownNameTest::RunTest(const FString& Parameters)
{
	// A 2xx that resolves to no board at all: Validation, and the read route is never attempted.
	{
		FFixture F;
		F.Fake->On(TEXT("leaderboard/by-name/"), FFlockFakeTransport::Ok(BoardBody(TEXT(""))));

		bool bDone = false;
		F.Provider->GetStandings(TEXT("Nope"), [&](TFlockResult<FFlockStandings> Result)
		{
			bDone = true;
			TestFalse(TEXT("fails"), Result.bSuccess);
			TestTrue(TEXT("as a validation error"), Result.Error.Type == EFlockErrorType::Validation);
			TestTrue(TEXT("error names the board"), Result.Error.Message.Contains(TEXT("Nope")));
		});
		TestTrue(TEXT("completed"), bDone);
		TestEqual(TEXT("no standings call attempted"), F.Fake->CountTo(TEXT("leaderboard/lb-1")), 0);
		Cleanup(F.Dir);
	}

	// A 404 from the lookup propagates, and likewise never reaches the read route.
	{
		FFixture F;
		F.Fake->On(TEXT("leaderboard/by-name/"), FFlockFakeTransport::Status(404, TEXT("{\"detail\":\"missing\"}")));

		bool bDone = false;
		F.Provider->GetStandings(TEXT("Nope"), [&](TFlockResult<FFlockStandings> Result)
		{
			bDone = true;
			TestFalse(TEXT("fails"), Result.bSuccess);
		});
		TestTrue(TEXT("completed"), bDone);
		TestEqual(TEXT("no standings call attempted"), F.Fake->CountTo(TEXT("leaderboard/lb-1")), 0);
		Cleanup(F.Dir);
	}
	return true;
}

// ── LB-04: the two bearer routes fail fast, before the name lookup is spent ──
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
	// The point of gating before the resolve: a guaranteed 401 should not cost a lookup first.
	TestEqual(TEXT("no by-name lookup spent"), F.Fake->CountTo(TEXT("leaderboard/by-name/")), 0);

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
		const FString Url = F.LastUrlContaining(TEXT("leaderboard/lb-1"));
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
		const FString Url = F.LastUrlContaining(TEXT("leaderboard/lb-1"));
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

	// The board was deleted server-side. Serving the cache here would hide a real, permanent answer.
	F.Fake->On(TEXT("leaderboard/lb-1"), FFlockFakeTransport::Status(404, TEXT("{\"detail\":\"gone\"}")));
	bool bDone = false;
	F.Provider->GetStandings(TEXT("HighScoreTest"), [&](TFlockResult<FFlockStandings> Result)
	{
		bDone = true;
		TestFalse(TEXT("404 propagates despite the cache"), Result.bSuccess);
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
	F.Provider->GetStandings(TEXT("HighScoreTest"), [](TFlockResult<FFlockStandings>) {});
	TestEqual(TEXT("one by-name call"), F.Fake->CountTo(TEXT("leaderboard/by-name/")), 1);

	F.Provider->ClearCache();

	// The memo is gone, so the name is resolved again.
	F.Provider->GetStandings(TEXT("HighScoreTest"), [](TFlockResult<FFlockStandings>) {});
	TestEqual(TEXT("by-name refetched after clear"), F.Fake->CountTo(TEXT("leaderboard/by-name/")), 2);

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
