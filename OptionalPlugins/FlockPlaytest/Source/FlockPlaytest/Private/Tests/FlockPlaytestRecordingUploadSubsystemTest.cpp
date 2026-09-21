// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

// Both need a recording, which only a build with video can make.
#if WITH_AUTOMATION_TESTS && WITH_FLOCK_PLAYTEST_VIDEO

#include "FlockPlaytestSubsystem.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Tests/FlockPlaytestFakeFileUploader.h"
#include "Tests/FlockPlaytestSubsystemTestSupport.h"
#include "Tests/FlockPlaytestVideoTestSupport.h"

using namespace FlockPlaytestSubsystemTesting;
using namespace FlockPlaytestFixtures;
using namespace FlockPlaytestVideoTesting;

namespace
{
	/**
	 * The fake transport answers whichever registered fragment is **longest**, and the session-start route
	 * ("/game/sdk/playtest-session") is a prefix of this one, so a bare "/recording-upload" loses to it and the link
	 * request is answered with a session start. The session id is what makes this fragment the longer of the two.
	 */
	const FString UploadLinkRoute = FString(TEXT("/game/sdk/playtest-session/")) + FlockPlaytestFixtures::PlaytestSessionId
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
	TSharedRef<FFlockPlaytestFakeFileUploader> StartRecordingPlaytest(FAutomationTestBase& Test, FPlaytestFixture& Fixture)
	{
		const TSharedRef<FFlockPlaytestFakeFileUploader> Uploader = MakeShared<FFlockPlaytestFakeFileUploader>();
		Uploader->On(TEXT("/put/"));
		Fixture.Playtest->SetFileUploaderForTesting(Uploader);
		Fixture.Transport->Answer(UploadLinkRoute, FFlockPlaytestFakeTransport::Status(200, UploadLinkBody()));

		Fixture.Playtest->SetVideoFrameSourceFactoryForTesting([](FIntPoint MaxVideoSize, FString&)
			-> TSharedPtr<IFlockPlaytestVideoFrameSource>
		{
			return MakeShared<FTestVideoFrameSource>(FitVideoSizeInside(FIntPoint(1280, 720), MaxVideoSize));
		});
		Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200,
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadsWhenAskedToStopTest,
	"Flock.Playtest.RecordingUpload.StoppingOnPurposeUploadsIt",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadsWhenAskedToStopTest::RunTest(const FString& Parameters)
{
	FPlaytestFixture Fixture;
	const TSharedRef<FFlockPlaytestFakeFileUploader> Uploader = StartRecordingPlaytest(*this, Fixture);

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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestDoesNotUploadAtTeardownTest,
	"Flock.Playtest.RecordingUpload.TeardownDoesNotStartAnUpload",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestDoesNotUploadAtTeardownTest::RunTest(const FString& Parameters)
{
	FPlaytestFixture Fixture;
	const TSharedRef<FFlockPlaytestFakeFileUploader> Uploader = StartRecordingPlaytest(*this, Fixture);

	// The game instance goes away, which finishes the recording.
	Fixture.Playtest->Deinitialize();

	TestEqual(TEXT("No link was asked for"), Fixture.Transport->CountRequestsEndingWith(UploadLinkRoute), 0);
	TestEqual(TEXT("And nothing was uploaded"), Uploader->Uploads.Num(), 0);

	// It is kept rather than lost: a later launch finds it waiting, with the session saved beside it.
	const TArray<FFlockPlaytestRecordingWaitingToUpload> Waiting =
		FFlockPlaytestRecordingsFolder::FindRecordingsWaitingToUpload(FPaths::Combine(Fixture.Folder, TEXT("Recordings")));
	if (TestEqual(TEXT("One recording is waiting for a later launch"), Waiting.Num(), 1))
	{
		TestTrue(TEXT("Its file is on disk"), IFileManager::Get().FileExists(*Waiting[0].VideoFilePath));
		TestFalse(TEXT("And it knows which session to go to"), Waiting[0].Session.IsEmpty());
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS && WITH_FLOCK_PLAYTEST_VIDEO
