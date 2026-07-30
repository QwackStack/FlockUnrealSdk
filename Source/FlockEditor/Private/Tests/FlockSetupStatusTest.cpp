// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Setup/FlockSetupStatus.h"

namespace
{
	/** A fully set-up project: every check passes, nothing to report. */
	FFlockSetupInput Healthy()
	{
		FFlockSetupInput Input;
		Input.ApiUrl = TEXT("https://api-flock.qwacks.com");
		Input.ApiKey = TEXT("key-abc");
		Input.GameId = TEXT("Duck Odyssey");
		Input.GameVersion = TEXT("1.0.0");
		Input.GameVersionId = TEXT("ver-abc");
		Input.bCodegenManifestPresent = true;
		Input.CodegenManifestVersionId = TEXT("ver-abc");
		Input.Probe = EFlockProbeState::Ok;
		Input.ProbeServerGameName = TEXT("Duck Odyssey");
		Input.CurrentSdkVersion = TEXT("0.16.0");
		Input.LastSeenSdkVersion = TEXT("0.16.0");
		return Input;
	}

	bool Has(const TArray<FFlockSetupFinding>& Findings, const TCHAR* Id)
	{
		const FName Wanted(Id);
		return Findings.ContainsByPredicate(
			[Wanted](const FFlockSetupFinding& Finding) { return Finding.Id == Wanted; });
	}

	const FFlockSetupFinding* Find(const TArray<FFlockSetupFinding>& Findings, const TCHAR* Id)
	{
		const FName Wanted(Id);
		return Findings.FindByPredicate(
			[Wanted](const FFlockSetupFinding& Finding) { return Finding.Id == Wanted; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSetupStatusHealthyTest, "Flock.Editor.Setup.Status.HealthyProjectIsSilent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSetupStatusHealthyTest::RunTest(const FString& Parameters)
{
	// The acceptance test for the whole feature: a configured project on a fresh clone reports nothing.
	const TArray<FFlockSetupFinding> Findings = FFlockSetupStatus::Evaluate(Healthy());
	TestEqual(TEXT("A healthy project produces no findings"), Findings.Num(), 0);
	TestFalse(TEXT("...and therefore no errors"), FFlockSetupStatus::HasErrors(Findings));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSetupStatusRequiredFieldsTest, "Flock.Editor.Setup.Status.RequiredFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSetupStatusRequiredFieldsTest::RunTest(const FString& Parameters)
{
	{
		FFlockSetupInput Input = Healthy();
		Input.ApiUrl.Empty();
		const TArray<FFlockSetupFinding> Findings = FFlockSetupStatus::Evaluate(Input);
		TestTrue(TEXT("Empty API URL reports"), Has(Findings, TEXT("Flock.Config.ApiUrl")));
		TestTrue(TEXT("...as an error"), FFlockSetupStatus::HasErrors(Findings));
	}

	{
		// A scheme-less URL fails at the first request as an opaque transport error, so it is caught here.
		FFlockSetupInput Input = Healthy();
		Input.ApiUrl = TEXT("api-flock.qwacks.com");
		TestTrue(TEXT("Scheme-less API URL reports"),
			Has(FFlockSetupStatus::Evaluate(Input), TEXT("Flock.Config.ApiUrl")));
	}

	{
		FFlockSetupInput Input = Healthy();
		Input.ApiKey.Empty();
		TestTrue(TEXT("Empty API key reports"),
			Has(FFlockSetupStatus::Evaluate(Input), TEXT("Flock.Config.ApiKey")));
	}

	{
		FFlockSetupInput Input = Healthy();
		Input.GameId.Empty();
		TestTrue(TEXT("Empty game name reports"),
			Has(FFlockSetupStatus::Evaluate(Input), TEXT("Flock.Config.GameId")));
	}

	{
		FFlockSetupInput Input = Healthy();
		Input.GameVersion.Empty();
		TestTrue(TEXT("Empty game version reports"),
			Has(FFlockSetupStatus::Evaluate(Input), TEXT("Flock.Config.GameVersion")));
	}

	{
		// Whitespace is not configuration.
		FFlockSetupInput Input = Healthy();
		Input.ApiKey = TEXT("   ");
		TestTrue(TEXT("Whitespace-only API key reports"),
			Has(FFlockSetupStatus::Evaluate(Input), TEXT("Flock.Config.ApiKey")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSetupStatusBlockedFindingTest, "Flock.Editor.Setup.Status.HidesBlockedFindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSetupStatusBlockedFindingTest::RunTest(const FString& Parameters)
{
	{
		FFlockSetupInput Input = Healthy();
		Input.GameVersionId.Empty();
		TestTrue(TEXT("A complete config with no baked ID reports unresolved"),
			Has(FFlockSetupStatus::Evaluate(Input), TEXT("Flock.Version.Unresolved")));
	}

	{
		// Resolving needs all four required fields, so "unresolved" alongside "no API key" is noise:
		// the developer cannot act on it until the key is set.
		FFlockSetupInput Input = Healthy();
		Input.GameVersionId.Empty();
		Input.ApiKey.Empty();
		const TArray<FFlockSetupFinding> Findings = FFlockSetupStatus::Evaluate(Input);
		TestTrue(TEXT("The blocking finding is reported"), Has(Findings, TEXT("Flock.Config.ApiKey")));
		TestFalse(TEXT("The blocked finding is withheld"), Has(Findings, TEXT("Flock.Version.Unresolved")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSetupStatusProbeTest, "Flock.Editor.Setup.Status.ProbeStates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSetupStatusProbeTest::RunTest(const FString& Parameters)
{
	// Silence until someone asks. A probe never runs on editor startup, so NotRun must say nothing.
	{
		FFlockSetupInput Input = Healthy();
		Input.Probe = EFlockProbeState::NotRun;
		TestEqual(TEXT("An unrun probe reports nothing"), FFlockSetupStatus::Evaluate(Input).Num(), 0);
	}

	struct FCase
	{
		EFlockProbeState State;
		const TCHAR* Id;
	};

	const FCase Cases[] = {
		{ EFlockProbeState::Unreachable,     TEXT("Flock.Connection.Unreachable") },
		{ EFlockProbeState::KeyRejected,     TEXT("Flock.Connection.KeyRejected") },
		{ EFlockProbeState::VersionNotFound, TEXT("Flock.Connection.VersionNotFound") },
		{ EFlockProbeState::Failed,          TEXT("Flock.Connection.Failed") },
	};

	for (const FCase& Case : Cases)
	{
		FFlockSetupInput Input = Healthy();
		Input.Probe = Case.State;
		Input.ProbeMessage = TEXT("server said no");
		const TArray<FFlockSetupFinding> Findings = FFlockSetupStatus::Evaluate(Input);
		TestTrue(FString::Printf(TEXT("Probe state reports %s"), Case.Id), Has(Findings, Case.Id));
		TestTrue(TEXT("...as an error"), FFlockSetupStatus::HasErrors(Findings));
	}

	// An undiagnosable failure surfaces the server's own wording rather than a guess.
	{
		FFlockSetupInput Input = Healthy();
		Input.Probe = EFlockProbeState::Failed;
		Input.ProbeMessage = TEXT("gateway timeout");
		// Named local: Find returns a pointer into this array, so it must outlive the lookup.
		const TArray<FFlockSetupFinding> Findings = FFlockSetupStatus::Evaluate(Input);
		const FFlockSetupFinding* Finding = Find(Findings, TEXT("Flock.Connection.Failed"));
		if (Finding)
		{
			TestEqual(TEXT("Carries the server message verbatim"), Finding->Detail.ToString(), FString(TEXT("gateway timeout")));
		}
		else
		{
			AddError(TEXT("Expected a Flock.Connection.Failed finding"));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSetupStatusGameNameTest, "Flock.Editor.Setup.Status.GameNameMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSetupStatusGameNameTest::RunTest(const FString& Parameters)
{
	{
		FFlockSetupInput Input = Healthy();
		Input.ProbeServerGameName = TEXT("Some Other Game");
		const TArray<FFlockSetupFinding> Findings = FFlockSetupStatus::Evaluate(Input);
		TestTrue(TEXT("A mismatched game name reports"), Has(Findings, TEXT("Flock.Connection.GameNameMismatch")));

		// The name is never sent to the server, so the SDK initializes fine — it cannot be an Error under
		// the severity policy, or auto-summon would fire for something that is not blocking anything.
		TestFalse(TEXT("...as a warning, not an error"), FFlockSetupStatus::HasErrors(Findings));
	}

	{
		// Nothing to compare against until a probe has read the game record.
		FFlockSetupInput Input = Healthy();
		Input.ProbeServerGameName.Empty();
		TestFalse(TEXT("No server name means no mismatch finding"),
			Has(FFlockSetupStatus::Evaluate(Input), TEXT("Flock.Connection.GameNameMismatch")));
	}

	{
		// Surrounding whitespace in settings is not a mismatch.
		FFlockSetupInput Input = Healthy();
		Input.GameId = TEXT("  Duck Odyssey  ");
		Input.ProbeServerGameName = TEXT("Duck Odyssey");
		TestFalse(TEXT("Whitespace does not count as a mismatch"),
			Has(FFlockSetupStatus::Evaluate(Input), TEXT("Flock.Connection.GameNameMismatch")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSetupStatusDriftTest, "Flock.Editor.Setup.Status.CodegenDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSetupStatusDriftTest::RunTest(const FString& Parameters)
{
	{
		FFlockSetupInput Input = Healthy();
		Input.CodegenManifestVersionId = TEXT("ver-old");
		const TArray<FFlockSetupFinding> Findings = FFlockSetupStatus::Evaluate(Input);
		TestTrue(TEXT("A stale manifest reports drift"), Has(Findings, TEXT("Flock.Codegen.Drift")));
		TestFalse(TEXT("Drift is not an error — the SDK still runs"), FFlockSetupStatus::HasErrors(Findings));
	}

	{
		// A project that never ran codegen must hear nothing about it.
		FFlockSetupInput Input = Healthy();
		Input.bCodegenManifestPresent = false;
		Input.CodegenManifestVersionId.Empty();
		TestFalse(TEXT("No manifest means no drift finding"),
			Has(FFlockSetupStatus::Evaluate(Input), TEXT("Flock.Codegen.Drift")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSetupStatusSdkVersionTest, "Flock.Editor.Setup.Status.SdkUpgrade",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSetupStatusSdkVersionTest::RunTest(const FString& Parameters)
{
	{
		FFlockSetupInput Input = Healthy();
		Input.LastSeenSdkVersion = TEXT("0.15.0");
		TestTrue(TEXT("A changed SDK version reports"),
			Has(FFlockSetupStatus::Evaluate(Input), TEXT("Flock.Sdk.Upgraded")));
	}

	{
		// A first add has no last-seen record and is not an upgrade — it must not read as one.
		FFlockSetupInput Input = Healthy();
		Input.LastSeenSdkVersion.Empty();
		TestFalse(TEXT("A first add is not an upgrade"),
			Has(FFlockSetupStatus::Evaluate(Input), TEXT("Flock.Sdk.Upgraded")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSetupStatusOrderingTest, "Flock.Editor.Setup.Status.OrdersBySeverity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSetupStatusOrderingTest::RunTest(const FString& Parameters)
{
	// One of each severity at once: Info (upgrade), Warning (drift), Error (missing key).
	FFlockSetupInput Input = Healthy();
	Input.ApiKey.Empty();
	Input.CodegenManifestVersionId = TEXT("ver-old");
	Input.LastSeenSdkVersion = TEXT("0.15.0");

	const TArray<FFlockSetupFinding> Findings = FFlockSetupStatus::Evaluate(Input);
	if (Findings.Num() < 3)
	{
		AddError(FString::Printf(TEXT("Expected at least 3 findings, got %d"), Findings.Num()));
		return false;
	}

	for (int32 Index = 1; Index < Findings.Num(); ++Index)
	{
		TestTrue(TEXT("Findings are ordered most severe first"),
			Findings[Index - 1].Severity >= Findings[Index].Severity);
	}

	// AtLeast is how the PIE guard and the packaging validator narrow to what blocks them.
	const TArray<FFlockSetupFinding> Errors = FFlockSetupStatus::AtLeast(Findings, EFlockSetupSeverity::Error);
	TestTrue(TEXT("AtLeast(Error) keeps the error"), Has(Errors, TEXT("Flock.Config.ApiKey")));
	TestFalse(TEXT("AtLeast(Error) drops the warning"), Has(Errors, TEXT("Flock.Codegen.Drift")));
	TestFalse(TEXT("AtLeast(Error) drops the info"), Has(Errors, TEXT("Flock.Sdk.Upgraded")));

	const TArray<FFlockSetupFinding> WarnUp = FFlockSetupStatus::AtLeast(Findings, EFlockSetupSeverity::Warning);
	TestTrue(TEXT("AtLeast(Warning) keeps the warning"), Has(WarnUp, TEXT("Flock.Codegen.Drift")));
	TestFalse(TEXT("AtLeast(Warning) drops the info"), Has(WarnUp, TEXT("Flock.Sdk.Upgraded")));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
