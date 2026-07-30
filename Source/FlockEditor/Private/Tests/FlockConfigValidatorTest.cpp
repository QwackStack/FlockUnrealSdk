// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Guards/FlockConfigValidator.h"
#include "Guards/FlockPlayModeGuard.h"
#include "Setup/FlockSetupStatus.h"

namespace
{
	/** A fully set-up project. Kept in step with the evaluator's own healthy fixture. */
	FFlockSetupInput HealthyProject()
	{
		FFlockSetupInput Input;
		Input.ApiUrl = TEXT("https://api-flock.qwacks.com");
		Input.ApiKey = TEXT("key-abc");
		Input.GameId = TEXT("Duck Odyssey");
		Input.GameVersion = TEXT("1.0.0");
		Input.GameVersionId = TEXT("ver-abc");
		Input.CurrentSdkVersion = TEXT("0.16.0");
		Input.LastSeenSdkVersion = TEXT("0.16.0");
		return Input;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockBuildBlockReasonTest, "Flock.Editor.BuildGuard.GetBuildBlockReason",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockBuildBlockReasonTest::RunTest(const FString& Parameters)
{
	FFlockSetupInput Unresolved = HealthyProject();
	Unresolved.GameVersionId.Empty();
	const TArray<FFlockSetupFinding> UnresolvedFindings = FFlockSetupStatus::Evaluate(Unresolved);
	const TArray<FFlockSetupFinding> HealthyFindings = FFlockSetupStatus::Evaluate(HealthyProject());

	// Guard disabled -> never blocks.
	TestTrue(TEXT("Guard off allows the build"),
		UFlockConfigValidator::GetBuildBlockReason(UnresolvedFindings, /*bCanResolve*/ true, /*bGuardEnabled*/ false).IsEmpty());

	// Stub lookup (cannot resolve) -> inert even with an unresolved version and the guard on.
	TestTrue(TEXT("Inert while only the stub lookup is registered"),
		UFlockConfigValidator::GetBuildBlockReason(UnresolvedFindings, /*bCanResolve*/ false, /*bGuardEnabled*/ true).IsEmpty());

	// Real lookup + unresolved + guard on -> blocks.
	TestFalse(TEXT("Blocks on an unresolved ID once the lookup is real"),
		UFlockConfigValidator::GetBuildBlockReason(UnresolvedFindings, /*bCanResolve*/ true, /*bGuardEnabled*/ true).IsEmpty());

	// Healthy -> allows.
	TestTrue(TEXT("A healthy project allows the build"),
		UFlockConfigValidator::GetBuildBlockReason(HealthyFindings, /*bCanResolve*/ true, /*bGuardEnabled*/ true).IsEmpty());

	// The hole the findings model closes: the version ID is baked and valid, but the API key has since
	// been cleared or rotated. The old check looked only at GameVersionId, so this packaged clean and
	// then failed at runtime.
	{
		FFlockSetupInput RotatedKey = HealthyProject();
		RotatedKey.ApiKey.Empty();
		const FString Reason = UFlockConfigValidator::GetBuildBlockReason(
			FFlockSetupStatus::Evaluate(RotatedKey), /*bCanResolve*/ true, /*bGuardEnabled*/ true);
		TestFalse(TEXT("A cleared API key blocks the build even with a baked version ID"), Reason.IsEmpty());
	}

	// A warning must never block a package — it still initializes.
	{
		FFlockSetupInput Drifted = HealthyProject();
		Drifted.bCodegenManifestPresent = true;
		Drifted.CodegenManifestVersionId = TEXT("ver-old");
		const TArray<FFlockSetupFinding> Findings = FFlockSetupStatus::Evaluate(Drifted);
		TestTrue(TEXT("Codegen drift is present"), Findings.Num() > 0);
		TestTrue(TEXT("...but a warning does not block the build"),
			UFlockConfigValidator::GetBuildBlockReason(Findings, /*bCanResolve*/ true, /*bGuardEnabled*/ true).IsEmpty());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGuardEquivalenceTest, "Flock.Editor.BuildGuard.SurfacesAgree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockGuardEquivalenceTest::RunTest(const FString& Parameters)
{
	// The regression the whole architecture exists to prevent: the PIE guard and the packaging validator
	// must block on exactly the same set the panel shows as errors. This fails the day someone adds a
	// check to one surface and not the others.
	FFlockSetupInput Broken = HealthyProject();
	Broken.ApiKey.Empty();
	Broken.GameVersionId.Empty();
	Broken.bCodegenManifestPresent = true;
	Broken.CodegenManifestVersionId = TEXT("ver-old");
	Broken.LastSeenSdkVersion = TEXT("0.15.0");

	const TArray<FFlockSetupFinding> All = FFlockSetupStatus::Evaluate(Broken);
	const TArray<FFlockSetupFinding> PanelErrors = FFlockSetupStatus::AtLeast(All, EFlockSetupSeverity::Error);
	const TArray<FFlockSetupFinding> GuardBlocking = FFlockPlayModeGuard::BlockingFindings(
		All, /*bGuardEnabled*/ true, /*bAutoInit*/ true);

	TestEqual(TEXT("The guard blocks on exactly the panel's errors"), GuardBlocking.Num(), PanelErrors.Num());
	for (int32 Index = 0; Index < PanelErrors.Num() && Index < GuardBlocking.Num(); ++Index)
	{
		TestEqual(TEXT("Same finding, same order"), GuardBlocking[Index].Id, PanelErrors[Index].Id);
	}

	// The validator agrees too: it blocks precisely when there is something for the guard to block on.
	const bool bValidatorBlocks = !UFlockConfigValidator::GetBuildBlockReason(
		All, /*bCanResolve*/ true, /*bGuardEnabled*/ true).IsEmpty();
	TestEqual(TEXT("The validator blocks iff the guard does"), bValidatorBlocks, GuardBlocking.Num() > 0);

	// And the two opt-outs still work, independently.
	TestEqual(TEXT("Guard disabled blocks nothing"),
		FFlockPlayModeGuard::BlockingFindings(All, /*bGuardEnabled*/ false, /*bAutoInit*/ true).Num(), 0);
	TestEqual(TEXT("Manual init blocks nothing — the game drives init itself"),
		FFlockPlayModeGuard::BlockingFindings(All, /*bGuardEnabled*/ true, /*bAutoInit*/ false).Num(), 0);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
