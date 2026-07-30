// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Setup/FlockSummonPolicy.h"

// Path must not be a prefix of another test's path. UE treats the dotted name as a hierarchy, so a test
// registered at "…SummonPolicy" is shadowed the moment a sibling registers at "…SummonPolicy.Something" —
// it stops running silently, and the total count hides it because the new test replaces it one-for-one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSummonPolicyTest, "Flock.Editor.Setup.SummonPolicy.Rules",
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSummonRecordSeenTest, "Flock.Editor.Setup.SummonPolicy.HeadlessRecordsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSummonRecordSeenTest::RunTest(const FString& Parameters)
{
	// Regression: a headless run raises no UI but was still recording the SDK version as "seen", which
	// consumed the one-shot first-run welcome (and, after a version bump, the upgrade notice) before the
	// developer ever opened the editor. Automation runs ate it every time.
	{
		FFlockSummonContext Context;
		Context.bHeadless = true;
		Context.bFirstAdd = true;
		Context.bHasError = true;

		const FFlockSummonDecision Decision = FFlockSummonPolicy::Decide(Context);
		TestFalse(TEXT("Headless opens nothing"), Decision.bOpenPanel);
		TestFalse(TEXT("Headless records nothing — nobody saw anything"), Decision.bRecordSeen);
	}

	// With a human present, the seen-state is recorded whatever we decide about the panel — otherwise a
	// healthy project would summon on every launch forever.
	{
		FFlockSummonContext Context;
		Context.bFirstAdd = true;
		const FFlockSummonDecision Decision = FFlockSummonPolicy::Decide(Context);
		TestTrue(TEXT("First add opens the panel"), Decision.bOpenPanel);
		TestTrue(TEXT("...and records that it was seen"), Decision.bRecordSeen);
	}

	{
		// Healthy project: nothing to show, but the developer was there, so it counts as seen.
		FFlockSummonContext Context;
		const FFlockSummonDecision Decision = FFlockSummonPolicy::Decide(Context);
		TestFalse(TEXT("Healthy project opens nothing"), Decision.bOpenPanel);
		TestTrue(TEXT("...but still records as seen, or it would summon every launch"), Decision.bRecordSeen);
	}

	{
		// Suppressed and already-summoned are also human-present cases.
		FFlockSummonContext Context;
		Context.bHasError = true;
		Context.bSuppressedByUser = true;
		TestTrue(TEXT("A muted notice still counts as seen"), FFlockSummonPolicy::Decide(Context).bRecordSeen);

		FFlockSummonContext Second;
		Second.bHasError = true;
		Second.bAlreadySummonedThisSession = true;
		TestTrue(TEXT("Second call in a session still counts as seen"), FFlockSummonPolicy::Decide(Second).bRecordSeen);
	}

	// Decide and ShouldSummon must never disagree about the panel.
	{
		const bool bFlags[] = { false, true };
		for (bool bHeadless : bFlags)
		for (bool bFirstAdd : bFlags)
		for (bool bHasError : bFlags)
		for (bool bSuppressed : bFlags)
		for (bool bAlready : bFlags)
		{
			FFlockSummonContext Context;
			Context.bHeadless = bHeadless;
			Context.bFirstAdd = bFirstAdd;
			Context.bHasError = bHasError;
			Context.bSuppressedByUser = bSuppressed;
			Context.bAlreadySummonedThisSession = bAlready;

			TestEqual(TEXT("Decide().bOpenPanel matches ShouldSummon() for every input"),
				FFlockSummonPolicy::Decide(Context).bOpenPanel, FFlockSummonPolicy::ShouldSummon(Context));
		}
	}

	return true;
}

#endif // WITH_AUTOMATION_TESTS
