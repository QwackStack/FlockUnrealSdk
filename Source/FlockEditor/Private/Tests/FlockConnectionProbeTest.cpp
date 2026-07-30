// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Setup/FlockConnectionProbe.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConnectionProbeClassifyTest, "Flock.Editor.Setup.Probe.Classify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConnectionProbeClassifyTest::RunTest(const FString& Parameters)
{
	// No HTTP response at all — the one case where "check your API URL" is fair advice.
	TestTrue(TEXT("A transport failure is Unreachable"),
		FFlockConnectionProbe::Classify(FFlockError::Make(EFlockErrorType::Connection, TEXT("refused")))
			== EFlockProbeState::Unreachable);

	TestTrue(TEXT("An Auth failure is a rejected key"),
		FFlockConnectionProbe::Classify(FFlockError::Make(EFlockErrorType::Auth, TEXT("nope"), 401))
			== EFlockProbeState::KeyRejected);

	// 401/403 that arrived classified as something else still mean the key was refused.
	TestTrue(TEXT("A bare 401 is a rejected key"),
		FFlockConnectionProbe::Classify(FFlockError::Make(EFlockErrorType::Network, TEXT("nope"), 401))
			== EFlockProbeState::KeyRejected);
	TestTrue(TEXT("A bare 403 is a rejected key"),
		FFlockConnectionProbe::Classify(FFlockError::Make(EFlockErrorType::Network, TEXT("nope"), 403))
			== EFlockProbeState::KeyRejected);

	// The game is identified by the API key and is never part of this request, so a 404 here can only
	// mean the version name did not match.
	TestTrue(TEXT("A 404 is a missing version name"),
		FFlockConnectionProbe::Classify(FFlockError::Make(EFlockErrorType::Network, TEXT("missing"), 404))
			== EFlockProbeState::VersionNotFound);

	// A timeout must not read as Unreachable: that would send a developer to edit an API URL which is
	// very likely correct.
	TestTrue(TEXT("A timeout is not reported as Unreachable"),
		FFlockConnectionProbe::Classify(FFlockError::Make(EFlockErrorType::Timeout, TEXT("timed out")))
			== EFlockProbeState::Failed);

	TestTrue(TEXT("A 500 falls back to Failed"),
		FFlockConnectionProbe::Classify(FFlockError::Make(EFlockErrorType::Network, TEXT("boom"), 500))
			== EFlockProbeState::Failed);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
