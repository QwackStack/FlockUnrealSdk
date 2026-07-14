// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Version/FlockVersionResolver.h"
#include "Version/FlockVersionLookup.h"
#include "Config/FlockConfig.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockByNameUrlTest, "Flock.Editor.Resolver.ByNameUrl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockByNameUrlTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Trailing slash trimmed, version segment added, name percent-encoded"),
		FFlockVersionResolver::ByNameUrl(TEXT("https://api-flock.qwacks.com/"), TEXT("My Version")),
		FString(TEXT("https://api-flock.qwacks.com/v1/game_version/by-name/My%20Version")));

	TestEqual(TEXT("No trailing slash, plain name preserved"),
		FFlockVersionResolver::ByNameUrl(TEXT("https://api-flock.qwacks.com"), TEXT("1.0.0")),
		FString(TEXT("https://api-flock.qwacks.com/v1/game_version/by-name/1.0.0")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockApplyResultTest, "Flock.Editor.Resolver.ApplyResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockApplyResultTest::RunTest(const FString& Parameters)
{
	UFlockConfig* Config = NewObject<UFlockConfig>(GetTransientPackage());
	Config->GameVersionId = TEXT("old");

	// A failed result leaves the prior ID untouched.
	TestFalse(TEXT("Failed result reports no change"),
		FFlockVersionResolver::ApplyResult(*Config, FFlockResolveResult::Fail(TEXT("boom"))));
	TestEqual(TEXT("Prior ID preserved on failure"), Config->GameVersionId, FString(TEXT("old")));

	// A successful result with a new ID changes it.
	TestTrue(TEXT("New ID reports a change"),
		FFlockVersionResolver::ApplyResult(*Config, FFlockResolveResult::Ok(TEXT("new"))));
	TestEqual(TEXT("ID updated on success"), Config->GameVersionId, FString(TEXT("new")));

	// The same ID is a no-op.
	TestFalse(TEXT("Unchanged ID reports no change"),
		FFlockVersionResolver::ApplyResult(*Config, FFlockResolveResult::Ok(TEXT("new"))));

	// An empty success ID is treated as no change.
	TestFalse(TEXT("Empty success ID reports no change"),
		FFlockVersionResolver::ApplyResult(*Config, FFlockResolveResult::Ok(TEXT(""))));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
