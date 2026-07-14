// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Guards/FlockConfigValidator.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockBuildBlockReasonTest, "Flock.Editor.BuildGuard.GetBuildBlockReason",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockBuildBlockReasonTest::RunTest(const FString& Parameters)
{
	// Guard disabled -> never blocks.
	TestTrue(TEXT("Guard off allows the build"),
		UFlockConfigValidator::GetBuildBlockReason(TEXT(""), /*bCanResolve*/ true, /*bGuardEnabled*/ false).IsEmpty());

	// Stub lookup (cannot resolve) -> inert even with an empty ID and the guard on.
	TestTrue(TEXT("Inert while only the stub lookup is registered"),
		UFlockConfigValidator::GetBuildBlockReason(TEXT(""), /*bCanResolve*/ false, /*bGuardEnabled*/ true).IsEmpty());

	// Real lookup + empty ID + guard on -> blocks.
	TestFalse(TEXT("Blocks on an unresolved ID once the lookup is real"),
		UFlockConfigValidator::GetBuildBlockReason(TEXT(""), /*bCanResolve*/ true, /*bGuardEnabled*/ true).IsEmpty());

	// Real lookup + resolved ID -> allows.
	TestTrue(TEXT("A resolved ID allows the build"),
		UFlockConfigValidator::GetBuildBlockReason(TEXT("ver-abc"), /*bCanResolve*/ true, /*bGuardEnabled*/ true).IsEmpty());

	return true;
}

#endif // WITH_AUTOMATION_TESTS
