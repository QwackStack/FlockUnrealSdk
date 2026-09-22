// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "ProtokitePlaytestStatus.h"
#include "Tests/ProtokitePlaytestTestSupport.h"

namespace
{
	/**
	 * Inputs with a player who has already said the playtest may collect everything, so each test below is about the
	 * one thing it varies. The answer has a band of its own, which the test at the end of this file covers.
	 */
	FProtokitePlaytestStatusInputs MakeInputs(bool bPlaytestingEnabled, const FString& ProtokiteApiUrl, bool bFlockInitialized,
		EProtokitePlaytestConfigState ConfigState = EProtokitePlaytestConfigState::Loaded,
		EProtokitePlaytestConsentChoice PlayerConsent = EProtokitePlaytestConsentChoice::VideoAndPlayData)
	{
		FProtokitePlaytestStatusInputs Inputs;
		Inputs.bPlaytestingEnabled = bPlaytestingEnabled;
		Inputs.ProtokiteApiUrl = ProtokiteApiUrl;
		Inputs.bFlockInitialized = bFlockInitialized;
		Inputs.ConfigState = ConfigState;
		Inputs.PlayerConsent = PlayerConsent;
		return Inputs;
	}

	TFlockResult<FProtokitePlaytestConfig> FailedWithStatus(int32 StatusCode)
	{
		return TFlockResult<FProtokitePlaytestConfig>::Fail(FFlockError::Make(EFlockErrorType::Network, TEXT("failed"), StatusCode));
	}

	TFlockResult<FProtokitePlaytestConfig> LoadedForVersion(const FString& FlockGameVersionId)
	{
		FProtokitePlaytestConfig Config;
		Config.TestId = TEXT("t1");
		Config.FlockGameVersionId = FlockGameVersionId;
		return TFlockResult<FProtokitePlaytestConfig>::Ok(Config);
	}

	void ExpectConfigState(FAutomationTestBase& Test, const FString& What, EProtokitePlaytestConfigState Actual,
		EProtokitePlaytestConfigState Expected)
	{
		Test.TestEqual(What, static_cast<int32>(Actual), static_cast<int32>(Expected));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestStatusTurnedOffOutranksEverythingTest,
	"Protokite.Playtest.Status.TurnedOffOutranksEverything",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestStatusTurnedOffOutranksEverythingTest::RunTest(const FString& Parameters)
{
	// With the switch off nothing else is looked at, so none of these can surface a URL, Flock or config answer.
	ExpectPlaytestStatus(*this, TEXT("Off, empty URL, Flock not initialized"),
		DecidePlaytestStatus(MakeInputs(false, TEXT(""), false)), EProtokitePlaytestStatus::TurnedOff);
	ExpectPlaytestStatus(*this, TEXT("Off, unusable URL, Flock initialized"),
		DecidePlaytestStatus(MakeInputs(false, TEXT("localhost:8020"), true)), EProtokitePlaytestStatus::TurnedOff);
	ExpectPlaytestStatus(*this, TEXT("Off, usable URL, Flock initialized, config loaded"),
		DecidePlaytestStatus(MakeInputs(false, TEXT("http://localhost:8020"), true)), EProtokitePlaytestStatus::TurnedOff);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestStatusProtokiteApiUrlMustBeUsableTest,
	"Protokite.Playtest.Status.ProtokiteApiUrlMustBeUsable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestStatusProtokiteApiUrlMustBeUsableTest::RunTest(const FString& Parameters)
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
			DecidePlaytestStatus(MakeInputs(true, Url, true)), EProtokitePlaytestStatus::Ready);
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
			DecidePlaytestStatus(MakeInputs(true, Url, true)), EProtokitePlaytestStatus::ProtokiteApiUrlUnusable);
	}

	// Empty is its own answer: nothing was set, rather than something set wrong.
	ExpectPlaytestStatus(*this, TEXT("Empty URL with Flock initialized"),
		DecidePlaytestStatus(MakeInputs(true, TEXT(""), true)), EProtokitePlaytestStatus::ProtokiteApiUrlMissing);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestStatusWaitsForFlockOnlyOnceSettingsAreCompleteTest,
	"Protokite.Playtest.Status.WaitsForFlockOnlyOnceSettingsAreComplete",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestStatusWaitsForFlockOnlyOnceSettingsAreCompleteTest::RunTest(const FString& Parameters)
{
	// A settings mistake is reported while the Flock SDK is still down, not hidden behind waiting for it.
	ExpectPlaytestStatus(*this, TEXT("Empty URL, Flock not initialized"),
		DecidePlaytestStatus(MakeInputs(true, TEXT(""), false)), EProtokitePlaytestStatus::ProtokiteApiUrlMissing);
	ExpectPlaytestStatus(*this, TEXT("Unusable URL, Flock not initialized"),
		DecidePlaytestStatus(MakeInputs(true, TEXT("localhost:8020"), false)), EProtokitePlaytestStatus::ProtokiteApiUrlUnusable);

	ExpectPlaytestStatus(*this, TEXT("Complete settings, Flock not initialized, even with a config"),
		DecidePlaytestStatus(MakeInputs(true, TEXT("http://localhost:8020"), false)), EProtokitePlaytestStatus::WaitingForFlock);
	ExpectPlaytestStatus(*this, TEXT("Complete settings, Flock initialized, config loaded"),
		DecidePlaytestStatus(MakeInputs(true, TEXT("http://localhost:8020"), true)), EProtokitePlaytestStatus::Ready);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestStatusConfigStateDecidesOnceFlockIsUpTest,
	"Protokite.Playtest.Status.ConfigStateDecidesOnceFlockIsUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestStatusConfigStateDecidesOnceFlockIsUpTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		EProtokitePlaytestConfigState ConfigState;
		EProtokitePlaytestStatus Expected;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{ EProtokitePlaytestConfigState::NotFetched, EProtokitePlaytestStatus::FetchingPlaytestConfig, TEXT("Not fetched yet") },
		{ EProtokitePlaytestConfigState::Fetching, EProtokitePlaytestStatus::FetchingPlaytestConfig, TEXT("Fetching") },
		{ EProtokitePlaytestConfigState::Loaded, EProtokitePlaytestStatus::Ready, TEXT("Loaded") },
		{ EProtokitePlaytestConfigState::PlaytestNotLinked, EProtokitePlaytestStatus::PlaytestNotLinked, TEXT("No linked playtest") },
		{ EProtokitePlaytestConfigState::ApiKeyRefused, EProtokitePlaytestStatus::ProtokiteRefusedApiKey, TEXT("Key refused") },
		{ EProtokitePlaytestConfigState::Unavailable, EProtokitePlaytestStatus::PlaytestConfigUnavailable, TEXT("Unavailable") },
		{ EProtokitePlaytestConfigState::ForAnotherVersion, EProtokitePlaytestStatus::PlaytestConfigForAnotherVersion, TEXT("Another version") },
	};
	for (const FCase& Case : Cases)
	{
		ExpectPlaytestStatus(*this, Case.What,
			DecidePlaytestStatus(MakeInputs(true, TEXT("http://localhost:8020"), true, Case.ConfigState)), Case.Expected);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestStatusRefusalsDecideTheConfigStateTest,
	"Protokite.Playtest.Status.RefusalsDecideTheConfigState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestStatusRefusalsDecideTheConfigStateTest::RunTest(const FString& Parameters)
{
	const FString Sent = TEXT("pt-test-version");
	ExpectConfigState(*this, TEXT("404 is no linked playtest"), DecidePlaytestConfigState(FailedWithStatus(404), Sent),
		EProtokitePlaytestConfigState::PlaytestNotLinked);
	ExpectConfigState(*this, TEXT("401 is a refused key"), DecidePlaytestConfigState(FailedWithStatus(401), Sent),
		EProtokitePlaytestConfigState::ApiKeyRefused);
	// Protokite never sends 403 on this route, so one came from something on the way and the key is not blamed.
	ExpectConfigState(*this, TEXT("403 is unavailable"), DecidePlaytestConfigState(FailedWithStatus(403), Sent),
		EProtokitePlaytestConfigState::Unavailable);
	ExpectConfigState(*this, TEXT("422 is a request without a key"), DecidePlaytestConfigState(FailedWithStatus(422), Sent),
		EProtokitePlaytestConfigState::ApiKeyRefused);
	ExpectConfigState(*this, TEXT("503 is unavailable"), DecidePlaytestConfigState(FailedWithStatus(503), Sent),
		EProtokitePlaytestConfigState::Unavailable);
	ExpectConfigState(*this, TEXT("500 is unavailable"), DecidePlaytestConfigState(FailedWithStatus(500), Sent),
		EProtokitePlaytestConfigState::Unavailable);
	ExpectConfigState(*this, TEXT("No answer at all is unavailable"), DecidePlaytestConfigState(FailedWithStatus(0), Sent),
		EProtokitePlaytestConfigState::Unavailable);
	ExpectConfigState(*this, TEXT("A 200 that could not be read is unavailable"),
		DecidePlaytestConfigState(FailedWithStatus(200), Sent), EProtokitePlaytestConfigState::Unavailable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestStatusAnswerForAnotherVersionIsNotLoadedTest,
	"Protokite.Playtest.Status.AnswerForAnotherVersionIsNotLoaded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestStatusAnswerForAnotherVersionIsNotLoadedTest::RunTest(const FString& Parameters)
{
	const FString Sent = TEXT("pt-test-version");
	ExpectConfigState(*this, TEXT("The version this build sent is loaded"),
		DecidePlaytestConfigState(LoadedForVersion(Sent), Sent), EProtokitePlaytestConfigState::Loaded);
	ExpectConfigState(*this, TEXT("Another version is not loaded"),
		DecidePlaytestConfigState(LoadedForVersion(TEXT("pt-other-version")), Sent), EProtokitePlaytestConfigState::ForAnotherVersion);
	ExpectConfigState(*this, TEXT("A version that differs only in letter case is another version"),
		DecidePlaytestConfigState(LoadedForVersion(TEXT("PT-TEST-VERSION")), Sent), EProtokitePlaytestConfigState::ForAnotherVersion);
	ExpectConfigState(*this, TEXT("No version named leaves nothing to compare"),
		DecidePlaytestConfigState(LoadedForVersion(FString()), Sent), EProtokitePlaytestConfigState::Loaded);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestStatusClosedPlaytestOutranksFlockAndConfigTest,
	"Protokite.Playtest.Status.ClosedPlaytestOutranksFlockAndConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestStatusClosedPlaytestOutranksFlockAndConfigTest::RunTest(const FString& Parameters)
{
	const auto Closed = [](bool bPlaytestingEnabled, const FString& ProtokiteApiUrl, bool bFlockInitialized,
		EProtokitePlaytestConfigState ConfigState)
	{
		FProtokitePlaytestStatusInputs Inputs = MakeInputs(bPlaytestingEnabled, ProtokiteApiUrl, bFlockInitialized, ConfigState);
		Inputs.bPlaytestNoLongerCollecting = true;
		return DecidePlaytestStatus(Inputs);
	};
	const FString Url = TEXT("http://localhost:8020");

	ExpectPlaytestStatus(*this, TEXT("Closed, with the config loaded"), Closed(true, Url, true, EProtokitePlaytestConfigState::Loaded),
		EProtokitePlaytestStatus::PlaytestNoLongerCollecting);
	ExpectPlaytestStatus(*this, TEXT("Closed, with the Flock SDK shut down"), Closed(true, Url, false, EProtokitePlaytestConfigState::NotFetched),
		EProtokitePlaytestStatus::PlaytestNoLongerCollecting);
	ExpectPlaytestStatus(*this, TEXT("Closed, with the config unavailable"), Closed(true, Url, true, EProtokitePlaytestConfigState::Unavailable),
		EProtokitePlaytestStatus::PlaytestNoLongerCollecting);
	ExpectPlaytestStatus(*this, TEXT("Turned off still says turned off"), Closed(false, Url, true, EProtokitePlaytestConfigState::Loaded),
		EProtokitePlaytestStatus::TurnedOff);
	ExpectPlaytestStatus(*this, TEXT("A missing URL is still reported first"), Closed(true, TEXT(""), true, EProtokitePlaytestConfigState::Loaded),
		EProtokitePlaytestStatus::ProtokiteApiUrlMissing);
	TestFalse(TEXT("It has a description"), DescribePlaytestStatus(EProtokitePlaytestStatus::PlaytestNoLongerCollecting).IsEmpty());
	return true;
}

/**
 * What the player allowed is read last, once a playtest is actually loaded. Asking earlier would put a question in
 * front of a player in a build that is misconfigured, or that no playtest is linked to, with nothing to ask about.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestStatusPlayerConsentDecidesLastTest,
	"Protokite.Playtest.Status.WhatThePlayerAllowedIsReadLast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestStatusPlayerConsentDecidesLastTest::RunTest(const FString& Parameters)
{
	const FString Url = TEXT("http://localhost:8020");
	const auto With = [&Url](EProtokitePlaytestConsentChoice Choice, EProtokitePlaytestConfigState ConfigState = EProtokitePlaytestConfigState::Loaded)
	{
		return DecidePlaytestStatus(MakeInputs(true, Url, true, ConfigState, Choice));
	};

	ExpectPlaytestStatus(*this, TEXT("Nobody has answered"), With(EProtokitePlaytestConsentChoice::NotAnswered),
		EProtokitePlaytestStatus::WaitingForPlayerConsent);
	ExpectPlaytestStatus(*this, TEXT("They asked for nothing to be collected"), With(EProtokitePlaytestConsentChoice::Nothing),
		EProtokitePlaytestStatus::PlayerRefusedPlaytest);

	// Either half on its own is still a playtest that runs: which half is honoured feature by feature, not here.
	ExpectPlaytestStatus(*this, TEXT("The screen only"), With(EProtokitePlaytestConsentChoice::VideoOnly), EProtokitePlaytestStatus::Ready);
	ExpectPlaytestStatus(*this, TEXT("Play data only"), With(EProtokitePlaytestConsentChoice::PlayDataOnly), EProtokitePlaytestStatus::Ready);
	ExpectPlaytestStatus(*this, TEXT("Everything"), With(EProtokitePlaytestConsentChoice::VideoAndPlayData), EProtokitePlaytestStatus::Ready);

	// Nothing before a loaded config is about the player: there is no playtest to ask about yet.
	ExpectPlaytestStatus(*this, TEXT("No answer, and the config still on its way"),
		With(EProtokitePlaytestConsentChoice::NotAnswered, EProtokitePlaytestConfigState::Fetching),
		EProtokitePlaytestStatus::FetchingPlaytestConfig);
	ExpectPlaytestStatus(*this, TEXT("No answer, and no playtest linked to this build"),
		With(EProtokitePlaytestConsentChoice::NotAnswered, EProtokitePlaytestConfigState::PlaytestNotLinked),
		EProtokitePlaytestStatus::PlaytestNotLinked);
	ExpectPlaytestStatus(*this, TEXT("Nothing allowed, and the key refused"),
		With(EProtokitePlaytestConsentChoice::Nothing, EProtokitePlaytestConfigState::ApiKeyRefused),
		EProtokitePlaytestStatus::ProtokiteRefusedApiKey);

	for (const EProtokitePlaytestStatus Status : { EProtokitePlaytestStatus::WaitingForPlayerConsent, EProtokitePlaytestStatus::PlayerRefusedPlaytest })
	{
		TestFalse(TEXT("It has a description"), DescribePlaytestStatus(Status).IsEmpty());
	}
	return true;
}

#endif
