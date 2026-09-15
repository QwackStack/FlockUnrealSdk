// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockPlaytestIdentity.h"
#include "FlockPlaytestSession.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Tests/FlockPlaytestSubsystemTestSupport.h"
#include "Tests/FlockPlaytestTestSupport.h"

using namespace FlockPlaytestSubsystemTesting;
using namespace FlockPlaytestFixtures;

namespace
{
	void ExpectSessionState(FAutomationTestBase& Test, const FString& What, EFlockPlaytestSessionState Actual,
		EFlockPlaytestSessionState Expected)
	{
		Test.TestEqual(What, static_cast<int32>(Actual), static_cast<int32>(Expected));
	}

	FString ReadFile(const FString& Path)
	{
		FString Contents;
		FFileHelper::LoadFileToString(Contents, *Path);
		return Contents;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionStartsFromTheFirstFlockSessionToReachTheServerTest,
	"Flock.Playtest.Session.StartsFromTheFirstFlockSessionToReachTheServer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionStartsFromTheFirstFlockSessionToReachTheServerTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	Fixture.StartFlock();
	ExpectPlaytestStatus(*this, TEXT("Ready"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);

	// A Flock session starting carries only its local id, which Protokite cannot take.
	Fixture.Flock->GetEvents()->InvokeSessionStarted(TEXT("0f8fad5b-d9cb-469f-a165-70867728950e"));
	TestEqual(TEXT("A Flock session starting sends nothing yet"), Fixture.SessionStarts(), 0);

	Fixture.RegisterFlockSession(FirstFlockSessionId);
	TestEqual(TEXT("The Flock session reaching the server starts the Protokite session"), Fixture.SessionStarts(), 1);
	const TSharedPtr<FJsonObject> Body = Fixture.LastSessionStartBody();
	const FString SavedDeviceId = ReadFile(Fixture.DeviceIdFilePath);
	TestEqual(TEXT("It names that Flock session by its server id"), StringMember(Body, TEXT("flock_session_id")),
		FString(FirstFlockSessionId));
	TestFalse(TEXT("A device id was saved"), SavedDeviceId.IsEmpty());
	TestEqual(TEXT("It sends this install's saved device id"), StringMember(Body, TEXT("device_id")), SavedDeviceId);
	TestEqual(TEXT("And no Steam id, since no Steam subsystem is running"), StringMember(Body, TEXT("steam_id")),
		FString(TEXT("<absent>")));

	if (const FFlockHttpRequest* Request = Fixture.Transport->FindLastRequestEndingWith(PlaytestSessionStartRoute))
	{
		TestEqual(TEXT("A POST"), Request->Method, FString(TEXT("POST")));
		TestEqual(TEXT("To the session route"), Request->Url, FString(TEXT("http://localhost:8020/game/sdk/playtest-session")));
		TestEqual(TEXT("With the API key the Flock SDK initialized with"), Request->Headers.FindRef(TEXT("X-Flock-API-Key")),
			FString(TEXT("secret")));
		TestEqual(TEXT("And its version id"), Request->Headers.FindRef(TEXT("X-Game-Version-ID")), FString(GameVersionId));
	}

	ExpectSessionState(*this, TEXT("Started"), Fixture.Playtest->GetPlaytestSessionState(), EFlockPlaytestSessionState::Started);
	TestEqual(TEXT("The session id Protokite gave is kept"), Fixture.Playtest->GetPlaytestSessionId(), FString(PlaytestSessionId));
	TestEqual(TEXT("And so is the identity sent"), Fixture.Playtest->GetPlaytestIdentity().DeviceId, SavedDeviceId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionWaitsForThePlaytestConfigTest,
	"Flock.Playtest.Session.WaitsForThePlaytestConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionWaitsForThePlaytestConfigTest::RunTest(const FString& Parameters)
{
	{
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.Transport->bHoldReplies = true;
		Fixture.StartFlock();
		Fixture.RegisterFlockSession(FirstFlockSessionId);
		Fixture.RegisterFlockSession(SecondFlockSessionId);
		TestEqual(TEXT("Nothing starts while the config is on its way"), Fixture.SessionStarts(), 0);

		Fixture.Transport->ReleaseAllHeldReplies();
		TestEqual(TEXT("The config arriving starts the session"), Fixture.SessionStarts(), 1);
		TestEqual(TEXT("Naming the first Flock session that reached the server"),
			StringMember(Fixture.LastSessionStartBody(), TEXT("flock_session_id")), FString(FirstFlockSessionId));
		ExpectSessionState(*this, TEXT("Started"), Fixture.Playtest->GetPlaytestSessionState(), EFlockPlaytestSessionState::Started);
	}
	{
		// Protokite could not be reached at first. The next Flock session starting fetches the config again, and the
		// session then names the first Flock session of the initialization, not the one that triggered the fetch.
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.Transport->AnswerInOrder(PlaytestConfigRoute, {
			FFlockPlaytestFakeTransport::ConnectionFailure(), FFlockPlaytestFakeTransport::Status(200, ConfigBody()) });
		Fixture.StartFlock();
		Fixture.RegisterFlockSession(FirstFlockSessionId);
		TestEqual(TEXT("Nothing starts while the config is unavailable"), Fixture.SessionStarts(), 0);

		Fixture.Flock->GetEvents()->InvokeSessionStarted(TEXT("local-2"));
		TestEqual(TEXT("The config loading on the second attempt starts the session"), Fixture.SessionStarts(), 1);
		TestEqual(TEXT("Naming the first Flock session"), StringMember(Fixture.LastSessionStartBody(), TEXT("flock_session_id")),
			FString(FirstFlockSessionId));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionRotationAndSignOutNeitherEndNorRestartTest,
	"Flock.Playtest.Session.RotationAndSignOutNeitherEndNorRestart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionRotationAndSignOutNeitherEndNorRestartTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);
	UFlockEvents* Events = Fixture.Flock->GetEvents();

	// Away past the timeout: the Flock session ends and a new one starts.
	FFlockSessionEndedArgs Ended;
	Ended.Reason = EFlockSessionEndReason::Timeout;
	Events->InvokeSessionEnded(Ended);
	Events->InvokeSessionStarted(TEXT("local-2"));
	Fixture.RegisterFlockSession(SecondFlockSessionId);

	// The player signs out, and signs in again.
	Ended.Reason = EFlockSessionEndReason::Logout;
	Events->InvokeSessionEnded(Ended);
	Events->InvokeLoggedOut();
	Events->InvokeSessionStarted(TEXT("local-3"));
	Fixture.RegisterFlockSession(ThirdFlockSessionId);

	TestEqual(TEXT("One start in the launch"), Fixture.SessionStarts(), 1);
	TestEqual(TEXT("No end"), Fixture.SessionEnds(), 0);
	ExpectSessionState(*this, TEXT("Still started"), Fixture.Playtest->GetPlaytestSessionState(), EFlockPlaytestSessionState::Started);
	TestEqual(TEXT("The same session"), Fixture.Playtest->GetPlaytestSessionId(), FString(PlaytestSessionId));
	TestEqual(TEXT("Still naming the first Flock session"), StringMember(Fixture.LastSessionStartBody(), TEXT("flock_session_id")),
		FString(FirstFlockSessionId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionFlockShuttingDownNeitherEndsNorRestartsItTest,
	"Flock.Playtest.Session.FlockShuttingDownNeitherEndsNorRestartsIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionFlockShuttingDownNeitherEndsNorRestartsItTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);

	Fixture.Flock->ShutdownSdk();
	ExpectSessionState(*this, TEXT("Still started once the Flock SDK shuts down"), Fixture.Playtest->GetPlaytestSessionState(),
		EFlockPlaytestSessionState::Started);
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	Fixture.RegisterFlockSession(SecondFlockSessionId);

	TestEqual(TEXT("One start in the launch"), Fixture.SessionStarts(), 1);
	TestEqual(TEXT("No end"), Fixture.SessionEnds(), 0);
	TestEqual(TEXT("The same session"), Fixture.Playtest->GetPlaytestSessionId(), FString(PlaytestSessionId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionFlockSessionOfAnEndedInitializationIsNotUsedTest,
	"Flock.Playtest.Session.FlockSessionOfAnEndedInitializationIsNotUsed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionFlockSessionOfAnEndedInitializationIsNotUsedTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	Fixture.Transport->AnswerInOrder(PlaytestConfigRoute, {
		FFlockPlaytestFakeTransport::Status(503, FlockUnreachableBody), FFlockPlaytestFakeTransport::Status(200, ConfigBody()) });
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);

	Fixture.Flock->ShutdownSdk();
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	ExpectPlaytestStatus(*this, TEXT("Ready in the second initialization"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);
	TestEqual(TEXT("A Flock session of the initialization that ended starts nothing"), Fixture.SessionStarts(), 0);

	Fixture.RegisterFlockSession(SecondFlockSessionId);
	TestEqual(TEXT("The new initialization's first session starts it"), Fixture.SessionStarts(), 1);
	TestEqual(TEXT("And is the one named"), StringMember(Fixture.LastSessionStartBody(), TEXT("flock_session_id")),
		FString(SecondFlockSessionId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionStartIsNeverTriedTwiceTest,
	"Flock.Playtest.Session.StartIsNeverTriedTwice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionStartIsNeverTriedTwiceTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		FFlockHttpResponse Response;
		bool bLoseTheAnswer;
		EFlockPlaytestSessionState Expected;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{ FFlockPlaytestFakeTransport::ConnectionFailure(), false, EFlockPlaytestSessionState::StartFailed, TEXT("No connection") },
		{ FFlockPlaytestFakeTransport::TimedOut(), false, EFlockPlaytestSessionState::StartFailed, TEXT("A timeout") },
		{ FFlockPlaytestFakeTransport::Status(503, FlockUnreachableBody), false, EFlockPlaytestSessionState::StartFailed, TEXT("HTTP 503") },
		{ FFlockPlaytestFakeTransport::Status(429, TEXT("{}")), false, EFlockPlaytestSessionState::StartFailed, TEXT("HTTP 429") },
		{ FFlockPlaytestFakeTransport::Status(401, InvalidApiKeyBody), false, EFlockPlaytestSessionState::StartFailed, TEXT("HTTP 401") },
		{ FFlockPlaytestFakeTransport::Status(404, NotLinkedBody), false, EFlockPlaytestSessionState::StartFailed, TEXT("HTTP 404") },
		{ FFlockPlaytestFakeTransport::Status(422, NoPlayerIdentityBody), false, EFlockPlaytestSessionState::StartFailed, TEXT("HTTP 422") },
		{ FFlockPlaytestFakeTransport::Status(200, Envelope(TEXT("{}"))), false, EFlockPlaytestSessionState::StartFailed, TEXT("A 200 without a session id") },
		{ FFlockPlaytestFakeTransport::Status(200, SessionStartBody()), true, EFlockPlaytestSessionState::Starting, TEXT("An answer that never arrives") },
	};
	for (const FCase& Case : Cases)
	{
		FScopedPlaytestSettings Settings(true, UsableUrl);
		// Three retries allowed, the way a game's Flock settings might, so a start that was retried would show.
		FScopedFlockRetrySettings RetrySettings(3, false);
		FPlaytestFixture Fixture(/*bTurnRetriesOff*/ false);
		Fixture.Transport->Answer(PlaytestSessionStartRoute, Case.Response);
		Fixture.StartFlock();

		Fixture.Transport->bHoldReplies = Case.bLoseTheAnswer;
		Fixture.RegisterFlockSession(FirstFlockSessionId);
		Fixture.Transport->DropAllHeldReplies();
		Fixture.Transport->bHoldReplies = false;
		RunPendingPlaytestRetries();

		// Everything that could start one again: a new Flock session, and the Flock SDK initializing afresh.
		Fixture.RegisterFlockSession(SecondFlockSessionId);
		Fixture.Flock->ShutdownSdk();
		Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
		Fixture.RegisterFlockSession(ThirdFlockSessionId);
		RunPendingPlaytestRetries();

		TestEqual(FString::Printf(TEXT("%s: one start in the launch"), Case.What), Fixture.SessionStarts(), 1);
		ExpectSessionState(*this, Case.What, Fixture.Playtest->GetPlaytestSessionState(), Case.Expected);
		TestTrue(FString::Printf(TEXT("%s: no session id"), Case.What), Fixture.Playtest->GetPlaytestSessionId().IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionClosedPlaytestStopsPlaytestingForTheLaunchTest,
	"Flock.Playtest.Session.ClosedPlaytestStopsPlaytestingForTheLaunch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionClosedPlaytestStopsPlaytestingForTheLaunchTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	Fixture.Transport->Answer(PlaytestSessionStartRoute, FFlockPlaytestFakeTransport::Status(400, NoLongerCollectingBody));
	FPlaytestLogCapture Capture;
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);

	ExpectPlaytestStatus(*this, TEXT("A closed playtest"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::PlaytestNoLongerCollecting);
	ExpectSessionState(*this, TEXT("Start failed"), Fixture.Playtest->GetPlaytestSessionState(), EFlockPlaytestSessionState::StartFailed);
	TestFalse(TEXT("Every feature is off"), Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::VideoRecording));
	const TArray<FPlaytestLogCapture::FLine> Closed =
		Capture.LinesContaining(DescribePlaytestStatus(EFlockPlaytestStatus::PlaytestNoLongerCollecting));
	if (TestEqual(TEXT("It is reported once"), Closed.Num(), 1))
	{
		TestEqual(TEXT("As a warning"), static_cast<int32>(Closed[0].Verbosity), static_cast<int32>(ELogVerbosity::Warning));
	}

	const int32 ConfigRequestsBefore = Fixture.ConfigRequests();
	Fixture.Flock->ShutdownSdk();
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	Fixture.Flock->GetEvents()->InvokeSessionStarted(TEXT("local-2"));
	Fixture.RegisterFlockSession(SecondFlockSessionId);

	ExpectPlaytestStatus(*this, TEXT("Still closed after the Flock SDK initializes again"), Fixture.Playtest->GetStatus(),
		EFlockPlaytestStatus::PlaytestNoLongerCollecting);
	TestEqual(TEXT("The playtest is not fetched again"), Fixture.ConfigRequests(), ConfigRequestsBefore);
	TestEqual(TEXT("No second start"), Fixture.SessionStarts(), 1);
	TestFalse(TEXT("Every feature is still off"), Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::VideoRecording));
	TestEqual(TEXT("Still reported once"),
		Capture.LinesContaining(DescribePlaytestStatus(EFlockPlaytestStatus::PlaytestNoLongerCollecting)).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionQuitEndsTheSessionTest,
	"Flock.Playtest.Session.QuitEndsTheSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionQuitEndsTheSessionTest::RunTest(const FString& Parameters)
{
	{
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.StartFlock();
		Fixture.RegisterFlockSession(FirstFlockSessionId);

		Fixture.Playtest->Deinitialize();
		TestEqual(TEXT("The game instance shutting down sends one end"), Fixture.SessionEnds(), 1);
		if (const FFlockHttpRequest* End = Fixture.Transport->FindLastRequestEndingWith(TEXT("/end")))
		{
			TestEqual(TEXT("A POST"), End->Method, FString(TEXT("POST")));
			TestEqual(TEXT("To the started session's end address"), End->Url,
				FString::Printf(TEXT("http://localhost:8020/game/sdk/playtest-session/%s/end"), PlaytestSessionId));
			TestEqual(TEXT("With the API key the start used"), End->Headers.FindRef(TEXT("X-Flock-API-Key")), FString(TEXT("secret")));
		}
		ExpectSessionState(*this, TEXT("Ended"), Fixture.Playtest->GetPlaytestSessionState(), EFlockPlaytestSessionState::Ended);
	}
	{
		// A session that never started has nothing to end.
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.StartFlock();
		Fixture.Playtest->Deinitialize();
		TestEqual(TEXT("No session, no end"), Fixture.SessionEnds(), 0);
	}
	{
		// The game ended it before quitting, so quitting sends nothing more.
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.StartFlock();
		Fixture.RegisterFlockSession(FirstFlockSessionId);
		TestTrue(TEXT("The game ends the session"), Fixture.Playtest->EndPlaytestSession());
		Fixture.Playtest->Deinitialize();
		TestEqual(TEXT("One end in all"), Fixture.SessionEnds(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionEndUsesTheStartsAddressAndHeadersTest,
	"Flock.Playtest.Session.EndUsesTheStartsAddressAndHeaders",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionEndUsesTheStartsAddressAndHeadersTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);

	// Afterwards the Flock SDK shuts down, so its headers are gone, and the setting points somewhere else.
	Fixture.Flock->ShutdownSdk();
	Settings.Settings->ProtokiteApiUrl = TEXT("http://elsewhere.invalid:8020");

	TestTrue(TEXT("The started session can still be ended"), Fixture.Playtest->EndPlaytestSession());
	if (const FFlockHttpRequest* End = Fixture.Transport->FindLastRequestEndingWith(TEXT("/end")))
	{
		TestTrue(TEXT("At the address the start used"), End->Url.StartsWith(TEXT("http://localhost:8020/"), ESearchCase::CaseSensitive));
		TestEqual(TEXT("With the API key the start used"), End->Headers.FindRef(TEXT("X-Flock-API-Key")), FString(TEXT("secret")));
		TestEqual(TEXT("And its version id"), End->Headers.FindRef(TEXT("X-Game-Version-ID")), FString(GameVersionId));
	}
	else
	{
		AddError(TEXT("No end was sent"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionEndingTwiceSendsTwoEndsTest,
	"Flock.Playtest.Session.EndingTwiceSendsTwoEnds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionEndingTwiceSendsTwoEndsTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	FPlaytestLogCapture Capture;
	Fixture.StartFlock();
	TestFalse(TEXT("Nothing to end before a session starts"), Fixture.Playtest->EndPlaytestSession());
	TestEqual(TEXT("And nothing is sent"), Fixture.SessionEnds(), 0);

	Fixture.RegisterFlockSession(FirstFlockSessionId);
	TestTrue(TEXT("The first end"), Fixture.Playtest->EndPlaytestSession());
	TestTrue(TEXT("The second end"), Fixture.Playtest->EndPlaytestSession());
	TestEqual(TEXT("Two ends are sent"), Fixture.SessionEnds(), 2);
	TestEqual(TEXT("Both are reported as ended"), Capture.LinesContaining(TEXT("ended.")).Num(), 2);
	TestEqual(TEXT("Neither is reported as a failure"), Capture.LinesContaining(TEXT("could not be ended")).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionStartAnsweredAfterTeardownIsEndedTest,
	"Flock.Playtest.Session.StartAnsweredAfterTeardownIsEnded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionStartAnsweredAfterTeardownIsEndedTest::RunTest(const FString& Parameters)
{
	{
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.StartFlock();
		Fixture.Transport->bHoldReplies = true;
		Fixture.RegisterFlockSession(FirstFlockSessionId);

		Fixture.Playtest->Deinitialize();
		TestEqual(TEXT("No end while the start is still on its way"), Fixture.SessionEnds(), 0);

		Fixture.Transport->bHoldReplies = false;
		Fixture.Transport->ReleaseAllHeldReplies();
		TestEqual(TEXT("The session the late answer names is ended straight away"), Fixture.SessionEnds(), 1);
		if (const FFlockHttpRequest* End = Fixture.Transport->FindLastRequestEndingWith(TEXT("/end")))
		{
			TestTrue(TEXT("It is that session"), End->Url.Contains(PlaytestSessionId, ESearchCase::CaseSensitive));
		}
		ExpectSessionState(*this, TEXT("Ended"), Fixture.Playtest->GetPlaytestSessionState(), EFlockPlaytestSessionState::Ended);
	}
	{
		// A late answer that created nothing has nothing to end.
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.Transport->Answer(PlaytestSessionStartRoute, FFlockPlaytestFakeTransport::Status(503, FlockUnreachableBody));
		Fixture.StartFlock();
		Fixture.Transport->bHoldReplies = true;
		Fixture.RegisterFlockSession(FirstFlockSessionId);
		Fixture.Playtest->Deinitialize();
		Fixture.Transport->bHoldReplies = false;
		Fixture.Transport->ReleaseAllHeldReplies();
		TestEqual(TEXT("A failed late answer sends no end"), Fixture.SessionEnds(), 0);
		ExpectSessionState(*this, TEXT("Start failed"), Fixture.Playtest->GetPlaytestSessionState(), EFlockPlaytestSessionState::StartFailed);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionNoPlayerIdentitySendsNothingTest,
	"Flock.Playtest.Session.NoPlayerIdentitySendsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionNoPlayerIdentitySendsNothingTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	// A file stands where the device id file's folder would be, so no device id can be saved.
	const FString Blocker = FPaths::Combine(Fixture.Folder, TEXT("blocker"));
	FFileHelper::SaveStringToFile(TEXT("x"), *Blocker);
	Fixture.Playtest->SetDeviceIdFilePathForTesting(FPaths::Combine(Blocker, TEXT("device_id.txt")));
	FPlaytestLogCapture Capture;
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);

	TestEqual(TEXT("No start without an identity"), Fixture.SessionStarts(), 0);
	ExpectSessionState(*this, TEXT("No identity"), Fixture.Playtest->GetPlaytestSessionState(), EFlockPlaytestSessionState::NoPlayerIdentity);
	TestTrue(TEXT("The identity is empty"), Fixture.Playtest->GetPlaytestIdentity().IsEmpty());
	const TArray<FPlaytestLogCapture::FLine> Reasons = Capture.LinesContaining(TEXT("No Protokite session is started this launch"));
	if (TestEqual(TEXT("The reason is logged once"), Reasons.Num(), 1))
	{
		TestEqual(TEXT("As a warning"), static_cast<int32>(Reasons[0].Verbosity), static_cast<int32>(ELogVerbosity::Warning));
		TestTrue(TEXT("Naming the file it could not save"), Reasons[0].Message.Contains(Blocker));
	}

	Fixture.RegisterFlockSession(SecondFlockSessionId);
	TestEqual(TEXT("Nothing is tried again this launch"), Fixture.SessionStarts(), 0);
	TestEqual(TEXT("And the reason is not logged again"), Capture.LinesContaining(TEXT("No Protokite session is started this launch")).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionSteamIdWhenSteamIsRunningTest,
	"Flock.Playtest.Session.SteamIdWhenSteamIsRunning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionSteamIdWhenSteamIsRunningTest::RunTest(const FString& Parameters)
{
	const FString SteamId = TEXT("76561198000000001");
	{
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.SteamAccount->Id = SteamId;
		Fixture.SteamAccount->Nickname = TEXT("Play Tester");
		Fixture.StartFlock();
		Fixture.RegisterFlockSession(FirstFlockSessionId);

		const TSharedPtr<FJsonObject> Body = Fixture.LastSessionStartBody();
		TestEqual(TEXT("The Steam id is sent"), StringMember(Body, TEXT("steam_id")), SteamId);
		TestEqual(TEXT("With the Steam name"), StringMember(Body, TEXT("player_name")), FString(TEXT("Play Tester")));
		TestEqual(TEXT("And no device id"), StringMember(Body, TEXT("device_id")), FString(TEXT("<absent>")));
		TestFalse(TEXT("The device id file is never touched"), IFileManager::Get().FileExists(*Fixture.DeviceIdFilePath));
		TestEqual(TEXT("The identity kept is the Steam one"), Fixture.Playtest->GetPlaytestIdentity().SteamId, SteamId);
	}
	{
		// A name too long for Protokite is left out rather than cut short, and the start still goes out.
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.SteamAccount->Id = SteamId;
		Fixture.SteamAccount->Nickname = FString::ChrN(201, TEXT('n'));
		Fixture.StartFlock();
		Fixture.RegisterFlockSession(FirstFlockSessionId);

		const TSharedPtr<FJsonObject> Body = Fixture.LastSessionStartBody();
		TestEqual(TEXT("A long name: the Steam id is still sent"), StringMember(Body, TEXT("steam_id")), SteamId);
		TestEqual(TEXT("A long name is left out"), StringMember(Body, TEXT("player_name")), FString(TEXT("<absent>")));
	}
	const FString UnusableSteamIds[] = { SteamId + TEXT(" "), FString::ChrN(65, TEXT('7')) };
	for (const FString& Unusable : UnusableSteamIds)
	{
		// Refused rather than trimmed, and the device id goes instead.
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.SteamAccount->Id = Unusable;
		FPlaytestLogCapture Capture;
		Fixture.StartFlock();
		Fixture.RegisterFlockSession(FirstFlockSessionId);

		const TSharedPtr<FJsonObject> Body = Fixture.LastSessionStartBody();
		const FString What = FString::Printf(TEXT("Steam id '%s'"), *Unusable);
		TestEqual(What + TEXT(" is not sent"), StringMember(Body, TEXT("steam_id")), FString(TEXT("<absent>")));
		TestEqual(What + TEXT(": the device id is sent instead"), StringMember(Body, TEXT("device_id")), ReadFile(Fixture.DeviceIdFilePath));
		TestEqual(What + TEXT(": one warning says why"), Capture.LinesContaining(TEXT("gave a Steam id Protokite cannot take")).Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionNothingIsResolvedOrSentUntilAStartTest,
	"Flock.Playtest.Session.NothingIsResolvedOrSentUntilAStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionNothingIsResolvedOrSentUntilAStartTest::RunTest(const FString& Parameters)
{
	{
		// Turned off: a whole launch, a Flock session included, sends nothing and looks up nothing.
		FScopedPlaytestSettings Settings(false, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.StartFlock();
		Fixture.RegisterFlockSession(FirstFlockSessionId);
		Fixture.Playtest->Deinitialize();
		TestEqual(TEXT("Turned off: no request of any kind"), Fixture.Transport->Requests.Num(), 0);
		TestEqual(TEXT("Turned off: Steam is never asked"), *Fixture.SteamReads, 0);
		TestFalse(TEXT("Turned off: no device id file is written"), IFileManager::Get().FileExists(*Fixture.DeviceIdFilePath));
	}
	{
		// Ready, with no Flock session yet: the identity waits for a start that is about to be sent.
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.StartFlock();
		ExpectPlaytestStatus(*this, TEXT("Ready"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);
		TestEqual(TEXT("Ready without a Flock session: Steam is not asked yet"), *Fixture.SteamReads, 0);
		TestFalse(TEXT("Ready without a Flock session: no device id file yet"), IFileManager::Get().FileExists(*Fixture.DeviceIdFilePath));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionLogsTheWaitAndTheStartOnceTest,
	"Flock.Playtest.Session.LogsTheWaitAndTheStartOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionLogsTheWaitAndTheStartOnceTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	FPlaytestLogCapture Capture;
	Fixture.StartFlock();
	Fixture.Flock->ShutdownSdk();
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());

	const TArray<FPlaytestLogCapture::FLine> Waits = Capture.LinesContaining(TEXT("starts once a Flock session reaches the server"));
	if (TestEqual(TEXT("Waiting for a Flock session is logged once, however often the status reaches Ready"), Waits.Num(), 1))
	{
		TestEqual(TEXT("At Log"), static_cast<int32>(Waits[0].Verbosity), static_cast<int32>(ELogVerbosity::Log));
		// A Flock session depends on these settings as well as on a sign-in, so a wait that never ends says where to look.
		for (const TCHAR* Setting : { TEXT("Analytics Enabled"), TEXT("Analytics Auto Start Session"), TEXT("Analytics Require Explicit Consent") })
		{
			TestTrue(FString::Printf(TEXT("It names %s"), Setting), Waits[0].Message.Contains(Setting));
		}
	}

	Fixture.RegisterFlockSession(FirstFlockSessionId);
	const TArray<FPlaytestLogCapture::FLine> Starts = Capture.LinesContaining(TEXT("started for this launch"));
	if (TestEqual(TEXT("The start is logged once"), Starts.Num(), 1))
	{
		TestEqual(TEXT("At Log"), static_cast<int32>(Starts[0].Verbosity), static_cast<int32>(ELogVerbosity::Log));
		TestTrue(TEXT("Naming the session"), Starts[0].Message.Contains(PlaytestSessionId));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionStartCarriesTheBuildFactsTest,
	"Flock.Playtest.Session.StartCarriesTheBuildFacts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionStartCarriesTheBuildFactsTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);

	const TSharedPtr<FJsonObject> Body = Fixture.LastSessionStartBody();
	TestEqual(TEXT("No player_name without a Steam name"), StringMember(Body, TEXT("player_name")), FString(TEXT("<absent>")));
	const TSharedPtr<FJsonObject>* Debug = nullptr;
	if (TestTrue(TEXT("extra_debug is an object"), Body.IsValid() && Body->TryGetObjectField(TEXT("extra_debug"), Debug) && Debug != nullptr))
	{
		TestEqual(TEXT("engine_version"), StringMember(*Debug, TEXT("engine_version")), FEngineVersion::Current().ToString());
		TestEqual(TEXT("build_configuration"), StringMember(*Debug, TEXT("build_configuration")),
			FString(LexToString(FApp::GetBuildConfiguration())));
		TestEqual(TEXT("sdk_version"), StringMember(*Debug, TEXT("sdk_version")), UFlockSubsystem::SdkVersion);
		const FString Gpu = FPlatformMisc::GetPrimaryGPUBrand();
		TestEqual(TEXT("gpu, when the machine names one"), StringMember(*Debug, TEXT("gpu")), Gpu.IsEmpty() ? FString(TEXT("<absent>")) : Gpu);
		TestEqual(TEXT("No map, since this game instance has no world"), StringMember(*Debug, TEXT("map")), FString(TEXT("<absent>")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionFlockSessionIdThatDoesNotFitIsLeftOutTest,
	"Flock.Playtest.Session.FlockSessionIdThatDoesNotFitIsLeftOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionFlockSessionIdThatDoesNotFitIsLeftOutTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	FPlaytestLogCapture Capture;
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FString(FirstFlockSessionId) + TEXT("7"));

	TestEqual(TEXT("The session still starts"), Fixture.SessionStarts(), 1);
	TestEqual(TEXT("Without a flock_session_id Protokite would refuse"), StringMember(Fixture.LastSessionStartBody(), TEXT("flock_session_id")),
		FString(TEXT("<absent>")));
	TestEqual(TEXT("And one warning says why"), Capture.LinesContaining(TEXT("cannot be sent to Protokite")).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSessionWithoutTestSourcesUsesTheDeviceIdUnderSavedTest,
	"Flock.Playtest.Session.WithoutTestSourcesUsesTheDeviceIdUnderSaved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSessionWithoutTestSourcesUsesTheDeviceIdUnderSavedTest::RunTest(const FString& Parameters)
{
	// The identity sources a game uses: the engine's Steam subsystem, of which this test process runs none, and the
	// device id file under Saved.
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture(/*bTurnRetriesOff*/ true, /*bUseTestIdentitySources*/ false);
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);

	FString DeviceId;
	FFlockPlaytestDeviceIdFile(FFlockPlaytestDeviceIdFile::GetDefaultPath()).ReadOrCreate(DeviceId);
	const TSharedPtr<FJsonObject> Body = Fixture.LastSessionStartBody();
	TestFalse(TEXT("The file under Saved holds a device id"), DeviceId.IsEmpty());
	TestEqual(TEXT("That device id is sent"), StringMember(Body, TEXT("device_id")), DeviceId);
	TestEqual(TEXT("And no Steam id"), StringMember(Body, TEXT("steam_id")), FString(TEXT("<absent>")));
	return true;
}

#endif
