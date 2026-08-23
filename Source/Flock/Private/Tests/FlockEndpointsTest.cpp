// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockEndpoints.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockEndpointsBuildTest, "Flock.Http.Endpoints.Build",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockEndpointsBuildTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("constant path"), FString(FlockEndpoints::PlayerLogin), FString(TEXT("player/login")));
	TestEqual(TEXT("by-id builder"), FlockEndpoints::PlayerDataById(TEXT("abc")), FString(TEXT("player_data/abc")));
	TestEqual(TEXT("simple by-name"), FlockEndpoints::GameVersionByName(TEXT("v1")), FString(TEXT("game_version/by-name/v1")));

	// A name with a space must be percent-encoded, never left raw, under the by-name prefix.
	const FString Encoded = FlockEndpoints::GameVersionByName(TEXT("beta build"));
	TestTrue(TEXT("keeps by-name prefix"), Encoded.StartsWith(TEXT("game_version/by-name/")));
	TestFalse(TEXT("no raw space in encoded name"), Encoded.Contains(TEXT(" ")));

	// Leaderboards. Every read is by name, so every one of these takes caller text and has to encode.
	TestEqual(TEXT("leaderboard by name"), FlockEndpoints::LeaderboardByName(TEXT("HighScoreTest")),
		FString(TEXT("leaderboard/by-name/HighScoreTest")));

	const FString EncodedBoard = FlockEndpoints::LeaderboardByName(TEXT("Weekly Best"));
	TestTrue(TEXT("keeps leaderboard by-name prefix"), EncodedBoard.StartsWith(TEXT("leaderboard/by-name/")));
	TestFalse(TEXT("no raw space in encoded board name"), EncodedBoard.Contains(TEXT(" ")));

	return true;
}

/**
 * Locks the four `/v1` leaderboard paths against the **live** surface, probed 2026-08-20.
 *
 * A path lock is only worth anything if the paths in it were checked against a running backend. The
 * previous version of this test locked `leaderboard/{id}`, `/{id}/me` and `/{id}/around-me` — literal,
 * green, and wrong: those three 404 on every backend, so the whole read surface was non-functional while
 * the suite reported it covered. The expectation had been derived from the implementation instead of from
 * the API, which is a test that can only ever agree with the code.
 *
 * Held separate from the builder test above so the failure message says *which* half broke: a builder that
 * stopped encoding, or a path that drifted off the API.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockEndpointsLeaderboardPathsTest, "Flock.Http.Endpoints.LeaderboardPathsMatchV1",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockEndpointsLeaderboardPathsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("board config"), FlockEndpoints::LeaderboardByName(TEXT("Weekly")),
		FString(TEXT("leaderboard/by-name/Weekly")));
	TestEqual(TEXT("standings"), FlockEndpoints::LeaderboardStandings(TEXT("Weekly")),
		FString(TEXT("leaderboard/by-name/Weekly/standings")));
	TestEqual(TEXT("my rank"), FlockEndpoints::LeaderboardMe(TEXT("Weekly")),
		FString(TEXT("leaderboard/by-name/Weekly/me")));
	TestEqual(TEXT("around me"), FlockEndpoints::LeaderboardAroundMe(TEXT("Weekly")),
		FString(TEXT("leaderboard/by-name/Weekly/around-me")));

	// The id-shaped paths are the unversioned dashboard routes (OAuth2, X-Game-Id) and are not this SDK's
	// to call. No builder here may produce one, whatever a board is named.
	const TArray<FString> Built = {
		FlockEndpoints::LeaderboardByName(TEXT("Weekly")),
		FlockEndpoints::LeaderboardStandings(TEXT("Weekly")),
		FlockEndpoints::LeaderboardMe(TEXT("Weekly")),
		FlockEndpoints::LeaderboardAroundMe(TEXT("Weekly")),
	};
	for (const FString& Path : Built)
	{
		TestTrue(FString::Printf(TEXT("'%s' is addressed by name"), *Path),
			Path.StartsWith(TEXT("leaderboard/by-name/"), ESearchCase::CaseSensitive));
	}

	return true;
}

#endif // WITH_AUTOMATION_TESTS
