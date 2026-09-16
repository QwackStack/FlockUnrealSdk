// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockLogger.h"
#include "FlockPlaytestRecordingUpload.h"
#include "FlockPlaytestRecordingUploads.h"
#include "FlockProtokiteClient.h"
#include "HAL/FileManager.h"
#include "Http/FlockHttpClient.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/FlockPlaytestFakeFileUploader.h"
#include "Tests/FlockPlaytestFakeTransport.h"

namespace
{
	const FString UploadRoute = TEXT("/recording-upload");
	const FString SignedUrl = TEXT("http://storage.local/put/recording.webm?signature=abc");
	const FString SessionId = TEXT("01M2N94A0CM8JMH48XV1Z735YK");
	const FString SessionVersionId = TEXT("the-session-version");
	const FString ThisLaunchVersionId = TEXT("this-launch-version");

	FString EnvelopedLinkBody(const FString& UploadUrl = SignedUrl)
	{
		return FString::Printf(
			TEXT("{\"error\":null,\"response\":null,\"result\":{\"upload_url\":\"%s\",\"bucket\":\"b\",\"key\":\"k\"}}"), *UploadUrl);
	}

	/** A scratch recordings folder that cleans itself up. */
	struct FUploadsTestFolder
	{
		FString Folder = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(),
			TEXT("FlockPlaytestTests"), FString::Printf(TEXT("Uploads-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
		FString Recordings = FPaths::Combine(Folder, TEXT("Recordings"));

		~FUploadsTestFolder()
		{
			IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
		}
	};

	struct FUploadsFixture
	{
		FUploadsTestFolder Scratch;
		TSharedRef<FFlockPlaytestFakeTransport> Transport = MakeShared<FFlockPlaytestFakeTransport>();
		TSharedRef<FFlockPlaytestFakeFileUploader> Uploader = MakeShared<FFlockPlaytestFakeFileUploader>();
		TSharedPtr<FFlockPlaytestRecordingUploads> Uploads;

		FUploadsFixture()
		{
			const TSharedRef<IFlockLogger> Logger = MakeShared<FFlockNullLogger>();
			FFlockRetryPolicy Policy;
			Policy.MaxRetries = 0;
			Policy.InitialDelaySeconds = 0.f;
			Policy.bUseJitter = false;
			const TSharedRef<FFlockProtokiteClient> Client =
				MakeShared<FFlockProtokiteClient>(MakeShared<FFlockHttpClient>(Transport, Logger), Policy, Logger);
			Uploads = MakeShared<FFlockPlaytestRecordingUploads>(Client, Uploader, Logger);
		}

		TMap<FString, FString> LaunchHeaders() const
		{
			TMap<FString, FString> Headers;
			Headers.Add(TEXT("X-Flock-API-Key"), TEXT("this-launch-key"));
			Headers.Add(TEXT("X-Game-Version-ID"), ThisLaunchVersionId);
			return Headers;
		}

		static FFlockPlaytestRecordingSession Session()
		{
			FFlockPlaytestRecordingSession Value;
			Value.ProtokiteSessionId = SessionId;
			Value.ProtokiteApiUrl = TEXT("http://127.0.0.1:8020");
			Value.FlockGameVersionId = SessionVersionId;
			return Value;
		}

		/** A held run with a finished recording in it, the way one looks when capture has stopped. */
		TSharedPtr<FFlockPlaytestRecordingRun> MakeRunWithRecording(FAutomationTestBase& Test, const FString& Name)
		{
			FString Error;
			const TSharedPtr<FFlockPlaytestRecordingRun> Run = FFlockPlaytestRecordingRun::CreateNamedForTesting(
				Scratch.Recordings, EFlockPlaytestRecordingKind::Playtest, Name, /*ReservedBytes*/ 0, Error);
			if (!Test.TestTrue(FString::Printf(TEXT("Precondition: the run is made (%s)"), *Error), Run.IsValid()))
			{
				return nullptr;
			}
			TArray<uint8> Bytes;
			Bytes.Init(7, 2048);
			Test.TestTrue(TEXT("Precondition: its recording is saved"),
				FFileHelper::SaveArrayToFile(Bytes, *Run->GetVideoFilePath()));
			FString SaveError;
			Run->SaveSession(Session(), SaveError);
			return Run;
		}
	};
}

/**
 * The invariant the whole ticket turns on. Protokite counts a session as having a recording from the moment it issues a
 * link, so a driver that reports the link's own 200 as success looks correct on the dashboard while the video is lost.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadLinkIsNotAnUploadTest,
	"Flock.Playtest.RecordingUpload.ALinkIsNotAnUpload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadLinkIsNotAnUploadTest::RunTest(const FString& Parameters)
{
	FUploadsFixture Fixture;
	const TSharedPtr<FFlockPlaytestRecordingRun> Run = Fixture.MakeRunWithRecording(*this, TEXT("run-a"));
	if (!Run.IsValid())
	{
		return false;
	}
	const FString VideoPath = Run->GetVideoFilePath();

	// The link is handed out happily; storage then refuses the bytes.
	Fixture.Transport->Answer(UploadRoute, FFlockPlaytestFakeTransport::Status(200, EnvelopedLinkBody()));
	Fixture.Uploader->OnStatus(TEXT("/put/"), 500, TEXT("InternalError"));

	TOptional<FFlockPlaytestRecordingUploadOutcome> Outcome;
	Fixture.Uploads->UploadOne(Run.ToSharedRef(), FUploadsFixture::Session(), Fixture.LaunchHeaders(),
		[&Outcome](FFlockPlaytestRecordingUploadOutcome Result) { Outcome = Result; });

	if (!TestTrue(TEXT("It finished"), Outcome.IsSet()))
	{
		return false;
	}
	TestFalse(TEXT("A link the bytes never followed is not an upload"), Outcome->bUploaded);
	TestTrue(TEXT("The recording is kept for a later launch"), IFileManager::Get().FileExists(*VideoPath));
	return true;
}

/** A 2xx from storage is what says the bytes arrived, and the recording is deleted once they have. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadDeletesOnSuccessTest,
	"Flock.Playtest.RecordingUpload.DeletesTheRecordingOnceItIsUploaded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadDeletesOnSuccessTest::RunTest(const FString& Parameters)
{
	FUploadsFixture Fixture;
	const TSharedPtr<FFlockPlaytestRecordingRun> Run = Fixture.MakeRunWithRecording(*this, TEXT("run-b"));
	if (!Run.IsValid())
	{
		return false;
	}
	const FString VideoPath = Run->GetVideoFilePath();

	Fixture.Transport->Answer(UploadRoute, FFlockPlaytestFakeTransport::Status(200, EnvelopedLinkBody()));
	// Signed for video/webm, so a PUT carrying anything else would be refused the way storage refuses it.
	Fixture.Uploader->On(TEXT("/put/")).SignedFor(TEXT("/put/"), TEXT("video/webm"));

	TOptional<FFlockPlaytestRecordingUploadOutcome> Outcome;
	Fixture.Uploads->UploadOne(Run.ToSharedRef(), FUploadsFixture::Session(), Fixture.LaunchHeaders(),
		[&Outcome](FFlockPlaytestRecordingUploadOutcome Result) { Outcome = Result; });

	if (!TestTrue(TEXT("It finished"), Outcome.IsSet()))
	{
		return false;
	}
	TestTrue(TEXT("It was uploaded"), Outcome->bUploaded);
	TestTrue(TEXT("The recording was deleted"), Outcome->bRecordingDeleted);
	TestFalse(TEXT("Its file is gone"), IFileManager::Get().FileExists(*VideoPath));

	if (TestEqual(TEXT("One upload"), Fixture.Uploader->Uploads.Num(), 1))
	{
		TestEqual(TEXT("It sent the content type the link was signed for"),
			Fixture.Uploader->Uploads[0].ContentType, FString(TEXT("video/webm")));
		TestEqual(TEXT("It sent the finished recording, not an empty file"), Fixture.Uploader->Uploads[0].FileBytes, (int64)2048);
	}
	return true;
}

/**
 * A presigned link is signed for a few minutes and is spent once used, so a retry has to ask for another. Reusing the
 * failed one would be refused every time, and the test that proves the difference is the count of link requests.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadRetriesWithAFreshLinkTest,
	"Flock.Playtest.RecordingUpload.RetriesOnceWithAFreshLink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadRetriesWithAFreshLinkTest::RunTest(const FString& Parameters)
{
	FUploadsFixture Fixture;
	const TSharedPtr<FFlockPlaytestRecordingRun> Run = Fixture.MakeRunWithRecording(*this, TEXT("run-c"));
	if (!Run.IsValid())
	{
		return false;
	}

	Fixture.Transport->Answer(UploadRoute, FFlockPlaytestFakeTransport::Status(200, EnvelopedLinkBody()));
	// 403 is what an expired signature answers.
	Fixture.Uploader->OnStatus(TEXT("/put/"), 403, TEXT("AccessDenied"));

	TOptional<FFlockPlaytestRecordingUploadOutcome> Outcome;
	Fixture.Uploads->UploadOne(Run.ToSharedRef(), FUploadsFixture::Session(), Fixture.LaunchHeaders(),
		[&Outcome](FFlockPlaytestRecordingUploadOutcome Result) { Outcome = Result; });

	if (!TestTrue(TEXT("It finished"), Outcome.IsSet()))
	{
		return false;
	}
	TestFalse(TEXT("It was not uploaded"), Outcome->bUploaded);
	TestEqual(TEXT("It asked for a second link rather than reusing the spent one"), Outcome->LinkRequests, 2);
	TestEqual(TEXT("And tried the upload twice"), Fixture.Uploader->Uploads.Num(), 2);
	TestEqual(TEXT("It stops after one retry"), Fixture.Transport->CountRequestsEndingWith(TEXT("/recording-upload")), 2);
	return true;
}

/**
 * Protokite finds a session's playtest from the Game Version ID, so a recording an earlier launch left must be asked for
 * with the version **its own session** started with. Every test run inside one launch, where the two are equal, passes
 * against a version that sends this launch's -- which is why the two ids here are deliberately different.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadUsesTheSessionsVersionTest,
	"Flock.Playtest.RecordingUpload.AsksWithTheSessionsGameVersion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadUsesTheSessionsVersionTest::RunTest(const FString& Parameters)
{
	TMap<FString, FString> LaunchHeaders;
	LaunchHeaders.Add(TEXT("X-Flock-API-Key"), TEXT("this-launch-key"));
	LaunchHeaders.Add(TEXT("X-Game-Version-ID"), ThisLaunchVersionId);

	const TMap<FString, FString> ForUpload =
		FFlockPlaytestRecordingUploads::MakeUploadHeaders(LaunchHeaders, SessionVersionId);

	const FString* Version = ForUpload.Find(TEXT("X-Game-Version-ID"));
	const FString* Key = ForUpload.Find(TEXT("X-Flock-API-Key"));
	if (TestTrue(TEXT("It sends a version"), Version != nullptr) && TestTrue(TEXT("It sends a key"), Key != nullptr))
	{
		TestEqual(TEXT("The session's version, not this launch's"), *Version, SessionVersionId);
		TestEqual(TEXT("This launch's key, because a saved session never holds one"), *Key, FString(TEXT("this-launch-key")));
	}

	// A session saved before the version was recorded leaves this launch's in place rather than sending none.
	const TMap<FString, FString> WithoutSaved = FFlockPlaytestRecordingUploads::MakeUploadHeaders(LaunchHeaders, FString());
	const FString* Fallback = WithoutSaved.Find(TEXT("X-Game-Version-ID"));
	if (TestTrue(TEXT("It still sends a version"), Fallback != nullptr))
	{
		TestEqual(TEXT("This launch's"), *Fallback, ThisLaunchVersionId);
	}
	return true;
}

/** A recording with no session has nowhere to go, and must not be sent anywhere. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadWithoutASessionTest,
	"Flock.Playtest.RecordingUpload.ARecordingWithNoSessionIsNotSent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadWithoutASessionTest::RunTest(const FString& Parameters)
{
	FUploadsFixture Fixture;
	const TSharedPtr<FFlockPlaytestRecordingRun> Run = Fixture.MakeRunWithRecording(*this, TEXT("run-d"));
	if (!Run.IsValid())
	{
		return false;
	}

	Fixture.Transport->Answer(UploadRoute, FFlockPlaytestFakeTransport::Status(200, EnvelopedLinkBody()));
	Fixture.Uploader->On(TEXT("/put/"));

	TOptional<FFlockPlaytestRecordingUploadOutcome> Outcome;
	Fixture.Uploads->UploadOne(Run.ToSharedRef(), FFlockPlaytestRecordingSession(), Fixture.LaunchHeaders(),
		[&Outcome](FFlockPlaytestRecordingUploadOutcome Result) { Outcome = Result; });

	if (!TestTrue(TEXT("It finished"), Outcome.IsSet()))
	{
		return false;
	}
	TestFalse(TEXT("Nothing was uploaded"), Outcome->bUploaded);
	TestEqual(TEXT("No link was asked for"), Fixture.Transport->CountRequestsEndingWith(TEXT("/recording-upload")), 0);
	TestEqual(TEXT("And nothing was sent"), Fixture.Uploader->Uploads.Num(), 0);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
