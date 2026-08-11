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

	// Leaderboards. The read routes take an id, but only the by-name lookup is ever handed caller text —
	// so that is the one that has to encode.
	TestEqual(TEXT("leaderboard by name"), FlockEndpoints::LeaderboardByName(TEXT("HighScoreTest")),
		FString(TEXT("leaderboard/by-name/HighScoreTest")));
	TestEqual(TEXT("leaderboard by id"), FlockEndpoints::LeaderboardById(TEXT("lb-1")), FString(TEXT("leaderboard/lb-1")));
	TestEqual(TEXT("leaderboard me"), FlockEndpoints::LeaderboardMe(TEXT("lb-1")), FString(TEXT("leaderboard/lb-1/me")));
	TestEqual(TEXT("leaderboard around me"), FlockEndpoints::LeaderboardAroundMe(TEXT("lb-1")),
		FString(TEXT("leaderboard/lb-1/around-me")));

	const FString EncodedBoard = FlockEndpoints::LeaderboardByName(TEXT("Weekly Best"));
	TestTrue(TEXT("keeps leaderboard by-name prefix"), EncodedBoard.StartsWith(TEXT("leaderboard/by-name/")));
	TestFalse(TEXT("no raw space in encoded board name"), EncodedBoard.Contains(TEXT(" ")));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
