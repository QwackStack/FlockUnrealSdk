// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"
#include "Misc/FlockEngineCompat.h"

#if WITH_AUTOMATION_TESTS

#include "ProtokitePlaytestLibrary.h"
#include "ProtokitePlaytestSubsystem.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "ProtokitePlaytestRecordingsFolder.h"
#include "ProtokitePlaytestSelfTest.h"
#include "Tests/ProtokitePlaytestFakeFileUploader.h"
#include "Tests/ProtokitePlaytestRecordingUploadTestSupport.h"
#include "Tests/ProtokitePlaytestSubsystemTestSupport.h"
#include "Tests/ProtokitePlaytestTestSupport.h"
#if WITH_PROTOKITE_PLAYTEST_VIDEO
#include "Tests/ProtokitePlaytestVideoTestSupport.h"
#endif

using namespace ProtokitePlaytestSubsystemTesting;
using namespace ProtokitePlaytestFixtures;
using namespace ProtokitePlaytestRecordingUploadTesting;

namespace
{
	/** Sets Start Session And Recording Automatically for one test and puts the previous value back when it ends. */
	struct FScopedStartAutomatically
	{
		UProtokitePlaytestSettings* Settings = GetMutableDefault<UProtokitePlaytestSettings>();
		bool bSaved = Settings->bStartSessionAndRecordingAutomatically;

		explicit FScopedStartAutomatically(bool bStartAutomatically) { Settings->bStartSessionAndRecordingAutomatically = bStartAutomatically; }
		~FScopedStartAutomatically() { Settings->bStartSessionAndRecordingAutomatically = bSaved; }
	};

	/** The id Protokite gives a launch's second session. */
	const FString SecondPlaytestSessionId = TEXT("01KX0PLAYTESTSESSION000002");

	FString UploadLinkBodyFor(const FString& SessionId)
	{
		return EnvelopedLinkBody(FString::Printf(TEXT("http://storage.local/put/%s.webm?signature=abc"), *SessionId));
	}

	/** Hears the session event for a test: each (session id, started) in order. */
	struct FSessionChangesHeard
	{
		TStrongObjectPtr<UProtokitePlaytestSelfTestListener> Listener{ NewObject<UProtokitePlaytestSelfTestListener>() };
		TSharedRef<TArray<TPair<FString, bool>>> Heard = MakeShared<TArray<TPair<FString, bool>>>();

		explicit FSessionChangesHeard(UProtokitePlaytestSubsystem* Playtest)
		{
			const TSharedRef<TArray<TPair<FString, bool>>> Into = Heard;
			Listener->OnSessionChanged = [Into](const FString& SessionId, bool bStarted) { Into->Add({ SessionId, bStarted }); };
			Playtest->OnPlaytestSessionChanged.AddDynamic(Listener.Get(), &UProtokitePlaytestSelfTestListener::HandlePlaytestSessionChanged);
		}
	};

	int32 CountRecordingFiles(const FPlaytestFixture& Fixture)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *FPaths::Combine(Fixture.Folder, TEXT("Recordings")), TEXT("*.webm"), true, false);
		return Files.Num();
	}

#if WITH_PROTOKITE_PLAYTEST_VIDEO
	/**
	 * A playtest that records video, with storage and an upload link standing by for both sessions a test may start.
	 * Returns the uploader every upload goes through.
	 */
	TSharedRef<FProtokitePlaytestFakeFileUploader> ReadyToRecord(FPlaytestFixture& Fixture)
	{
		const TSharedRef<FProtokitePlaytestFakeFileUploader> Uploader = MakeShared<FProtokitePlaytestFakeFileUploader>();
		Uploader->On(TEXT("/put/"));
		Fixture.Playtest->SetFileUploaderForTesting(Uploader);
		Fixture.Transport->Answer(UploadLinkRouteFor(PlaytestSessionId), FProtokitePlaytestFakeTransport::Status(200, UploadLinkBodyFor(PlaytestSessionId)));
		Fixture.Transport->Answer(UploadLinkRouteFor(SecondPlaytestSessionId),
			FProtokitePlaytestFakeTransport::Status(200, UploadLinkBodyFor(SecondPlaytestSessionId)));
		Fixture.Playtest->SetVideoFrameSourceFactoryForTesting([](FIntPoint MaxVideoSize, FString&) -> TSharedPtr<IProtokitePlaytestVideoFrameSource>
		{
			return MakeShared<ProtokitePlaytestVideoTesting::FTestVideoFrameSource>(
				FitVideoSizeInside(FIntPoint(1280, 720), MaxVideoSize));
		});
		Fixture.AnswerConfig(FProtokitePlaytestFakeTransport::Status(200, ConfigBody(GameVersionId, false, /*bVideoRecording*/ true)));
		return Uploader;
	}

	/** Half a second of frames, waiting after each for the worker, so the file has something in it. */
	void RecordHalfASecondOfPlay(FPlaytestFixture& Fixture)
	{
		for (int32 Frame = 0; Frame < 30; ++Frame)
		{
			Fixture.Playtest->TickVideoRecordingForTesting(1.f / 60.f);
			Fixture.Playtest->WaitUntilVideoWrittenForTesting();
		}
	}

	/** Waits for a stopped recording's file and hands the finish over on the next video frame, where an upload begins. */
	void ApplyTheFinishedRecording(FPlaytestFixture& Fixture)
	{
		Fixture.Playtest->WaitUntilVideoWrittenForTesting();
		Fixture.Playtest->TickVideoRecordingForTesting(1.f / 60.f);
	}
#endif
}

/**
 * With Start Session And Recording Automatically off nothing starts by itself, ready as the playtest is; the start calls
 * are what start a session and a recording. The counter-case is the calls themselves, so a build where nothing ever starts
 * fails here too.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadControlAutomaticOffStartsNothingTest,
	"Protokite.Playtest.UploadControl.AutomaticOffStartsNothingUntilAsked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadControlAutomaticOffStartsNothingTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedStartAutomatically Automatic(false);
	FPlaytestFixture Fixture;
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);
	// Another player signs in: their Flock session is the latest.
	Fixture.RegisterFlockSession(SecondFlockSessionId);
	ExpectPlaytestStatus(*this, TEXT("Ready"), Fixture.Playtest->GetStatus(), EProtokitePlaytestStatus::Ready);
	TestEqual(TEXT("No session starts by itself"), Fixture.SessionStarts(), 0);
	ExpectSessionState(*this, TEXT("Not started"), Fixture.Playtest->GetPlaytestSessionState(), EProtokitePlaytestSessionState::NotStarted);

	TestTrue(TEXT("The game starts one"), Fixture.Playtest->StartPlaytestSession());
	TestEqual(TEXT("It is sent"), Fixture.SessionStarts(), 1);
	TestEqualSensitive(TEXT("Naming the latest Flock session"), StringMember(Fixture.LastSessionStartBody(), TEXT("flock_session_id")),
		FString(SecondFlockSessionId));
	ExpectSessionState(*this, TEXT("Started"), Fixture.Playtest->GetPlaytestSessionState(), EProtokitePlaytestSessionState::Started);
	return true;
}

/** Every refusal of StartPlaytestSession: not ready, one starting, one open, a player who chose nothing. Each says why. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadControlStartSessionRefusalsTest,
	"Protokite.Playtest.UploadControl.StartingASessionTwiceIsRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadControlStartSessionRefusalsTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	{
		FScopedStartAutomatically Automatic(false);
		FPlaytestFixture Fixture;
		FPlaytestLogCapture Capture;
		TestFalse(TEXT("Not ready before the Flock SDK is"), Fixture.Playtest->StartPlaytestSession());
		TestEqual(TEXT("Saying so, with the status"), Capture.LinesContaining(DescribePlaytestStatus(Fixture.Playtest->GetStatus())).Num(), 1);

		Fixture.StartFlock();
		TestTrue(TEXT("Ready, it is asked for"), Fixture.Playtest->StartPlaytestSession());
		TestEqual(TEXT("And waits for a Flock session"), Fixture.SessionStarts(), 0);
		TestFalse(TEXT("A second ask while it waits is refused"), Fixture.Playtest->StartPlaytestSession());
		TestEqual(TEXT("As already starting"), Capture.LinesContaining(TEXT("one is already starting")).Num(), 1);

		Fixture.RegisterFlockSession(FirstFlockSessionId);
		TestEqual(TEXT("One start is sent"), Fixture.SessionStarts(), 1);
		TestFalse(TEXT("A start while it is open is refused"), Fixture.Playtest->StartPlaytestSession());
		TestEqual(TEXT("As open"), Capture.LinesContaining(TEXT("one is open")).Num(), 1);
		TestEqual(TEXT("Still one start"), Fixture.SessionStarts(), 1);
	}
	{
		// The launch's own start is already asked for while the setting is on.
		FScopedStartAutomatically Automatic(true);
		FPlaytestFixture Fixture;
		Fixture.StartFlock();
		TestFalse(TEXT("With the setting on, the launch's start is already on its way"), Fixture.Playtest->StartPlaytestSession());
		Fixture.RegisterFlockSession(FirstFlockSessionId);
		TestEqual(TEXT("One start"), Fixture.SessionStarts(), 1);
	}
	{
		FScopedStartAutomatically Automatic(false);
		FPlaytestFixture Fixture(true, true, EProtokitePlaytestConsentChoice::Nothing);
		FPlaytestLogCapture Capture;
		Fixture.StartFlock();
		Fixture.RegisterFlockSession(FirstFlockSessionId);
		TestFalse(TEXT("A player who chose nothing gets no session"), Fixture.Playtest->StartPlaytestSession());
		TestEqual(TEXT("Nothing is sent"), Fixture.SessionStarts(), 0);
		TestEqual(TEXT("The reason is the player's answer"),
			Capture.LinesContaining(FString(TEXT("No Protokite session is started: ")) + DescribePlaytestStatus(EProtokitePlaytestStatus::PlayerRefusedPlaytest)).Num(), 1);
	}
	return true;
}

/** After an end, StartPlaytestSession starts a new one, naming the latest Flock session (owner, 2026-11-19). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadControlStartAfterEndTest,
	"Protokite.Playtest.UploadControl.StartingAfterAnEndNamesTheLatestFlockSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadControlStartAfterEndTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);
	TestTrue(TEXT("Precondition: the first session ends"), Fixture.Playtest->EndPlaytestSession());

	// A later Flock session: the open-session rule kept the first one named, the next start names this one.
	Fixture.RegisterFlockSession(SecondFlockSessionId);
	TestEqual(TEXT("Nothing starts by itself after an end"), Fixture.SessionStarts(), 1);
	Fixture.Transport->Answer(PlaytestSessionStartRoute, FProtokitePlaytestFakeTransport::Status(200, SessionStartBody(SecondPlaytestSessionId)));
	TestTrue(TEXT("The game starts another"), Fixture.Playtest->StartPlaytestSession());
	TestEqual(TEXT("A second start is sent"), Fixture.SessionStarts(), 2);
	TestEqualSensitive(TEXT("Naming the latest Flock session"), StringMember(Fixture.LastSessionStartBody(), TEXT("flock_session_id")),
		FString(SecondFlockSessionId));
	TestEqualSensitive(TEXT("And it is the session now"), Fixture.Playtest->GetPlaytestSessionId(), SecondPlaytestSessionId);

	TestTrue(TEXT("It ends in turn"), Fixture.Playtest->EndPlaytestSession());
	TestEqual(TEXT("Its own end is sent"),
		Fixture.Transport->CountRequestsEndingWith(FString::Printf(TEXT("/%s/end"), *SecondPlaytestSessionId)), 1);
	return true;
}

/**
 * The Blueprint nodes reach the calls: in the running game, under -game, each node's call answers and logs why when it
 * refuses, which a node that found no subsystem never does. Without a playtest subsystem they answer false.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadControlNodesTest,
	"Protokite.Playtest.UploadControl.BlueprintNodesReachTheCalls",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadControlNodesTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("No context: no session"), UProtokitePlaytestLibrary::StartPlaytestSession(nullptr));
	TestFalse(TEXT("No context: no recording"), UProtokitePlaytestLibrary::StartVideoRecording(nullptr));

	UGameInstance* GameInstance = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::Game && Context.OwningGameInstance != nullptr)
		{
			GameInstance = Context.OwningGameInstance;
		}
	}
	const UProtokitePlaytestSubsystem* Playtest = GameInstance != nullptr ? GameInstance->GetSubsystem<UProtokitePlaytestSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("A game is running, with its playtest subsystem"), Playtest))
	{
		return false;
	}
	if (Playtest->GetStatus() == EProtokitePlaytestStatus::Ready)
	{
		// A ready playtest would really start them, which a test must not do to the running game.
		AddInfo(TEXT("This project's playtest is ready, so the nodes are not called here."));
		return true;
	}
	FPlaytestLogCapture Capture;
	TestFalse(TEXT("The session node is refused"), UProtokitePlaytestLibrary::StartPlaytestSession(GameInstance->GetWorld()));
	TestEqual(TEXT("By the call, which says why"), Capture.LinesContaining(FString(TEXT("No Protokite session is started: "))
		+ DescribePlaytestStatus(Playtest->GetStatus())).Num(), 1);
	TestFalse(TEXT("The recording node is refused"), UProtokitePlaytestLibrary::StartVideoRecording(GameInstance->GetWorld()));
	TestEqual(TEXT("By the call, which says why"), Capture.LinesContaining(TEXT("No video recording is started")).Num(), 1);
	return true;
}

/**
 * The session event: started with the id once Protokite answers, ended with the same id when the end is sent, after which
 * the id is empty. A failed start raises nothing, and neither does a quit's own end.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadControlSessionEventTest,
	"Protokite.Playtest.UploadControl.TheSessionEventSaysWhenASessionOpensAndCloses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadControlSessionEventTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	{
		FPlaytestFixture Fixture;
		FSessionChangesHeard Sessions(Fixture.Playtest);
		Fixture.StartFlock();
		TestEqual(TEXT("Nothing before a session starts"), Sessions.Heard->Num(), 0);
		Fixture.RegisterFlockSession(FirstFlockSessionId);
		if (TestEqual(TEXT("Started once"), Sessions.Heard->Num(), 1))
		{
			TestEqualSensitive(TEXT("With its id"), (*Sessions.Heard)[0].Key, FString(PlaytestSessionId));
			TestTrue(TEXT("As started"), (*Sessions.Heard)[0].Value);
		}
		TestEqualSensitive(TEXT("The id is readable"), Fixture.Playtest->GetPlaytestSessionId(), FString(PlaytestSessionId));

		TestTrue(TEXT("The game ends it"), Fixture.Playtest->EndPlaytestSession());
		if (TestEqual(TEXT("Ended once"), Sessions.Heard->Num(), 2))
		{
			TestEqualSensitive(TEXT("With the same id"), (*Sessions.Heard)[1].Key, FString(PlaytestSessionId));
			TestFalse(TEXT("As ended"), (*Sessions.Heard)[1].Value);
		}
		TestTrue(TEXT("The id is empty once ended"), Fixture.Playtest->GetPlaytestSessionId().IsEmpty());

		TestTrue(TEXT("Another starts"), Fixture.Playtest->StartPlaytestSession());
		Sessions.Heard->Reset();
		Fixture.Playtest->Deinitialize();
		TestEqual(TEXT("A quit's own end sends the end"), Fixture.SessionEnds(), 2);
		TestEqual(TEXT("And raises nothing"), Sessions.Heard->Num(), 0);
	}
	{
		FScopedStartAutomatically Automatic(false);
		FPlaytestFixture Fixture;
		FSessionChangesHeard Sessions(Fixture.Playtest);
		Fixture.Transport->Answer(PlaytestSessionStartRoute, FProtokitePlaytestFakeTransport::Status(500, TEXT("{\"detail\":\"Internal Server Error\"}")));
		Fixture.StartFlock();
		Fixture.RegisterFlockSession(FirstFlockSessionId);
		TestTrue(TEXT("Precondition: a start is asked for"), Fixture.Playtest->StartPlaytestSession());
		TestEqual(TEXT("Precondition: it was sent"), Fixture.SessionStarts(), 1);
		TestEqual(TEXT("A failed start raises nothing"), Sessions.Heard->Num(), 0);
	}
	return true;
}

#if WITH_PROTOKITE_PLAYTEST_VIDEO

/**
 * Several finished recordings this launch holds count at the size they are, not at the room each reserved to grow into: a
 * launch that records several short parts before any session does not run out of budget. When the budget is truly full,
 * StartVideoRecording says no, with the reason.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadControlDiskBudgetTest,
	"Protokite.Playtest.UploadControl.HeldRecordingsCountAtTheirSize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadControlDiskBudgetTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedStartAutomatically Automatic(false);
	UProtokitePlaytestSettings* Video = GetMutableDefault<UProtokitePlaytestSettings>();
	const int32 SavedBudgetMb = Video->RecordingsDiskBudgetMb;
	const int32 SavedSizeMb = Video->MaxRecordingSizeMb;
	ON_SCOPE_EXIT
	{
		Video->RecordingsDiskBudgetMb = SavedBudgetMb;
		Video->MaxRecordingSizeMb = SavedSizeMb;
	};
	// Room for three recordings at their size limit.
	Video->RecordingsDiskBudgetMb = 3;
	Video->MaxRecordingSizeMb = 1;
	{
		FPlaytestFixture Fixture;
		ReadyToRecord(Fixture);
		Fixture.StartFlock();
		for (int32 Recording = 0; Recording < 5; ++Recording)
		{
			if (!TestTrue(FString::Printf(TEXT("Recording %d starts"), Recording + 1), Fixture.Playtest->StartVideoRecording()))
			{
				break;
			}
			RecordHalfASecondOfPlay(Fixture);
			Fixture.Playtest->StopVideoRecording();
			ApplyTheFinishedRecording(Fixture);
		}
		TestEqual(TEXT("All five are kept, waiting for a session"), CountRecordingFiles(Fixture), 5);
	}
	{
		FPlaytestFixture Fixture;
		FPlaytestLogCapture Capture;
		ReadyToRecord(Fixture);
		Fixture.StartFlock();
		// Another game still recording holds the whole budget.
		FString Error;
		const TSharedPtr<FProtokitePlaytestRecordingRun> StillRunning = FProtokitePlaytestRecordingRun::Create(
			FPaths::Combine(Fixture.Folder, TEXT("Recordings")), EProtokitePlaytestRecordingKind::Playtest, 3 * 1024 * 1024, Error);
		TestTrue(TEXT("Precondition: the other game's run"), StillRunning.IsValid());
		TestFalse(TEXT("No room: the start is refused"), Fixture.Playtest->StartVideoRecording());
		TestEqual(TEXT("Naming the budget"), Capture.LinesContaining(TEXT("Recordings Disk Budget")).Num(), 1);
	}
	return true;
}

/**
 * Ending a session stops its recording, uploads it to that session and sends the end only once the upload has answered.
 * Meanwhile the session is still ending, so neither another end nor a new start gets in.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadControlEndWaitsForTheUploadTest,
	"Protokite.Playtest.UploadControl.EndingUploadsTheRecordingBeforeTheEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadControlEndWaitsForTheUploadTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	const TSharedRef<FProtokitePlaytestFakeFileUploader> Uploader = ReadyToRecord(Fixture);
	Uploader->bDeferred = true;
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);
	RecordHalfASecondOfPlay(Fixture);
	FUploadUpdatesHeard Heard(Fixture.Playtest);

	TArray<bool> Ended;
	TestTrue(TEXT("The end is taken"), Fixture.Playtest->EndPlaytestSession([&Ended](bool bEnded, const FString&) { Ended.Add(bEnded); }));
	TestFalse(TEXT("The recording stopped"), Fixture.Playtest->IsRecordingVideo());
	TestEqual(TEXT("Its upload began"), Uploader->Uploads.Num(), 1);
	TestEqual(TEXT("To this session"), Uploader->CountUploadsContaining(PlaytestSessionId), 1);
	TestEqual(TEXT("The end waits for its answer"), Fixture.SessionEnds(), 0);
	TestEqual(TEXT("And has not answered"), Ended.Num(), 0);
	TestFalse(TEXT("A second end is refused while it waits"), Fixture.Playtest->EndPlaytestSession());
	TestFalse(TEXT("And so is a new start"), Fixture.Playtest->StartPlaytestSession());

	// A game that starts the next session on hearing the upload: the end has gone by then, so it is not refused.
	TArray<bool> StartedOnTheAnswer;
	UProtokitePlaytestSubsystem* Playtest = Fixture.Playtest;
	const TSharedRef<TArray<FProtokitePlaytestUploadUpdate>> Into = Heard.Heard;
	Heard.Listener->OnUploadChanged = [Into, Playtest, &StartedOnTheAnswer](const FProtokitePlaytestUploadUpdate& Update)
	{
		Into->Add(Update);
		if (Update.State == EProtokitePlaytestUploadState::Uploaded)
		{
			StartedOnTheAnswer.Add(Playtest->StartPlaytestSession());
		}
	};
	Uploader->FlushAll();
	TestEqual(TEXT("The upload answered, so the end is sent"), Fixture.SessionEnds(), 1);
	if (TestEqual(TEXT("The handler tried once"), StartedOnTheAnswer.Num(), 1))
	{
		TestTrue(TEXT("And the next session started"), StartedOnTheAnswer[0]);
	}
	if (TestEqual(TEXT("And answered once"), Ended.Num(), 1))
	{
		TestTrue(TEXT("As ended"), Ended[0]);
	}
	if (TestEqual(TEXT("The upload was heard start and finish"), Heard.Heard->Num(), 2))
	{
		TestTrue(TEXT("Uploaded"), (*Heard.Heard)[1].State == EProtokitePlaytestUploadState::Uploaded);
		TestEqualSensitive(TEXT("To this session"), (*Heard.Heard)[1].PlaytestSessionId, FString(PlaytestSessionId));
	}
	TestEqual(TEXT("A second start was sent"), Fixture.SessionStarts(), 2);
	return true;
}

/**
 * A game that closes while its end waits for an upload still ends the session: the upload cannot finish inside a
 * shutdown, so the end goes at once, and the game's callback hears it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadControlClosingWhileEndingTest,
	"Protokite.Playtest.UploadControl.ClosingWhileAnEndWaitsSendsTheEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadControlClosingWhileEndingTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	const TSharedRef<FProtokitePlaytestFakeFileUploader> Uploader = ReadyToRecord(Fixture);
	Uploader->bDeferred = true;
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);
	RecordHalfASecondOfPlay(Fixture);

	FUploadUpdatesHeard Heard(Fixture.Playtest);
	FSessionChangesHeard Sessions(Fixture.Playtest);
	TArray<bool> Ended;
	TArray<bool> StartedFromTheCallback;
	UProtokitePlaytestSubsystem* Playtest = Fixture.Playtest;
	TestTrue(TEXT("The end is taken"), Fixture.Playtest->EndPlaytestSession([&Ended, &StartedFromTheCallback, Playtest](bool bEnded, const FString&)
	{
		Ended.Add(bEnded);
		StartedFromTheCallback.Add(Playtest->StartPlaytestSession());
	}));
	TestEqual(TEXT("Precondition: it waits for the upload"), Fixture.SessionEnds(), 0);

	Fixture.Playtest->Deinitialize();
	TestEqual(TEXT("Closing sends the end"), Fixture.SessionEnds(), 1);
	TestEqual(TEXT("And the game hears it"), Ended.Num(), 1);
	TestTrue(TEXT("A start from that callback is refused"), StartedFromTheCallback.Num() == 1 && !StartedFromTheCallback[0]);
	TestEqual(TEXT("So no session is left open"), Fixture.SessionStarts(), 1);
	TestEqual(TEXT("And the closing game hears no session end"), Sessions.Heard->Num(), 0);
	Uploader->FlushAll();
	TestEqual(TEXT("The upload answering later sends no second end"), Fixture.SessionEnds(), 1);
	TestEqual(TEXT("Nor raises anything into the closing game"), Heard.Answers().Num(), 0);
	return true;
}

/** The counter-case: an upload that fails still lets the end go, and the recording is kept on disk. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadControlEndAfterAFailedUploadTest,
	"Protokite.Playtest.UploadControl.EndingAfterAFailedUploadKeepsTheRecording",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadControlEndAfterAFailedUploadTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	const TSharedRef<FProtokitePlaytestFakeFileUploader> Uploader = ReadyToRecord(Fixture);
	Uploader->OnStatus(TEXT("/put/"), 500, TEXT("storage is down"));
	Uploader->bDeferred = true;
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);
	RecordHalfASecondOfPlay(Fixture);
	FUploadUpdatesHeard Heard(Fixture.Playtest);

	TestTrue(TEXT("The end is taken"), Fixture.Playtest->EndPlaytestSession());
	TestEqual(TEXT("The end waits for the upload"), Fixture.SessionEnds(), 0);
	Uploader->FlushAll();
	TestEqual(TEXT("The failed upload answered, so the end is sent"), Fixture.SessionEnds(), 1);
	if (TestEqual(TEXT("Answered once"), Heard.Answers().Num(), 1))
	{
		TestTrue(TEXT("As kept"), Heard.Answers()[0].State == EProtokitePlaytestUploadState::Kept);
		TestTrue(TEXT("Saying why"), Heard.Answers()[0].WhyNot.Contains(TEXT("500")));
	}
	TestEqual(TEXT("The recording is kept on disk"), CountRecordingFiles(Fixture), 1);
	return true;
}

/**
 * A recording made while no session is open waits for the next one started, and so does a second one made meanwhile
 * (owner, 2026-11-19). Each is answered Kept, with no session, then uploaded once a session starts.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadControlRecordingsWaitForTheNextSessionTest,
	"Protokite.Playtest.UploadControl.RecordingsMadeWithoutASessionGoToTheNextOne",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadControlRecordingsWaitForTheNextSessionTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedStartAutomatically Automatic(false);
	FPlaytestFixture Fixture;
	FPlaytestLogCapture Capture;
	const TSharedRef<FProtokitePlaytestFakeFileUploader> Uploader = ReadyToRecord(Fixture);
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);
	FUploadUpdatesHeard Heard(Fixture.Playtest);
	TestFalse(TEXT("Nothing records by itself"), Fixture.Playtest->IsRecordingVideo());

	for (int32 Recording = 0; Recording < 2; ++Recording)
	{
		TestTrue(TEXT("The game starts a recording"), Fixture.Playtest->StartVideoRecording());
		TestFalse(TEXT("A second start while it records is refused"), Fixture.Playtest->StartVideoRecording());
		RecordHalfASecondOfPlay(Fixture);
		TestTrue(TEXT("It stops"), Fixture.Playtest->StopVideoRecording());
		ApplyTheFinishedRecording(Fixture);
	}
	TestEqual(TEXT("Each refusal said why"), Capture.LinesContaining(TEXT("one is already recording")).Num(), 2);
	TestEqual(TEXT("Nothing is uploaded without a session"), Uploader->Uploads.Num(), 0);
	if (TestEqual(TEXT("Both were answered"), Heard.Heard->Num(), 2))
	{
		for (const FProtokitePlaytestUploadUpdate& Update : *Heard.Heard)
		{
			TestTrue(TEXT("As kept"), Update.State == EProtokitePlaytestUploadState::Kept);
			TestTrue(TEXT("With no session"), Update.PlaytestSessionId.IsEmpty());
			TestTrue(TEXT("Waiting for the next one"), Update.WhyNot.Contains(TEXT("next one")));
		}
	}
	TestEqual(TEXT("Both are on disk"), CountRecordingFiles(Fixture), 2);

	Heard.Heard->Reset();
	Uploader->bDeferred = true;
	TestTrue(TEXT("A session starts"), Fixture.Playtest->StartPlaytestSession());
	TestEqual(TEXT("Both are sent to it"), Uploader->CountUploadsContaining(PlaytestSessionId), 2);

	// Its end waits for both answers, not the first.
	TestTrue(TEXT("The session ends"), Fixture.Playtest->EndPlaytestSession());
	Uploader->FlushOne();
	TestEqual(TEXT("One answer is not enough"), Fixture.SessionEnds(), 0);
	Uploader->FlushOne();
	TestEqual(TEXT("Both answered, the end is sent"), Fixture.SessionEnds(), 1);
	TestEqual(TEXT("Each heard start and finish"), Heard.Starts(), 2);
	TestEqual(TEXT("Both uploaded"), Heard.Answers().FilterByPredicate([](const FProtokitePlaytestUploadUpdate& Update)
		{ return Update.State == EProtokitePlaytestUploadState::Uploaded; }).Num(), 2);
	TestEqual(TEXT("And deleted"), CountRecordingFiles(Fixture), 0);
	return true;
}

/**
 * With the setting on, a session the game starts after an end records again (owner, 2026-11-19), and the recording the
 * ended session uploaded is gone from disk before the next one starts. With it off, the new session records nothing by
 * itself.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadControlEachSessionRecordsTest,
	"Protokite.Playtest.UploadControl.EachSessionStartedRecordsWhileTheSettingIsOn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadControlEachSessionRecordsTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	{
		FPlaytestFixture Fixture;
		const TSharedRef<FProtokitePlaytestFakeFileUploader> Uploader = ReadyToRecord(Fixture);
		Fixture.StartFlock();
		Fixture.RegisterFlockSession(FirstFlockSessionId);
		RecordHalfASecondOfPlay(Fixture);
		TestTrue(TEXT("Precondition: the first session ends"), Fixture.Playtest->EndPlaytestSession());
		TestEqual(TEXT("Its recording was uploaded"), Uploader->Uploads.Num(), 1);
		TestEqual(TEXT("And deleted before anything else starts"), CountRecordingFiles(Fixture), 0);
		TestFalse(TEXT("Nothing records after the end"), Fixture.Playtest->IsRecordingVideo());

		// A recording made between the two sessions never goes to the one that ended: it waits for the next.
		FUploadUpdatesHeard Heard(Fixture.Playtest);
		TestTrue(TEXT("The game records between sessions"), Fixture.Playtest->StartVideoRecording());
		RecordHalfASecondOfPlay(Fixture);
		Fixture.Playtest->StopVideoRecording();
		ApplyTheFinishedRecording(Fixture);
		TestEqual(TEXT("It is not sent to the ended session"), Uploader->Uploads.Num(), 1);
		if (TestEqual(TEXT("It is answered once"), Heard.Heard->Num(), 1))
		{
			TestTrue(TEXT("Kept, waiting for a session"), (*Heard.Heard)[0].PlaytestSessionId.IsEmpty()
				&& (*Heard.Heard)[0].State == EProtokitePlaytestUploadState::Kept);
		}

		Fixture.Transport->Answer(PlaytestSessionStartRoute, FProtokitePlaytestFakeTransport::Status(200, SessionStartBody(SecondPlaytestSessionId)));
		TestTrue(TEXT("The game starts another session"), Fixture.Playtest->StartPlaytestSession());
		TestEqual(TEXT("The one made between sessions goes to it"), Uploader->CountUploadsContaining(SecondPlaytestSessionId), 1);
		TestTrue(TEXT("And the new session records"), Fixture.Playtest->IsRecordingVideo());
		RecordHalfASecondOfPlay(Fixture);
		TestTrue(TEXT("It is sent with its session"), Fixture.Playtest->StopVideoRecordingAndUploadIt());
		TestEqual(TEXT("To the second session too"), Uploader->CountUploadsContaining(SecondPlaytestSessionId), 2);
		TestEqual(TEXT("Each deleted once uploaded, none left"), CountRecordingFiles(Fixture), 0);
	}
	{
		// A session started on the frame the last recording is still being saved records once it is.
		FPlaytestFixture Fixture;
		const TSharedRef<FProtokitePlaytestFakeFileUploader> Uploader = ReadyToRecord(Fixture);
		Fixture.StartFlock();
		Fixture.RegisterFlockSession(FirstFlockSessionId);
		TestTrue(TEXT("Precondition: the first session ends"), Fixture.Playtest->EndPlaytestSession());
		TestTrue(TEXT("Precondition: a recording between sessions"), Fixture.Playtest->StartVideoRecording());
		RecordHalfASecondOfPlay(Fixture);
		Fixture.Playtest->StopVideoRecording();
		Fixture.Transport->Answer(PlaytestSessionStartRoute, FProtokitePlaytestFakeTransport::Status(200, SessionStartBody(SecondPlaytestSessionId)));
		TestTrue(TEXT("A session starts while it is saved"), Fixture.Playtest->StartPlaytestSession());
		ApplyTheFinishedRecording(Fixture);
		Fixture.Playtest->TickVideoRecordingForTesting(1.f / 60.f);
		TestTrue(TEXT("The new session records once the last one is saved"), Fixture.Playtest->IsRecordingVideo());
		TestEqual(TEXT("And the saved one went to the new session"), Uploader->CountUploadsContaining(SecondPlaytestSessionId), 1);
	}
	{
		FScopedStartAutomatically Automatic(false);
		FPlaytestFixture Fixture;
		ReadyToRecord(Fixture);
		Fixture.StartFlock();
		Fixture.RegisterFlockSession(FirstFlockSessionId);
		TestTrue(TEXT("Precondition: a session starts"), Fixture.Playtest->StartPlaytestSession());
		TestFalse(TEXT("With the setting off, it records nothing by itself"), Fixture.Playtest->IsRecordingVideo());
	}
	return true;
}

/** Every refusal of StartVideoRecording says why: not ready, the player's answer, the playtest's config, a recording still saving. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadControlStartRecordingRefusalsTest,
	"Protokite.Playtest.UploadControl.StartingARecordingIsRefusedWithTheReason",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadControlStartRecordingRefusalsTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedStartAutomatically Automatic(false);
	{
		FPlaytestFixture Fixture;
		FPlaytestLogCapture Capture;
		ReadyToRecord(Fixture);
		TestFalse(TEXT("Not ready"), Fixture.Playtest->StartVideoRecording());
		Fixture.StartFlock();
		TestTrue(TEXT("Ready, it starts"), Fixture.Playtest->StartVideoRecording());
		RecordHalfASecondOfPlay(Fixture);
		Fixture.Playtest->StopVideoRecording();
		TestFalse(TEXT("Refused while the last one is still being saved"), Fixture.Playtest->StartVideoRecording());
		TestEqual(TEXT("Saying so"), Capture.LinesContaining(TEXT("still being saved")).Num(), 1);
		ApplyTheFinishedRecording(Fixture);
		TestTrue(TEXT("Once saved, another starts"), Fixture.Playtest->StartVideoRecording());
	}
	{
		FPlaytestFixture Fixture(true, true, EProtokitePlaytestConsentChoice::PlayDataOnly);
		FPlaytestLogCapture Capture;
		ReadyToRecord(Fixture);
		Fixture.StartFlock();
		TestFalse(TEXT("The player's answer does not allow it"), Fixture.Playtest->StartVideoRecording());
		TestEqual(TEXT("Saying so"), Capture.LinesContaining(TEXT("does not allow recording the screen")).Num(), 1);
	}
	{
		FPlaytestFixture Fixture;
		FPlaytestLogCapture Capture;
		ReadyToRecord(Fixture);
		Fixture.AnswerConfig(FProtokitePlaytestFakeTransport::Status(200, ConfigBody(GameVersionId, false, /*bVideoRecording*/ false)));
		Fixture.StartFlock();
		TestFalse(TEXT("The playtest does not record video"), Fixture.Playtest->StartVideoRecording());
		TestEqual(TEXT("Saying so"), Capture.LinesContaining(TEXT("does not record video")).Num(), 1);
	}
	return true;
}

/** Recordings an earlier launch left are reported too, marked as from an earlier launch. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadControlEarlierLaunchTest,
	"Protokite.Playtest.UploadControl.AnEarlierLaunchsRecordingIsReportedAsSuch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadControlEarlierLaunchTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Earlier;
	ReadyToRecord(Earlier);
	Earlier.StartFlock();
	Earlier.RegisterFlockSession(FirstFlockSessionId);
	RecordHalfASecondOfPlay(Earlier);
	// The game closes while recording: the recording is kept for a later launch.
	Earlier.Playtest->Deinitialize();

	FPlaytestFixture Next;
	const TSharedRef<FProtokitePlaytestFakeFileUploader> Uploader = ReadyToRecord(Next);
	Next.Playtest->SetVideoRecordingFolderForTesting(FPaths::Combine(Earlier.Folder, TEXT("Recordings")));
	FUploadUpdatesHeard Heard(Next.Playtest);
	Next.StartFlock();
	// What earlier launches left is pushed from a ticker once the Flock SDK is up.
	Heard.TickUntilHeard(5.f);

	TestEqual(TEXT("The earlier recording was uploaded"), Uploader->CountUploadsContaining(PlaytestSessionId), 1);
	if (TestTrue(TEXT("Its start and finish were heard"), Heard.Heard->Num() >= 2 && Heard.Answers().Num() >= 1))
	{
		TestTrue(TEXT("Started first"), (*Heard.Heard)[0].State == EProtokitePlaytestUploadState::Started);
		TestTrue(TEXT("From an earlier launch"), (*Heard.Heard)[0].bFromAnEarlierLaunch);
		TestEqualSensitive(TEXT("To its own session"), (*Heard.Heard)[0].PlaytestSessionId, FString(PlaytestSessionId));
		TestTrue(TEXT("Then uploaded"), Heard.Answers()[0].State == EProtokitePlaytestUploadState::Uploaded);
		TestTrue(TEXT("From an earlier launch"), Heard.Answers()[0].bFromAnEarlierLaunch);
		TestTrue(TEXT("With its bytes"), Heard.Answers()[0].BytesSent > 0);
	}
	return true;
}

#endif // WITH_PROTOKITE_PLAYTEST_VIDEO

#endif // WITH_AUTOMATION_TESTS
