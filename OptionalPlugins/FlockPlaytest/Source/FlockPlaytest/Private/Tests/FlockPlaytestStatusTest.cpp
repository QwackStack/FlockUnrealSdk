// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockPlaytestStatus.h"
#include "Tests/FlockPlaytestTestSupport.h"

namespace
{
	FFlockPlaytestStatusInputs MakeInputs(bool bPlaytestingEnabled, const FString& ProtokiteApiUrl, bool bFlockInitialized)
	{
		FFlockPlaytestStatusInputs Inputs;
		Inputs.bPlaytestingEnabled = bPlaytestingEnabled;
		Inputs.ProtokiteApiUrl = ProtokiteApiUrl;
		Inputs.bFlockInitialized = bFlockInitialized;
		return Inputs;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestStatusTurnedOffOutranksEverythingTest,
	"Flock.Playtest.Status.TurnedOffOutranksEverything",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestStatusTurnedOffOutranksEverythingTest::RunTest(const FString& Parameters)
{
	// With the switch off nothing else is looked at, so none of these can surface a URL or Flock answer.
	ExpectPlaytestStatus(*this, TEXT("Off, empty URL, Flock not initialized"),
		DecidePlaytestStatus(MakeInputs(false, TEXT(""), false)), EFlockPlaytestStatus::TurnedOff);
	ExpectPlaytestStatus(*this, TEXT("Off, unusable URL, Flock initialized"),
		DecidePlaytestStatus(MakeInputs(false, TEXT("localhost:8020"), true)), EFlockPlaytestStatus::TurnedOff);
	ExpectPlaytestStatus(*this, TEXT("Off, usable URL, Flock initialized"),
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
		ExpectPlaytestStatus(*this, FString::Printf(TEXT("'%s' with Flock initialized"), Url),
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

	ExpectPlaytestStatus(*this, TEXT("Complete settings, Flock not initialized"),
		DecidePlaytestStatus(MakeInputs(true, TEXT("http://localhost:8020"), false)), EFlockPlaytestStatus::WaitingForFlock);
	ExpectPlaytestStatus(*this, TEXT("Complete settings, Flock initialized"),
		DecidePlaytestStatus(MakeInputs(true, TEXT("http://localhost:8020"), true)), EFlockPlaytestStatus::Ready);
	return true;
}

#endif
