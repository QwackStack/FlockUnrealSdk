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

	// The answers the local backend sent on 2026-10-15: a wrong key and an unknown version name are told apart by code.
	TestTrue(TEXT("An unknown API key is a rejected key"),
		FFlockConnectionProbe::Classify(FFlockError::Make(EFlockErrorType::Auth, TEXT("Invalid API Key"), 401, FString(),
			TEXT("game.invalid_api_key"), TEXT("Invalid API Key"))) == EFlockProbeState::KeyRejected);
	TestTrue(TEXT("A blank API key is a rejected key"),
		FFlockConnectionProbe::Classify(FFlockError::Make(EFlockErrorType::Auth, TEXT("Missing X-Flock-API-Key header"), 401, FString(),
			TEXT("game.missing_api_key"), TEXT("Missing X-Flock-API-Key header"))) == EFlockProbeState::KeyRejected);
	TestTrue(TEXT("An unknown version name is a missing version"),
		FFlockConnectionProbe::Classify(FFlockError::Make(EFlockErrorType::Network, TEXT("Game version with name 'x' not found"), 404,
			FString(), TEXT("game_version.game_version_by_name_not_found"))) == EFlockProbeState::VersionNotFound);

	// A wrong API URL answers 404 with request.not_found (the URL ends in /v1) or 401 with no code (the URL misses
	// the server's path): neither is the version or the key, and each reports its own reason and fix instead.
	TestTrue(TEXT("A route that does not exist is not a missing version"),
		FFlockConnectionProbe::Classify(FFlockError::Make(EFlockErrorType::Network, TEXT("Not Found"), 404, FString(),
			TEXT("request.not_found"), TEXT("Not Found"))) == EFlockProbeState::Failed);
	TestTrue(TEXT("A 401 without a code is not a rejected key"),
		FFlockConnectionProbe::Classify(FFlockError::Make(EFlockErrorType::Auth, TEXT("Invalid token."), 401, FString(),
			FString(), TEXT("Invalid token."))) == EFlockProbeState::Failed);
	TestTrue(TEXT("A 403 without a code is not a rejected key"),
		FFlockConnectionProbe::Classify(FFlockError::Make(EFlockErrorType::Auth, TEXT("The server refused to do this"), 403))
			== EFlockProbeState::Failed);

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
