// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockPlaytestStatus.h"
#include "Tests/FlockPlaytestTestSupport.h"

namespace
{
	FFlockPlaytestStatusInputs MakeInputs(bool bPlaytestingEnabled, const FString& ProtokiteApiUrl, bool bFlockInitialized,
		EFlockPlaytestConfigState ConfigState = EFlockPlaytestConfigState::Loaded)
	{
		FFlockPlaytestStatusInputs Inputs;
		Inputs.bPlaytestingEnabled = bPlaytestingEnabled;
		Inputs.ProtokiteApiUrl = ProtokiteApiUrl;
		Inputs.bFlockInitialized = bFlockInitialized;
		Inputs.ConfigState = ConfigState;
		return Inputs;
	}

	TFlockResult<FFlockPlaytestConfig> FailedWithStatus(int32 StatusCode)
	{
		return TFlockResult<FFlockPlaytestConfig>::Fail(FFlockError::Make(EFlockErrorType::Network, TEXT("failed"), StatusCode));
	}

	TFlockResult<FFlockPlaytestConfig> LoadedForVersion(const FString& FlockGameVersionId)
	{
		FFlockPlaytestConfig Config;
		Config.TestId = TEXT("t1");
		Config.FlockGameVersionId = FlockGameVersionId;
		return TFlockResult<FFlockPlaytestConfig>::Ok(Config);
	}

	void ExpectConfigState(FAutomationTestBase& Test, const FString& What, EFlockPlaytestConfigState Actual,
		EFlockPlaytestConfigState Expected)
	{
		Test.TestEqual(What, static_cast<int32>(Actual), static_cast<int32>(Expected));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestStatusTurnedOffOutranksEverythingTest,
	"Flock.Playtest.Status.TurnedOffOutranksEverything",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestStatusTurnedOffOutranksEverythingTest::RunTest(const FString& Parameters)
{
	// With the switch off nothing else is looked at, so none of these can surface a URL, Flock or config answer.
	ExpectPlaytestStatus(*this, TEXT("Off, empty URL, Flock not initialized"),
		DecidePlaytestStatus(MakeInputs(false, TEXT(""), false)), EFlockPlaytestStatus::TurnedOff);
	ExpectPlaytestStatus(*this, TEXT("Off, unusable URL, Flock initialized"),
		DecidePlaytestStatus(MakeInputs(false, TEXT("localhost:8020"), true)), EFlockPlaytestStatus::TurnedOff);
	ExpectPlaytestStatus(*this, TEXT("Off, usable URL, Flock initialized, config loaded"),
		DecidePlaytestStatus(MakeInputs(false, TEXT("http://localhost:8020"), true)), EFlockPlaytestStatus::TurnedOff);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestStatusProtokiteApiUrlMustBeUsableTest,
	"Flock.Playtest.Status.ProtokiteApiUrlMustBeUsable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestStatusProtokiteApiUrlMustBeUsableTest::RunTest(const FString& Parameters)
{
	const TCHAR* const UsableUrls[] = {
		TEXT("http://localhost:8020"),
		TEXT("https://protokite.example.com"),
		TEXT("HTTPS://PROTOKITE.EXAMPLE.COM"),
		TEXT("http://localhost:8020/"),
	};
	for (const TCHAR* Url : UsableUrls)
	{
		TestTrue(FString::Printf(TEXT("'%s' is usable"), Url), IsUsableProtokiteApiUrl(Url));
		ExpectPlaytestStatus(*this, FString::Printf(TEXT("'%s' with Flock initialized and the config loaded"), Url),
			DecidePlaytestStatus(MakeInputs(true, Url, true)), EFlockPlaytestStatus::Ready);
	}

	// Whitespace is refused rather than trimmed: a pasted space or line break has to be fixed in the setting.
	const TCHAR* const UnusableUrls[] = {
		TEXT("localhost:8020"),
		TEXT("ftp://protokite.example.com"),
		TEXT("http://"),
		TEXT("http:///game/sdk"),
		TEXT("http://:8020"),
		TEXT(" http://localhost:8020"),
		TEXT("http://localhost:8020 "),
		TEXT("http://localhost:8020\n"),
		TEXT("http://local host:8020"),
	};
	for (const TCHAR* Url : UnusableUrls)
	{
		TestFalse(FString::Printf(TEXT("'%s' is not usable"), Url), IsUsableProtokiteApiUrl(Url));
		ExpectPlaytestStatus(*this, FString::Printf(TEXT("'%s' with Flock initialized"), Url),
			DecidePlaytestStatus(MakeInputs(true, Url, true)), EFlockPlaytestStatus::ProtokiteApiUrlUnusable);
	}

	// Empty is its own answer: nothing was set, rather than something set wrong.
	ExpectPlaytestStatus(*this, TEXT("Empty URL with Flock initialized"),
		DecidePlaytestStatus(MakeInputs(true, TEXT(""), true)), EFlockPlaytestStatus::ProtokiteApiUrlMissing);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestStatusWaitsForFlockOnlyOnceSettingsAreCompleteTest,
	"Flock.Playtest.Status.WaitsForFlockOnlyOnceSettingsAreComplete",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestStatusWaitsForFlockOnlyOnceSettingsAreCompleteTest::RunTest(const FString& Parameters)
{
	// A settings mistake is reported while the Flock SDK is still down, not hidden behind waiting for it.
	ExpectPlaytestStatus(*this, TEXT("Empty URL, Flock not initialized"),
		DecidePlaytestStatus(MakeInputs(true, TEXT(""), false)), EFlockPlaytestStatus::ProtokiteApiUrlMissing);
	ExpectPlaytestStatus(*this, TEXT("Unusable URL, Flock not initialized"),
		DecidePlaytestStatus(MakeInputs(true, TEXT("localhost:8020"), false)), EFlockPlaytestStatus::ProtokiteApiUrlUnusable);

	ExpectPlaytestStatus(*this, TEXT("Complete settings, Flock not initialized, even with a config"),
		DecidePlaytestStatus(MakeInputs(true, TEXT("http://localhost:8020"), false)), EFlockPlaytestStatus::WaitingForFlock);
	ExpectPlaytestStatus(*this, TEXT("Complete settings, Flock initialized, config loaded"),
		DecidePlaytestStatus(MakeInputs(true, TEXT("http://localhost:8020"), true)), EFlockPlaytestStatus::Ready);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestStatusConfigStateDecidesOnceFlockIsUpTest,
	"Flock.Playtest.Status.ConfigStateDecidesOnceFlockIsUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestStatusConfigStateDecidesOnceFlockIsUpTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		EFlockPlaytestConfigState ConfigState;
		EFlockPlaytestStatus Expected;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{ EFlockPlaytestConfigState::NotFetched, EFlockPlaytestStatus::FetchingPlaytestConfig, TEXT("Not fetched yet") },
		{ EFlockPlaytestConfigState::Fetching, EFlockPlaytestStatus::FetchingPlaytestConfig, TEXT("Fetching") },
		{ EFlockPlaytestConfigState::Loaded, EFlockPlaytestStatus::Ready, TEXT("Loaded") },
		{ EFlockPlaytestConfigState::PlaytestNotLinked, EFlockPlaytestStatus::PlaytestNotLinked, TEXT("No linked playtest") },
		{ EFlockPlaytestConfigState::ApiKeyRefused, EFlockPlaytestStatus::ProtokiteRefusedApiKey, TEXT("Key refused") },
		{ EFlockPlaytestConfigState::Unavailable, EFlockPlaytestStatus::PlaytestConfigUnavailable, TEXT("Unavailable") },
		{ EFlockPlaytestConfigState::ForAnotherVersion, EFlockPlaytestStatus::PlaytestConfigForAnotherVersion, TEXT("Another version") },
	};
	for (const FCase& Case : Cases)
	{
		ExpectPlaytestStatus(*this, Case.What,
			DecidePlaytestStatus(MakeInputs(true, TEXT("http://localhost:8020"), true, Case.ConfigState)), Case.Expected);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestStatusRefusalsDecideTheConfigStateTest,
	"Flock.Playtest.Status.RefusalsDecideTheConfigState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestStatusRefusalsDecideTheConfigStateTest::RunTest(const FString& Parameters)
{
	const FString Sent = TEXT("pt-test-version");
	ExpectConfigState(*this, TEXT("404 is no linked playtest"), DecidePlaytestConfigState(FailedWithStatus(404), Sent),
		EFlockPlaytestConfigState::PlaytestNotLinked);
	ExpectConfigState(*this, TEXT("401 is a refused key"), DecidePlaytestConfigState(FailedWithStatus(401), Sent),
		EFlockPlaytestConfigState::ApiKeyRefused);
	// Protokite never sends 403 on this route, so one came from something on the way and the key is not blamed.
	ExpectConfigState(*this, TEXT("403 is unavailable"), DecidePlaytestConfigState(FailedWithStatus(403), Sent),
		EFlockPlaytestConfigState::Unavailable);
	ExpectConfigState(*this, TEXT("422 is a request without a key"), DecidePlaytestConfigState(FailedWithStatus(422), Sent),
		EFlockPlaytestConfigState::ApiKeyRefused);
	ExpectConfigState(*this, TEXT("503 is unavailable"), DecidePlaytestConfigState(FailedWithStatus(503), Sent),
		EFlockPlaytestConfigState::Unavailable);
	ExpectConfigState(*this, TEXT("500 is unavailable"), DecidePlaytestConfigState(FailedWithStatus(500), Sent),
		EFlockPlaytestConfigState::Unavailable);
	ExpectConfigState(*this, TEXT("No answer at all is unavailable"), DecidePlaytestConfigState(FailedWithStatus(0), Sent),
		EFlockPlaytestConfigState::Unavailable);
	ExpectConfigState(*this, TEXT("A 200 that could not be read is unavailable"),
		DecidePlaytestConfigState(FailedWithStatus(200), Sent), EFlockPlaytestConfigState::Unavailable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestStatusAnswerForAnotherVersionIsNotLoadedTest,
	"Flock.Playtest.Status.AnswerForAnotherVersionIsNotLoaded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestStatusAnswerForAnotherVersionIsNotLoadedTest::RunTest(const FString& Parameters)
{
	const FString Sent = TEXT("pt-test-version");
	ExpectConfigState(*this, TEXT("The version this build sent is loaded"),
		DecidePlaytestConfigState(LoadedForVersion(Sent), Sent), EFlockPlaytestConfigState::Loaded);
	ExpectConfigState(*this, TEXT("Another version is not loaded"),
		DecidePlaytestConfigState(LoadedForVersion(TEXT("pt-other-version")), Sent), EFlockPlaytestConfigState::ForAnotherVersion);
	ExpectConfigState(*this, TEXT("A version that differs only in letter case is another version"),
		DecidePlaytestConfigState(LoadedForVersion(TEXT("PT-TEST-VERSION")), Sent), EFlockPlaytestConfigState::ForAnotherVersion);
	ExpectConfigState(*this, TEXT("No version named leaves nothing to compare"),
		DecidePlaytestConfigState(LoadedForVersion(FString()), Sent), EFlockPlaytestConfigState::Loaded);
	return true;
}

#endif
