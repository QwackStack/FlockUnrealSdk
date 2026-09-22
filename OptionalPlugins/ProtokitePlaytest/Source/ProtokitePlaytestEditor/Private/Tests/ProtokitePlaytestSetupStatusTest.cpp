// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Config/FlockConfig.h"
#include "ProtokitePlaytestSettings.h"
#include "ProtokitePlaytestSetupStatus.h"

namespace
{
	/** Everything a playtest needs, set up right: every test changes one thing from here. */
	FProtokitePlaytestSetupInput PlaytestReadyInput()
	{
		FProtokitePlaytestSetupInput Input;
		Input.bPlaytestingEnabled = true;
		Input.ProtokiteApiUrl = TEXT("https://protokite.example.com");
		Input.FlockGameVersion = TEXT("pt-01KX0PLAYTEST00000000000000");
		Input.bFlockAnalyticsEnabled = true;
		Input.bFlockAnalyticsAutoStartSession = true;
		Input.bFlockAnalyticsRequireExplicitConsent = false;
		// A build that asks its players another way, so these tests are about what is set up wrongly. The question this
		// plugin asks has a test of its own below.
		Input.bAskThePlayerForPlaytestConsent = false;
		return Input;
	}

	const FProtokitePlaytestSetupFinding* FindSetupFinding(const TArray<FProtokitePlaytestSetupFinding>& Findings, const TCHAR* Id)
	{
		return Findings.FindByPredicate([Id](const FProtokitePlaytestSetupFinding& Finding) { return Finding.Id == FName(Id); });
	}
}

/** A project set up for a playtest hears nothing, and a project with playtesting off hears nothing however wrong it is. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSetupQuietTest, "Protokite.Playtest.Editor.Setup.QuietWhenSetUpOrTurnedOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSetupQuietTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Set up: nothing to say"), FProtokitePlaytestSetupStatus::Evaluate(PlaytestReadyInput()).Num(), 0);

	FProtokitePlaytestSetupInput Off;
	Off.bPlaytestingEnabled = false;
	Off.ProtokiteApiUrl = TEXT(" not a url ");
	Off.FlockGameVersion = TEXT("1.0.0");
	Off.bFlockAnalyticsEnabled = false;
	Off.bFlockAnalyticsAutoStartSession = false;
	Off.bFlockAnalyticsRequireExplicitConsent = true;
	TestEqual(TEXT("Turned off: nothing to say, however wrong the rest is"), FProtokitePlaytestSetupStatus::Evaluate(Off).Num(), 0);
	return true;
}

/**
 * The Game Version must be a playtest's, named pt- and the test's id, letter for letter as Protokite names it. A release
 * version finds no playtest, and it is the trap a studio walks into by pasting the ID from Protokite's test page.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSetupVersionTest, "Protokite.Playtest.Editor.Setup.NamesAVersionThatIsNotAPlaytests",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSetupVersionTest::RunTest(const FString& Parameters)
{
	FProtokitePlaytestSetupInput Release = PlaytestReadyInput();
	Release.FlockGameVersion = TEXT("1.0.0");
	const TArray<FProtokitePlaytestSetupFinding> Findings = FProtokitePlaytestSetupStatus::Evaluate(Release);
	const FProtokitePlaytestSetupFinding* Finding = FindSetupFinding(Findings, TEXT("Playtest.NotAPlaytestVersion"));
	if (TestNotNull(TEXT("A release version is named"), Finding))
	{
		TestEqual(TEXT("As a warning"), static_cast<int32>(Finding->Severity), static_cast<int32>(EProtokitePlaytestSetupSeverity::Warning));
		TestTrue(TEXT("Saying which version"), Finding->Detail.ToString().Contains(TEXT("'1.0.0'"), ESearchCase::CaseSensitive));
		TestTrue(TEXT("And what a playtest's is called"), Finding->Detail.ToString().Contains(TEXT("pt-<test id>"), ESearchCase::CaseSensitive));
		TestEqual(TEXT("Fixed on the Flock SDK's page"), static_cast<int32>(Finding->Fix), static_cast<int32>(EProtokitePlaytestSetupFix::OpenFlockSettings));
	}
	TestEqual(TEXT("And nothing else"), Findings.Num(), 1);

	FProtokitePlaytestSetupInput Capitals = PlaytestReadyInput();
	Capitals.FlockGameVersion = TEXT("PT-01KX0PLAYTEST00000000000000");
	TestNotNull(TEXT("Protokite names it in small letters, so capitals are not a playtest's"),
		FindSetupFinding(FProtokitePlaytestSetupStatus::Evaluate(Capitals), TEXT("Playtest.NotAPlaytestVersion")));

	FProtokitePlaytestSetupInput Empty = PlaytestReadyInput();
	Empty.FlockGameVersion.Empty();
	TestNull(TEXT("No version at all is the Flock SDK's own finding, not repeated here"),
		FindSetupFinding(FProtokitePlaytestSetupStatus::Evaluate(Empty), TEXT("Playtest.NotAPlaytestVersion")));
	return true;
}

/** A missing or unusable URL stops playtesting, in the running game's own words. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSetupUrlTest, "Protokite.Playtest.Editor.Setup.NamesAProtokiteUrlThatCannotBeUsed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSetupUrlTest::RunTest(const FString& Parameters)
{
	FProtokitePlaytestSetupInput Missing = PlaytestReadyInput();
	Missing.ProtokiteApiUrl.Empty();
	const TArray<FProtokitePlaytestSetupFinding> MissingFindings = FProtokitePlaytestSetupStatus::Evaluate(Missing);
	const FProtokitePlaytestSetupFinding* MissingFinding = FindSetupFinding(MissingFindings, TEXT("Playtest.ProtokiteApiUrlMissing"));
	if (TestNotNull(TEXT("An empty URL is named"), MissingFinding))
	{
		TestEqual(TEXT("As stopping playtesting"), static_cast<int32>(MissingFinding->Severity),
			static_cast<int32>(EProtokitePlaytestSetupSeverity::StopsPlaytesting));
		TestEqual(TEXT("Fixed on the playtest page"), static_cast<int32>(MissingFinding->Fix),
			static_cast<int32>(EProtokitePlaytestSetupFix::OpenPlaytestSettings));
	}

	FProtokitePlaytestSetupInput Spaced = PlaytestReadyInput();
	Spaced.ProtokiteApiUrl = TEXT("https://protokite.example.com ");
	const TArray<FProtokitePlaytestSetupFinding> SpacedFindings = FProtokitePlaytestSetupStatus::Evaluate(Spaced);
	const FProtokitePlaytestSetupFinding* SpacedFinding = FindSetupFinding(SpacedFindings, TEXT("Playtest.ProtokiteApiUrlUnusable"));
	if (TestNotNull(TEXT("A URL with a trailing space is named, never trimmed"), SpacedFinding))
	{
		TestTrue(TEXT("Quoting it, so the space shows"), SpacedFinding->Detail.ToString().Contains(TEXT("'https://protokite.example.com '"), ESearchCase::CaseSensitive));
	}
	return true;
}

/**
 * The Flock SDK's analytics off stops a playtest outright, since the playtest session starts from a Flock one. The
 * start-session and consent settings only make it wait, which is worth knowing and nothing more -- and said only while a
 * Flock session can start at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSetupFlockSettingsTest, "Protokite.Playtest.Editor.Setup.NamesFlockSettingsAPlaytestWaitsOn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSetupFlockSettingsTest::RunTest(const FString& Parameters)
{
	FProtokitePlaytestSetupInput NoAnalytics = PlaytestReadyInput();
	NoAnalytics.bFlockAnalyticsEnabled = false;
	NoAnalytics.bFlockAnalyticsAutoStartSession = false;
	NoAnalytics.bFlockAnalyticsRequireExplicitConsent = true;
	const TArray<FProtokitePlaytestSetupFinding> NoAnalyticsFindings = FProtokitePlaytestSetupStatus::Evaluate(NoAnalytics);
	const FProtokitePlaytestSetupFinding* Off = FindSetupFinding(NoAnalyticsFindings, TEXT("Playtest.FlockAnalyticsOff"));
	if (TestNotNull(TEXT("Analytics off is named"), Off))
	{
		TestEqual(TEXT("As stopping playtesting"), static_cast<int32>(Off->Severity), static_cast<int32>(EProtokitePlaytestSetupSeverity::StopsPlaytesting));
		TestTrue(TEXT("Naming the setting"), Off->Detail.ToString().Contains(TEXT("Analytics Enabled"), ESearchCase::CaseSensitive));
	}
	TestEqual(TEXT("And the settings that only make it wait are not added on top"), NoAnalyticsFindings.Num(), 1);

	FProtokitePlaytestSetupInput Waits = PlaytestReadyInput();
	Waits.bFlockAnalyticsAutoStartSession = false;
	Waits.bFlockAnalyticsRequireExplicitConsent = true;
	Waits.FlockGameVersion = TEXT("1.0.0");
	const TArray<FProtokitePlaytestSetupFinding> WaitFindings = FProtokitePlaytestSetupStatus::Evaluate(Waits);
	const FProtokitePlaytestSetupFinding* StartSession = FindSetupFinding(WaitFindings, TEXT("Playtest.WaitsForStartSession"));
	const FProtokitePlaytestSetupFinding* Consent = FindSetupFinding(WaitFindings, TEXT("Playtest.WaitsForFlockAnalyticsConsent"));
	TestTrue(TEXT("Both waits are named"), StartSession != nullptr && Consent != nullptr);
	if (StartSession != nullptr && Consent != nullptr)
	{
		TestEqual(TEXT("As information"), static_cast<int32>(StartSession->Severity), static_cast<int32>(EProtokitePlaytestSetupSeverity::Info));
		TestEqual(TEXT("Both of them"), static_cast<int32>(Consent->Severity), static_cast<int32>(EProtokitePlaytestSetupSeverity::Info));
	}
	if (TestEqual(TEXT("Three findings"), WaitFindings.Num(), 3))
	{
		TestEqual(TEXT("The most serious first"), WaitFindings[0].Id, FName(TEXT("Playtest.NotAPlaytestVersion")));
	}
	return true;
}

/**
 * The playtest's own consent question is named, and named apart from the Flock SDK's analytics consent. A build can be
 * waiting on both at once, and "consent is missing" without saying whose sends a developer to the wrong settings page.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSetupAsksThePlayerTest,
	"Protokite.Playtest.Editor.Setup.NamesThePlaytestsOwnConsentQuestion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSetupAsksThePlayerTest::RunTest(const FString& Parameters)
{
	FProtokitePlaytestSetupInput Asks = PlaytestReadyInput();
	Asks.bAskThePlayerForPlaytestConsent = true;
	const TArray<FProtokitePlaytestSetupFinding> Findings = FProtokitePlaytestSetupStatus::Evaluate(Asks);
	const FProtokitePlaytestSetupFinding* Question = FindSetupFinding(Findings, TEXT("Playtest.AsksThePlayerWhatToCollect"));
	if (TestNotNull(TEXT("The question is named"), Question))
	{
		TestEqual(TEXT("As information, since nothing is set up wrongly"), static_cast<int32>(Question->Severity),
			static_cast<int32>(EProtokitePlaytestSetupSeverity::Info));
		TestTrue(TEXT("Naming the setting that stops it being asked"),
			Question->Detail.ToString().Contains(TEXT("Ask The Player For Playtest Consent"), ESearchCase::CaseSensitive));
		TestEqual(TEXT("Fixed on the playtest's own settings page"), static_cast<int32>(Question->Fix),
			static_cast<int32>(EProtokitePlaytestSetupFix::OpenPlaytestSettings));
	}
	TestEqual(TEXT("And it is the only thing said about a project that is otherwise set up"), Findings.Num(), 1);

	// Both questions at once, each naming whose it is.
	FProtokitePlaytestSetupInput Both = Asks;
	Both.bFlockAnalyticsRequireExplicitConsent = true;
	const TArray<FProtokitePlaytestSetupFinding> BothFindings = FProtokitePlaytestSetupStatus::Evaluate(Both);
	const FProtokitePlaytestSetupFinding* FlockConsent = FindSetupFinding(BothFindings, TEXT("Playtest.WaitsForFlockAnalyticsConsent"));
	const FProtokitePlaytestSetupFinding* PlaytestConsent = FindSetupFinding(BothFindings, TEXT("Playtest.AsksThePlayerWhatToCollect"));
	if (TestTrue(TEXT("Both are named"), FlockConsent != nullptr && PlaytestConsent != nullptr))
	{
		TestTrue(TEXT("The Flock SDK's says it is the game's own"),
			FlockConsent->Title.ToString().Contains(TEXT("Flock SDK's analytics consent"), ESearchCase::CaseSensitive));
		TestTrue(TEXT("And the playtest's says it is the playtest's"),
			PlaytestConsent->Title.ToString().Contains(TEXT("playtest asks the player"), ESearchCase::CaseSensitive));
	}
	return true;
}

/** The findings come from the project's own settings pages, which is what the editor reads as Play starts. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSetupReadsProjectSettingsTest, "Protokite.Playtest.Editor.Setup.ReadsTheProjectsSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSetupReadsProjectSettingsTest::RunTest(const FString& Parameters)
{
	UProtokitePlaytestSettings* Playtest = GetMutableDefault<UProtokitePlaytestSettings>();
	UFlockConfig* Flock = GetMutableDefault<UFlockConfig>();
	const bool bSavedPlaytesting = Playtest->bPlaytestingEnabled;
	const FString SavedUrl = Playtest->ProtokiteApiUrl;
	const FString SavedVersion = Flock->GameVersion;
	const bool bSavedAnalytics = Flock->bAnalyticsEnabled;
	const bool bSavedAutoStart = Flock->bAnalyticsAutoStartSession;
	const bool bSavedConsent = Flock->bAnalyticsRequireExplicitConsent;
	ON_SCOPE_EXIT
	{
		Playtest->bPlaytestingEnabled = bSavedPlaytesting;
		Playtest->ProtokiteApiUrl = SavedUrl;
		Flock->GameVersion = SavedVersion;
		Flock->bAnalyticsEnabled = bSavedAnalytics;
		Flock->bAnalyticsAutoStartSession = bSavedAutoStart;
		Flock->bAnalyticsRequireExplicitConsent = bSavedConsent;
	};

	Playtest->bPlaytestingEnabled = true;
	Playtest->ProtokiteApiUrl = TEXT("http://localhost:8020");
	Flock->GameVersion = TEXT("2.4.0");

	// Read twice, so every pair of the three Flock switches differs in one reading or the other: a field read from
	// its neighbour cannot pass both.
	Flock->bAnalyticsEnabled = false;
	Flock->bAnalyticsAutoStartSession = true;
	Flock->bAnalyticsRequireExplicitConsent = false;
	const FProtokitePlaytestSetupInput First = FProtokitePlaytestSetupInput::FromProjectSettings();
	TestTrue(TEXT("Enable Playtesting"), First.bPlaytestingEnabled);
	TestEqual(TEXT("Protokite API URL"), First.ProtokiteApiUrl, FString(TEXT("http://localhost:8020")));
	TestEqual(TEXT("The Flock SDK's Game Version"), First.FlockGameVersion, FString(TEXT("2.4.0")));
	TestFalse(TEXT("First reading: Analytics Enabled"), First.bFlockAnalyticsEnabled);
	TestTrue(TEXT("First reading: Analytics Auto Start Session"), First.bFlockAnalyticsAutoStartSession);
	TestFalse(TEXT("First reading: Analytics Require Explicit Consent"), First.bFlockAnalyticsRequireExplicitConsent);

	Flock->bAnalyticsAutoStartSession = false;
	Flock->bAnalyticsRequireExplicitConsent = true;
	const FProtokitePlaytestSetupInput Second = FProtokitePlaytestSetupInput::FromProjectSettings();
	TestFalse(TEXT("Second reading: Analytics Enabled"), Second.bFlockAnalyticsEnabled);
	TestFalse(TEXT("Second reading: Analytics Auto Start Session"), Second.bFlockAnalyticsAutoStartSession);
	TestTrue(TEXT("Second reading: Analytics Require Explicit Consent"), Second.bFlockAnalyticsRequireExplicitConsent);

	Playtest->bPlaytestingEnabled = false;
	TestFalse(TEXT("Enable Playtesting, turned off"), FProtokitePlaytestSetupInput::FromProjectSettings().bPlaytestingEnabled);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
