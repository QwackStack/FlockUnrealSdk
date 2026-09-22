// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

// Both need a recording, which only a build with video can make.
#if WITH_AUTOMATION_TESTS && WITH_PROTOKITE_PLAYTEST_VIDEO

#include "ProtokitePlaytestConsent.h"
#include "ProtokitePlaytestSelfTest.h"
#include "ProtokitePlaytestSubsystem.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#include "Tests/ProtokitePlaytestFakeFileUploader.h"
#include "Tests/ProtokitePlaytestSubsystemTestSupport.h"
#include "Tests/ProtokitePlaytestVideoTestSupport.h"

using namespace ProtokitePlaytestSubsystemTesting;
using namespace ProtokitePlaytestFixtures;
using namespace ProtokitePlaytestVideoTesting;

namespace
{
	/**
	 * The fake transport answers whichever registered fragment is **longest**, and the session-start route
	 * ("/game/sdk/playtest-session") is a prefix of this one, so a bare "/recording-upload" loses to it and the link
	 * request is answered with a session start. The session id is what makes this fragment the longer of the two.
	 */
	const FString UploadLinkRoute = FString(TEXT("/game/sdk/playtest-session/")) + ProtokitePlaytestFixtures::PlaytestSessionId
		+ TEXT("/recording-upload");
	const FString UploadSignedUrl = TEXT("http://storage.local/put/recording.webm?signature=abc");
	const FString UploadFlockSessionId = TEXT("01M2N94AAAAAAAAAAAAAAAAAAA");

	FString UploadLinkBody()
	{
		return FString::Printf(
			TEXT("{\"error\":null,\"response\":null,\"result\":{\"upload_url\":\"%s\",\"bucket\":\"b\",\"key\":\"k\"}}"),
			*UploadSignedUrl);
	}

	/**
	 * A launch that is recording a playtest, with a Protokite session started for it, and the network for both the link
	 * and the upload standing by. Returns the uploader every attempt goes through.
	 */
	TSharedRef<FProtokitePlaytestFakeFileUploader> StartRecordingPlaytest(FAutomationTestBase& Test, FPlaytestFixture& Fixture)
	{
		const TSharedRef<FProtokitePlaytestFakeFileUploader> Uploader = MakeShared<FProtokitePlaytestFakeFileUploader>();
		Uploader->On(TEXT("/put/"));
		Fixture.Playtest->SetFileUploaderForTesting(Uploader);
		Fixture.Transport->Answer(UploadLinkRoute, FProtokitePlaytestFakeTransport::Status(200, UploadLinkBody()));

		Fixture.Playtest->SetVideoFrameSourceFactoryForTesting([](FIntPoint MaxVideoSize, FString&)
			-> TSharedPtr<IProtokitePlaytestVideoFrameSource>
		{
			return MakeShared<FTestVideoFrameSource>(FitVideoSizeInside(FIntPoint(1280, 720), MaxVideoSize));
		});
		Fixture.AnswerConfig(FProtokitePlaytestFakeTransport::Status(200,
			ConfigBody(GameVersionId, /*bHeavyAnalytics*/ false, /*bVideoRecording*/ true)));
		Fixture.StartFlock();

		// The Protokite session is what gives the recording somewhere to be uploaded to.
		Fixture.RegisterFlockSession(UploadFlockSessionId);

		// Half a second of frames, waiting after each for the worker, so the file has something in it.
		for (int32 Frame = 0; Frame < 30; ++Frame)
		{
			Fixture.Playtest->TickVideoRecordingForTesting(1.f / 60.f);
			Fixture.Playtest->WaitUntilVideoWrittenForTesting();
		}
		Test.TestTrue(TEXT("Precondition: it is recording"), Fixture.Playtest->IsRecordingVideo());
		return Uploader;
	}
}

/**
 * The counter-case for the one below, and the reason it means anything: stopping on purpose **does** upload. Without
 * this, a version where uploading never happened at all would pass the teardown test perfectly.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadsWhenAskedToStopTest,
	"Protokite.Playtest.RecordingUpload.StoppingOnPurposeUploadsIt",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadsWhenAskedToStopTest::RunTest(const FString& Parameters)
{
	// The settings a ready playtest needs, set by the test itself: read from the project's own ini, this passes in a
	// project that happens to have playtesting on and fails in every other, which is what a studio's project is.
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	const TSharedRef<FProtokitePlaytestFakeFileUploader> Uploader = StartRecordingPlaytest(*this, Fixture);

	// What the feedback form's button calls.
	TestTrue(TEXT("It stopped the recording"), Fixture.Playtest->StopVideoRecordingAndUploadIt());

	TestEqual(TEXT("It asked for a link"), Fixture.Transport->CountRequestsEndingWith(UploadLinkRoute), 1);
	if (TestEqual(TEXT("And uploaded the recording"), Uploader->Uploads.Num(), 1))
	{
		TestEqual(TEXT("As video/webm"), Uploader->Uploads[0].ContentType, FString(TEXT("video/webm")));
		TestTrue(TEXT("With a file that had something in it"), Uploader->Uploads[0].FileBytes > 0);
	}
	return true;
}

/**
 * A recording finished because the game instance is shutting down must not start an upload. A whole recording cannot be
 * sent inside a shutdown, so starting one there abandons it part-way and loses the video: the recording is left on disk
 * instead, and a later launch pushes it (D10).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestDoesNotUploadAtTeardownTest,
	"Protokite.Playtest.RecordingUpload.TeardownDoesNotStartAnUpload",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestDoesNotUploadAtTeardownTest::RunTest(const FString& Parameters)
{
	// The settings a ready playtest needs, set by the test itself: read from the project's own ini, this passes in a
	// project that happens to have playtesting on and fails in every other, which is what a studio's project is.
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	const TSharedRef<FProtokitePlaytestFakeFileUploader> Uploader = StartRecordingPlaytest(*this, Fixture);

	// The game instance goes away, which finishes the recording.
	Fixture.Playtest->Deinitialize();

	TestEqual(TEXT("No link was asked for"), Fixture.Transport->CountRequestsEndingWith(UploadLinkRoute), 0);
	TestEqual(TEXT("And nothing was uploaded"), Uploader->Uploads.Num(), 0);

	// It is kept rather than lost: a later launch finds it waiting, with the session saved beside it.
	const TArray<FProtokitePlaytestRecordingWaitingToUpload> Waiting =
		FProtokitePlaytestRecordingsFolder::FindRecordingsWaitingToUpload(FPaths::Combine(Fixture.Folder, TEXT("Recordings")));
	if (TestEqual(TEXT("One recording is waiting for a later launch"), Waiting.Num(), 1))
	{
		TestTrue(TEXT("Its file is on disk"), IFileManager::Get().FileExists(*Waiting[0].VideoFilePath));
		TestFalse(TEXT("And it knows which session to go to"), Waiting[0].Session.IsEmpty());
	}
	return true;
}

/**
 * A player who takes the screen recording back after it has started gets what was recorded deleted, not uploaded -- and
 * not kept either. Keeping it would be the same as sending it a launch later, because the session it belongs to is
 * saved beside it, which is the whole point of D10.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestTakingVideoBackDeletesItTest,
	"Protokite.Playtest.RecordingUpload.TakingTheScreenBackDeletesTheRecording",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestTakingVideoBackDeletesItTest::RunTest(const FString& Parameters)
{
	// The settings a ready playtest needs, set by the test itself: read from the project's own ini, this passes in a
	// project that happens to have playtesting on and fails in every other, which is what a studio's project is.
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	const TSharedRef<FProtokitePlaytestFakeFileUploader> Uploader = StartRecordingPlaytest(*this, Fixture);

	// Whoever is waiting to hear about the recording hears why it did not go, the same as every other way out.
	const TStrongObjectPtr<UProtokitePlaytestSelfTestListener> Listener(NewObject<UProtokitePlaytestSelfTestListener>());
	TArray<FString> WhyNot;
	Listener->OnUploadFinished = [&WhyNot](bool bUploaded, const FString& Reason)
	{
		WhyNot.Add(bUploaded ? FString(TEXT("uploaded")) : Reason);
	};
	Fixture.Playtest->OnRecordingUploadFinished.AddDynamic(Listener.Get(), &UProtokitePlaytestSelfTestListener::HandleRecordingUploadFinished);

	// The player changes their mind: play data, but no screen recording.
	Fixture.Playtest->SetPlaytestConsent(EProtokitePlaytestConsentChoice::PlayDataOnly);
	TestFalse(TEXT("Capture stopped"), Fixture.Playtest->IsRecordingVideo());

	// The file is finished on the writer thread, and the next video frame is what applies a finished recording.
	Fixture.Playtest->WaitUntilVideoWrittenForTesting();
	Fixture.Playtest->TickVideoRecordingForTesting(1.f / 60.f);

	TestEqual(TEXT("No link was asked for"), Fixture.Transport->CountRequestsEndingWith(UploadLinkRoute), 0);
	TestEqual(TEXT("And nothing was uploaded"), Uploader->Uploads.Num(), 0);

	const TArray<FProtokitePlaytestRecordingWaitingToUpload> Waiting =
		FProtokitePlaytestRecordingsFolder::FindRecordingsWaitingToUpload(FPaths::Combine(Fixture.Folder, TEXT("Recordings")));
	TestEqual(TEXT("Nothing is left for a later launch to send"), Waiting.Num(), 0);
	TestTrue(TEXT("The recording's own file is gone"), Fixture.Playtest->GetFinishedVideoRecordingPath().IsEmpty());

	if (TestEqual(TEXT("It was reported once"), WhyNot.Num(), 1))
	{
		TestTrue(TEXT("Saying it was the player's doing"), WhyNot[0].Contains(TEXT("not to be recorded")));
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS && WITH_PROTOKITE_PLAYTEST_VIDEO
