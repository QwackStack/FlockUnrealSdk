// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Config/FlockConfig.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigIsValidTest, "Flock.Runtime.Config.IsValid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigIsValidTest::RunTest(const FString& Parameters)
{
	UFlockConfig* Config = NewObject<UFlockConfig>(GetTransientPackage());
	Config->ApiUrl = TEXT("");
	Config->ApiKey = TEXT("");
	Config->GameId = TEXT("");
	Config->GameVersion = TEXT("");

	FString Error;
	TestFalse(TEXT("Empty config is invalid"), Config->IsValid(Error));
	TestEqual(TEXT("First missing field is API URL"), Error, FString(TEXT("API URL is required.")));

	Config->ApiUrl = TEXT("https://api-flock.qwacks.com");
	TestFalse(TEXT("Missing API Key is invalid"), Config->IsValid(Error));
	TestEqual(TEXT("Next missing field is API Key"), Error, FString(TEXT("API Key is required.")));

	Config->ApiKey = TEXT("secret");
	Config->GameId = TEXT("my-game");
	Config->GameVersion = TEXT("1.0.0");
	TestTrue(TEXT("Fully populated config is valid"), Config->IsValid(Error));
	TestTrue(TEXT("Error is cleared on success"), Error.IsEmpty());

	// IsValid must NOT consider the baked GameVersionId — that gate lives in init.
	Config->GameVersionId = TEXT("");
	TestTrue(TEXT("IsValid ignores the (unbaked) GameVersionId"), Config->IsValid(Error));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
