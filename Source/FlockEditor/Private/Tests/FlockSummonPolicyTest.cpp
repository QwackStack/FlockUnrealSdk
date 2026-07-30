// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Setup/FlockSummonPolicy.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSummonPolicyTest, "Flock.Editor.Setup.SummonPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSummonPolicyTest::RunTest(const FString& Parameters)
{
	// A healthy project never opens anything. This is the acceptance rule for the whole feature: if it
	// ever fails, the panel is a nuisance no matter how good it looks.
	{
		FFlockSummonContext Context;
		TestFalse(TEXT("Healthy project stays silent"), FFlockSummonPolicy::ShouldSummon(Context));
	}

	// An error is the ordinary reason to interrupt.
	{
		FFlockSummonContext Context;
		Context.bHasError = true;
		TestTrue(TEXT("An error summons"), FFlockSummonPolicy::ShouldSummon(Context));
	}

	// A warning is not an error, and must not summon.
	{
		FFlockSummonContext Context;
		Context.bHasError = false;
		TestFalse(TEXT("A non-error state does not summon"), FFlockSummonPolicy::ShouldSummon(Context));
	}

	// First add is the one case that opens with nothing wrong.
	{
		FFlockSummonContext Context;
		Context.bFirstAdd = true;
		TestTrue(TEXT("First add summons without an error"), FFlockSummonPolicy::ShouldSummon(Context));
	}

	// Headless outranks everything, including a first add.
	{
		FFlockSummonContext Context;
		Context.bHeadless = true;
		Context.bHasError = true;
		Context.bFirstAdd = true;
		TestFalse(TEXT("Headless never raises UI"), FFlockSummonPolicy::ShouldSummon(Context));
	}

	// Once per editor process, however bad the state is.
	{
		FFlockSummonContext Context;
		Context.bAlreadySummonedThisSession = true;
		Context.bHasError = true;
		TestFalse(TEXT("Only summons once per session"), FFlockSummonPolicy::ShouldSummon(Context));

		Context.bFirstAdd = true;
		TestFalse(TEXT("Session cap also applies to a first add"), FFlockSummonPolicy::ShouldSummon(Context));
	}

	// Suppression mutes the error-driven interruption...
	{
		FFlockSummonContext Context;
		Context.bHasError = true;
		Context.bSuppressedByUser = true;
		TestFalse(TEXT("Suppression stops the interruption"), FFlockSummonPolicy::ShouldSummon(Context));
	}

	// ...but cannot mute a first add, which has no finding to have been suppressed.
	{
		FFlockSummonContext Context;
		Context.bFirstAdd = true;
		Context.bSuppressedByUser = true;
		TestTrue(TEXT("Suppression does not apply to a first add"), FFlockSummonPolicy::ShouldSummon(Context));
	}

	return true;
}

#endif // WITH_AUTOMATION_TESTS
