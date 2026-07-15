// Copyright 2022, Qwacks. All Rights Reserved.

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

	return true;
}

#endif // WITH_AUTOMATION_TESTS
