// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Tests/FlockPlaytestSubsystemTestSupport.h"
#include "Tests/FlockPlaytestTestSupport.h"

using namespace FlockPlaytestSubsystemTesting;
using namespace FlockPlaytestFixtures;

namespace
{
	const TCHAR* const ExceptionsNotCapturedWarning = TEXT("but the Flock SDK is not capturing exceptions");

	/** A signed-in player whose Flock session reaches the server, which is what starts the launch's Protokite session. */
	void StartSignedInWithExceptionCapturing(FPlaytestFixture& Fixture, bool bExceptionCapturing)
	{
		Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200,
			ConfigBody(GameVersionId, /*bHeavyAnalytics*/ false, /*bVideoRecording*/ false, bExceptionCapturing)));
		Fixture.SignInToFlockOnStart();
		Fixture.StartFlock();
	}
}

/**
 * Exceptions are the Flock SDK's to report, and a playtest switch never turns that on (decision D4). So a playtest that
 * asks for them from a game whose Flock SDK is not capturing them gets nothing, and the one thing this plugin can do is
 * say so -- once, as a warning naming the setting, when the launch's session starts.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestExceptionsWarnWhenNotCapturedTest,
	"Flock.Playtest.Exceptions.WarnsWhenTheFlockSdkIsNotCapturingThem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestExceptionsWarnWhenNotCapturedTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true, /*bCaptureExceptions*/ false);
	FPlaytestLogCapture Log;
	FPlaytestFixture Fixture;
	StartSignedInWithExceptionCapturing(Fixture, true);

	TestTrue(TEXT("Precondition: the playtest asks for exceptions"),
		Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::ExceptionCapturing));
	TestFalse(TEXT("Precondition: the Flock SDK is not capturing them"), Fixture.Flock->GetExceptionCaptureCoverage().bEnabled);
	TestEqual(TEXT("Precondition: the launch's session started"), Fixture.SessionStarts(), 1);

	// A later Flock session changes nothing: the launch has its one session, and the warning has been given.
	Fixture.RegisterFlockSession(SecondFlockSessionId);

	const TArray<FPlaytestLogCapture::FLine> Lines = Log.LinesContaining(ExceptionsNotCapturedWarning);
	if (TestEqual(TEXT("It is logged once"), Lines.Num(), 1))
	{
		TestEqual(TEXT("As a warning"), static_cast<int32>(Lines[0].Verbosity), static_cast<int32>(ELogVerbosity::Warning));
		TestTrue(TEXT("Naming the setting to turn on"),
			Lines[0].Message.Contains(TEXT("Analytics Capture Exceptions"), ESearchCase::CaseSensitive));
	}
	return true;
}

/** The control for the warning: with the Flock SDK capturing, a playtest asking for exceptions hears nothing about it. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestExceptionsQuietWhenCapturedTest,
	"Flock.Playtest.Exceptions.SaysNothingWhenTheFlockSdkIsCapturingThem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestExceptionsQuietWhenCapturedTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true, /*bCaptureExceptions*/ true);
	FPlaytestLogCapture Log;
	FPlaytestFixture Fixture;
	StartSignedInWithExceptionCapturing(Fixture, true);

	TestTrue(TEXT("Precondition: the Flock SDK is capturing"), Fixture.Flock->GetExceptionCaptureCoverage().bEnabled);
	TestEqual(TEXT("Precondition: the launch's session started"), Fixture.SessionStarts(), 1);
	TestEqual(TEXT("Nothing is said"), Log.LinesContaining(ExceptionsNotCapturedWarning).Num(), 0);
	return true;
}

/** Nor does a playtest that does not ask for exceptions hear about a Flock SDK that is not capturing them. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestExceptionsQuietWhenNotAskedForTest,
	"Flock.Playtest.Exceptions.SaysNothingWhenThePlaytestDoesNotAskForThem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestExceptionsQuietWhenNotAskedForTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true, /*bCaptureExceptions*/ false);
	FPlaytestLogCapture Log;
	FPlaytestFixture Fixture;
	StartSignedInWithExceptionCapturing(Fixture, false);

	TestFalse(TEXT("Precondition: the playtest does not ask for exceptions"),
		Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::ExceptionCapturing));
	TestEqual(TEXT("Precondition: the launch's session started"), Fixture.SessionStarts(), 1);
	TestEqual(TEXT("Nothing is said"), Log.LinesContaining(ExceptionsNotCapturedWarning).Num(), 0);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
