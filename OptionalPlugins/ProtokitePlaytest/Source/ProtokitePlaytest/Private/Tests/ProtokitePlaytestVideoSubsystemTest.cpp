// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS && WITH_PROTOKITE_PLAYTEST_VIDEO

#include "Engine/Engine.h"
#include "ProtokitePlaytestLibrary.h"
#include "ProtokitePlaytestLocalSettings.h"
#include "ProtokitePlaytestVideoFile.h"
#include "ProtokitePlaytestVideoRecording.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Tests/ProtokitePlaytestSubsystemTestSupport.h"
#include "Tests/ProtokitePlaytestTestSupport.h"
#include "Tests/ProtokitePlaytestVideoTestSupport.h"

using namespace ProtokitePlaytestSubsystemTesting;
using namespace ProtokitePlaytestFixtures;
using namespace ProtokitePlaytestVideoTesting;

namespace
{
	/** Sets the project's Video Recording settings for one test and puts the previous values back when it ends. */
	struct FScopedVideoSettings
	{
		UProtokitePlaytestSettings* Settings = GetMutableDefault<UProtokitePlaytestSettings>();
		int32 SavedWidth = Settings->VideoWidth;
		int32 SavedHeight = Settings->VideoHeight;
		int32 SavedFramesPerSecond = Settings->VideoFramesPerSecond;
		int32 SavedBitrateKbps = Settings->VideoBitrateKbps;
		float SavedMaxMinutes = Settings->MaxRecordingMinutes;
		int32 SavedMaxSizeMb = Settings->MaxRecordingSizeMb;
		int32 SavedDiskBudgetMb = Settings->RecordingsDiskBudgetMb;

		explicit FScopedVideoSettings(FIntPoint VideoSize = FIntPoint(64, 36), float MaxMinutes = 60.f, int32 MaxSizeMb = 1536, int32 BitrateKbps = 2000,
			int32 DiskBudgetMb = 4096)
		{
			Settings->VideoWidth = VideoSize.X;
			Settings->VideoHeight = VideoSize.Y;
			Settings->VideoFramesPerSecond = 30;
			Settings->VideoBitrateKbps = BitrateKbps;
			Settings->MaxRecordingMinutes = MaxMinutes;
			Settings->MaxRecordingSizeMb = MaxSizeMb;
			Settings->RecordingsDiskBudgetMb = DiskBudgetMb;
		}

		~FScopedVideoSettings()
		{
			Settings->VideoWidth = SavedWidth;
			Settings->VideoHeight = SavedHeight;
			Settings->VideoFramesPerSecond = SavedFramesPerSecond;
			Settings->VideoBitrateKbps = SavedBitrateKbps;
			Settings->MaxRecordingMinutes = SavedMaxMinutes;
			Settings->MaxRecordingSizeMb = SavedMaxSizeMb;
			Settings->RecordingsDiskBudgetMb = SavedDiskBudgetMb;
		}
	};

	/** What the test frame sources were asked. */
	struct FVideoSourceLog
	{
		int32 SourcesAskedFor = 0;
		int32 SourcesCreated = 0;
		TWeakPtr<FTestVideoFrameSource> LastSource;
		/** While false, the factory finds no viewport yet. */
		bool bViewportExists = true;
	};

	FString RecordingsFolder(const FPlaytestFixture& Fixture)
	{
		return FPaths::Combine(Fixture.Folder, TEXT("Recordings"));
	}

	/**
	 * Records from test frames of a 1280 by 720 screen, fitted to the Video Recording settings, into the fixture's own
	 * folder. While the log's bViewportExists is false, no viewport is found yet.
	 */
	TSharedRef<FVideoSourceLog> UseTestVideoFrames(FPlaytestFixture& Fixture, bool bNoisyPictures = false)
	{
		const TSharedRef<FVideoSourceLog> Log = MakeShared<FVideoSourceLog>();
		Fixture.Playtest->SetVideoRecordingFolderForTesting(RecordingsFolder(Fixture));
		Fixture.Playtest->SetVideoFrameSourceFactoryForTesting([Log, bNoisyPictures](FIntPoint MaxVideoSize, FString& OutWhyNot)
			-> TSharedPtr<IProtokitePlaytestVideoFrameSource>
		{
			++Log->SourcesAskedFor;
			if (!Log->bViewportExists)
			{
				return nullptr;
			}
			++Log->SourcesCreated;
			const TSharedRef<FTestVideoFrameSource> Source = MakeShared<FTestVideoFrameSource>(FitVideoSizeInside(FIntPoint(1280, 720), MaxVideoSize));
			Source->bNoisyPictures = bNoisyPictures;
			Log->LastSource = Source;
			return Source;
		});
		return Log;
	}

	void StartWithVideo(FPlaytestFixture& Fixture, bool bVideoRecording)
	{
		Fixture.AnswerConfig(FProtokitePlaytestFakeTransport::Status(200, ConfigBody(GameVersionId, /*bHeavyAnalytics*/ false, bVideoRecording)));
		Fixture.StartFlock();
	}

	/** Plays Seconds of frames, each FrameSeconds long, waiting after each for the worker to encode what it was handed. */
	void PlayVideoFrames(FPlaytestFixture& Fixture, double Seconds, double FrameSeconds = 1.0 / 60.0)
	{
		const int32 Frames = FMath::RoundToInt(Seconds / FrameSeconds);
		for (int32 Index = 0; Index < Frames; ++Index)
		{
			Fixture.Playtest->TickVideoRecordingForTesting(static_cast<float>(FrameSeconds));
			Fixture.Playtest->WaitUntilVideoWrittenForTesting();
		}
	}

	/** Waits for the worker, then ticks once so the finished file is applied the way the game's next frame applies it. */
	FString WaitForTheFile(FPlaytestFixture& Fixture)
	{
		Fixture.Playtest->WaitUntilVideoWrittenForTesting();
		Fixture.Playtest->TickVideoRecordingForTesting(1.f / 60.f);
		return Fixture.Playtest->GetFinishedVideoRecordingPath();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoSubsystemVideoOffCreatesNothingTest,
	"Protokite.Playtest.Video.Subsystem.VideoOffCreatesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoSubsystemVideoOffCreatesNothingTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video;
	FPlaytestFixture Fixture;
	const TSharedRef<FVideoSourceLog> Log = UseTestVideoFrames(Fixture);
	StartWithVideo(Fixture, /*bVideoRecording*/ false);
	ExpectPlaytestStatus(*this, TEXT("Ready"), Fixture.Playtest->GetStatus(), EProtokitePlaytestStatus::Ready);

	PlayVideoFrames(Fixture, 2.0);
	TestEqual(TEXT("No frame source is asked for"), Log->SourcesAskedFor, 0);
	TestFalse(TEXT("Nothing records"), Fixture.Playtest->IsRecordingVideo());
	TestFalse(TEXT("The video ticker does not run"), Fixture.Playtest->IsVideoTickerRunningForTesting());
	TestEqual(TEXT("No file is written"), RecordingFilesIn(RecordingsFolder(Fixture)).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoSubsystemRecordsWhenThePlaytestTurnsVideoOnTest,
	"Protokite.Playtest.Video.Subsystem.RecordsWhenThePlaytestTurnsVideoOn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoSubsystemRecordsWhenThePlaytestTurnsVideoOnTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video(FIntPoint(64, 64));
	FPlaytestFixture Fixture;
	const TSharedRef<FVideoSourceLog> Log = UseTestVideoFrames(Fixture);
	FPlaytestLogCapture Capture;
	StartWithVideo(Fixture, /*bVideoRecording*/ true);

	TestTrue(TEXT("Recording starts once the config is loaded, before any Flock session"), Fixture.Playtest->IsRecordingVideo());
	TestFalse(TEXT("The Blueprint stop node answers false with no playtest subsystem"), UProtokitePlaytestLibrary::StopVideoRecording(nullptr));
	TestFalse(TEXT("The Blueprint recording check answers false with no playtest subsystem"), UProtokitePlaytestLibrary::IsRecordingVideo(nullptr));
	TestEqual(TEXT("No Protokite session was needed"), Fixture.SessionStarts(), 0);
	TestEqual(TEXT("One frame source"), Log->SourcesCreated, 1);
	if (const TSharedPtr<FTestVideoFrameSource> Source = Log->LastSource.Pin())
	{
		TestEqual(TEXT("The screen's shape fitted inside Video Width by Video Height"), Source->FrameSize, FIntPoint(64, 36));
	}

	PlayVideoFrames(Fixture, 1.0);
	const TArray<FString> WhileRecording = RecordingFilesIn(RecordingsFolder(Fixture));
	if (TestEqual(TEXT("One file while recording"), WhileRecording.Num(), 1))
	{
		TestTrue(TEXT("Named as unfinished"), WhileRecording[0].EndsWith(TEXT(".webm.part")));
	}

	TestTrue(TEXT("The game stops it"), Fixture.Playtest->StopVideoRecording());
	TestFalse(TEXT("A second stop changes nothing"), Fixture.Playtest->StopVideoRecording());
	const FString Path = WaitForTheFile(Fixture);
	TestTrue(TEXT("The file is saved under its final name"), Path.EndsWith(TEXT(".webm")));
	TestTrue(TEXT("Named as the playtest's recording"), FPaths::GetCleanFilename(Path).StartsWith(TEXT("recording-")));
	TestEqual(TEXT("In a run's folder under Playtest"), FPaths::GetCleanFilename(FPaths::GetPath(FPaths::GetPath(Path))),
		FString(FProtokitePlaytestRecordingsFolder::PlaytestFolderName));
	const TArray<FString> Afterwards = RecordingFilesIn(RecordingsFolder(Fixture));
	if (TestEqual(TEXT("One file afterwards"), Afterwards.Num(), 1))
	{
		TestEqual(TEXT("The finished one"), FPaths::GetCleanFilename(Afterwards[0]), FPaths::GetCleanFilename(Path));
	}

	FVideoFileRead File;
	if (TestTrue(TEXT("The file reads back"), ReadVideoFile(Path, File)))
	{
		TestEqual(TEXT("Width"), File.Width, 64);
		TestEqual(TEXT("Height"), File.Height, 36);
		TestEqual(TEXT("A second of play at 30 frames a second"), File.Frames.Num(), 30);
		TestTrue(TEXT("The finished file states its segment's size"), File.bSegmentSizeWritten);
		if (File.Frames.Num() > 0)
		{
			TestEqual(TEXT("The last is shown 967 ms in"), File.Frames.Last().TimestampMs, 967LL);
		}
		FIntPoint PictureSize;
		double Brightness = -1.0;
		TestEqual(TEXT("Every frame decodes"), DecodeVideoFile(File, PictureSize, Brightness), File.Frames.Num());
	}
	TestEqual(TEXT("The saved file is logged once"), Capture.LinesContaining(TEXT("Video saved to")).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoSubsystemStopsForGoodAtTheLengthLimitTest,
	"Protokite.Playtest.Video.Subsystem.StopsForGoodAtTheLengthLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoSubsystemStopsForGoodAtTheLengthLimitTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video(FIntPoint(64, 36), /*MaxMinutes*/ 0.1f);
	FPlaytestFixture Fixture;
	const TSharedRef<FVideoSourceLog> Log = UseTestVideoFrames(Fixture);
	FPlaytestLogCapture Capture;
	StartWithVideo(Fixture, true);
	Fixture.RegisterFlockSession(FirstFlockSessionId);
	TestEqual(TEXT("Precondition: the Protokite session started"), Fixture.SessionStarts(), 1);

	PlayVideoFrames(Fixture, 7.0);
	TestFalse(TEXT("Recording stopped at six seconds, the shortest limit the settings allow"), Fixture.Playtest->IsRecordingVideo());
	FVideoFileRead File;
	if (TestTrue(TEXT("The file reads back"), ReadVideoFile(WaitForTheFile(Fixture), File)))
	{
		TestEqual(TEXT("Six seconds of frames"), File.Frames.Num(), 180);
		TestTrue(TEXT("None shown at or after the limit"), File.Frames.Num() > 0 && File.Frames.Last().TimestampMs < 6000);
	}
	TestEqual(TEXT("The log says why it stopped"), Capture.LinesContaining(TEXT("length limit")).Num(), 1);

	// Playtesting going away and coming back starts no second recording.
	Fixture.Flock->ShutdownSdk();
	Fixture.StartFlock();
	ExpectPlaytestStatus(*this, TEXT("Ready again"), Fixture.Playtest->GetStatus(), EProtokitePlaytestStatus::Ready);
	PlayVideoFrames(Fixture, 1.0);
	TestEqual(TEXT("Still one frame source"), Log->SourcesCreated, 1);
	TestFalse(TEXT("Not recording"), Fixture.Playtest->IsRecordingVideo());
	TestEqual(TEXT("Still one file"), RecordingFilesIn(RecordingsFolder(Fixture)).Num(), 1);

	TestTrue(TEXT("The session can still be ended"), Fixture.Playtest->EndPlaytestSession());
	TestEqual(TEXT("And its end is sent"), Fixture.SessionEnds(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoSubsystemStopsForGoodAtTheSizeLimitTest,
	"Protokite.Playtest.Video.Subsystem.StopsForGoodAtTheSizeLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoSubsystemStopsForGoodAtTheSizeLimitTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video(FIntPoint(320, 180), /*MaxMinutes*/ 60.f, /*MaxSizeMb*/ 1, /*BitrateKbps*/ 20000);
	FPlaytestFixture Fixture;
	const TSharedRef<FVideoSourceLog> Log = UseTestVideoFrames(Fixture, /*bNoisyPictures*/ true);
	FPlaytestLogCapture Capture;
	StartWithVideo(Fixture, true);

	for (int32 Index = 0; Index < 1800 && Fixture.Playtest->IsRecordingVideo(); ++Index)
	{
		PlayVideoFrames(Fixture, 1.0 / 30.0, 1.0 / 30.0);
	}
	TestFalse(TEXT("Recording stopped"), Fixture.Playtest->IsRecordingVideo());
	const FString Path = WaitForTheFile(Fixture);
	FVideoFileRead File;
	if (TestTrue(TEXT("The file reads back"), ReadVideoFile(Path, File)))
	{
		TestTrue(TEXT("Frames were written"), File.Frames.Num() > 0);
		TestTrue(FString::Printf(TEXT("The file, %lld bytes, stays within one megabyte"), File.FileBytes), File.FileBytes <= 1024 * 1024);
		TestTrue(TEXT("And came close to it"), File.FileBytes > 512 * 1024);
	}
	TestEqual(TEXT("The log says why it stopped"), Capture.LinesContaining(TEXT("size limit")).Num(), 1);
	TestEqual(TEXT("One frame source"), Log->SourcesCreated, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoSubsystemBackgroundTimeIsNotRecordedTest,
	"Protokite.Playtest.Video.Subsystem.BackgroundTimeIsNotRecorded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoSubsystemBackgroundTimeIsNotRecordedTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video;
	FPlaytestFixture Fixture;
	UseTestVideoFrames(Fixture);
	StartWithVideo(Fixture, true);

	PlayVideoFrames(Fixture, 1.0);
	Fixture.Playtest->SetBackgroundedForTesting(true);
	PlayVideoFrames(Fixture, 2.0);
	Fixture.Playtest->SetBackgroundedForTesting(false);
	// The first frame time after the return carries the time away.
	Fixture.Playtest->TickVideoRecordingForTesting(30.f);
	PlayVideoFrames(Fixture, 1.0);
	Fixture.Playtest->StopVideoRecording();

	FVideoFileRead File;
	if (TestTrue(TEXT("The file reads back"), ReadVideoFile(WaitForTheFile(Fixture), File)))
	{
		TestEqual(TEXT("Two seconds of play"), File.Frames.Num(), 60);
		int64 LongestGapMs = 0;
		for (int32 Index = 1; Index < File.Frames.Num(); ++Index)
		{
			LongestGapMs = FMath::Max(LongestGapMs, File.Frames[Index].TimestampMs - File.Frames[Index - 1].TimestampMs);
		}
		TestTrue(FString::Printf(TEXT("No gap where the game was away (longest %lld ms)"), LongestGapMs), LongestGapMs <= 50);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoSubsystemStopsWhenPlaytestingStopsTest,
	"Protokite.Playtest.Video.Subsystem.StopsWhenPlaytestingStopsAndNeverRecordsTwice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoSubsystemStopsWhenPlaytestingStopsTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video;
	FPlaytestFixture Fixture;
	const TSharedRef<FVideoSourceLog> Log = UseTestVideoFrames(Fixture);
	FPlaytestLogCapture Capture;
	StartWithVideo(Fixture, true);
	PlayVideoFrames(Fixture, 1.0);

	Fixture.Flock->ShutdownSdk();
	TestFalse(TEXT("The Flock SDK shutting down stops the recording"), Fixture.Playtest->IsRecordingVideo());
	TestFalse(TEXT("Its file is kept"), WaitForTheFile(Fixture).IsEmpty());
	TestEqual(TEXT("The log says why"), Capture.LinesContaining(TEXT("playtesting stopped")).Num(), 1);
	TestFalse(TEXT("The video ticker stops once the file is saved"), Fixture.Playtest->IsVideoTickerRunningForTesting());

	Fixture.StartFlock();
	ExpectPlaytestStatus(*this, TEXT("Ready again"), Fixture.Playtest->GetStatus(), EProtokitePlaytestStatus::Ready);
	TestFalse(TEXT("No second recording starts"), Fixture.Playtest->IsRecordingVideo());
	TestEqual(TEXT("One frame source"), Log->SourcesCreated, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoSubsystemGameInstanceShutDownFinishesTheFileTest,
	"Protokite.Playtest.Video.Subsystem.GameInstanceShutDownFinishesTheFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoSubsystemGameInstanceShutDownFinishesTheFileTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video;
	FPlaytestFixture Fixture;
	UseTestVideoFrames(Fixture);
	StartWithVideo(Fixture, true);
	PlayVideoFrames(Fixture, 1.0);

	Fixture.Playtest->Deinitialize();
	const FString Path = Fixture.Playtest->GetFinishedVideoRecordingPath();
	TestFalse(TEXT("The file is finished before shutting down returns"), Path.IsEmpty());
	TestFalse(TEXT("The video ticker is stopped"), Fixture.Playtest->IsVideoTickerRunningForTesting());
	const TArray<FString> Files = RecordingFilesIn(RecordingsFolder(Fixture));
	if (TestEqual(TEXT("One file"), Files.Num(), 1))
	{
		TestTrue(TEXT("Under its final name"), Files[0].EndsWith(TEXT(".webm")));
	}
	FVideoFileRead File;
	if (TestTrue(TEXT("The file reads back"), ReadVideoFile(Path, File)))
	{
		TestTrue(TEXT("The finished file states its segment's size"), File.bSegmentSizeWritten);
		TestEqual(TEXT("A second of frames"), File.Frames.Num(), 30);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoRecordingDropsFramesWhenEncodingFallsBehindTest,
	"Protokite.Playtest.Video.Recording.DropsFramesWhenEncodingFallsBehind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoRecordingDropsFramesWhenEncodingFallsBehindTest::RunTest(const FString& Parameters)
{
	const FString Folder = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("ProtokitePlaytestTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FProtokitePlaytestVideoSettings VideoSettings;
	FEvent* Gate = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ true);
	FString Error;
	{
		const TSharedRef<FTestVideoFrameSource> Source = MakeShared<FTestVideoFrameSource>(FIntPoint(64, 36));
		const TSharedPtr<FProtokitePlaytestVideoRecording> Recording = FProtokitePlaytestVideoRecording::Start(Source, VideoSettings,
			FPaths::Combine(Folder, TEXT("held.webm")), Error, [Gate]() { Gate->Wait(); });
		if (!TestTrue(TEXT("The recording starts"), Recording.IsValid()))
		{
			AddError(Error);
			Gate->Trigger();
			FPlatformProcess::ReturnSynchEventToPool(Gate);
			return true;
		}

		// A 30 fps game against a 30 fps capture: every frame is captured, and the worker holds on the first.
		for (int32 Index = 0; Index < 60; ++Index)
		{
			Recording->AddFrame(1.0 / 30.0);
		}
		Gate->Trigger();
		Recording->WaitUntilWritten();
		Recording->StopCapturing(EProtokitePlaytestVideoStopReason::StoppedByGame);
		Recording->WaitUntilWritten();

		const FProtokitePlaytestVideoRecordingSummary Summary = Recording->GetSummary();
		TestEqual(TEXT("Eight frames waited, and the one handed over at the stop"), Summary.FramesWritten, 9);
		TestEqual(TEXT("The rest were dropped and counted"), Summary.FramesDroppedBecauseEncodingFellBehind, 51);
	}
	FPlatformProcess::ReturnSynchEventToPool(Gate);
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoSubsystemNothingCapturedKeepsNoFileTest,
	"Protokite.Playtest.Video.Subsystem.NothingCapturedKeepsNoFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoSubsystemNothingCapturedKeepsNoFileTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video;
	FPlaytestFixture Fixture;
	UseTestVideoFrames(Fixture);
	FPlaytestLogCapture Capture;
	StartWithVideo(Fixture, true);
	TestTrue(TEXT("Precondition: recording"), Fixture.Playtest->IsRecordingVideo());

	TestTrue(TEXT("Stopped before any frame arrived"), Fixture.Playtest->StopVideoRecording());
	TestTrue(TEXT("No file is saved"), WaitForTheFile(Fixture).IsEmpty());
	TestEqual(TEXT("And none is left behind"), RecordingFilesIn(RecordingsFolder(Fixture)).Num(), 0);
	TestEqual(TEXT("The log says so"), Capture.LinesContaining(TEXT("before any frame was captured")).Num(), 1);
	TestFalse(TEXT("The video ticker stops"), Fixture.Playtest->IsVideoTickerRunningForTesting());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoSubsystemCouldNotStartWarnsOnceTest,
	"Protokite.Playtest.Video.Subsystem.CouldNotStartWarnsOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoSubsystemCouldNotStartWarnsOnceTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video;
	FPlaytestFixture Fixture;
	const TSharedRef<FVideoSourceLog> Log = UseTestVideoFrames(Fixture);
	// A folder name Windows refuses, so the file cannot be created.
	Fixture.Playtest->SetVideoRecordingFolderForTesting(FPaths::Combine(Fixture.Folder, TEXT("not|a|folder")));
	FPlaytestLogCapture Capture;
	StartWithVideo(Fixture, true);
	PlayVideoFrames(Fixture, 1.0);
	Fixture.Flock->ShutdownSdk();
	Fixture.StartFlock();
	PlayVideoFrames(Fixture, 1.0);

	const TArray<FPlaytestLogCapture::FLine> Lines = Capture.LinesContaining(TEXT("the recording could not start"));
	if (TestEqual(TEXT("Warned once"), Lines.Num(), 1))
	{
		TestEqual(TEXT("As a warning"), static_cast<int32>(Lines[0].Verbosity), static_cast<int32>(ELogVerbosity::Warning));
	}
	TestEqual(TEXT("Tried once"), Log->SourcesCreated, 1);
	TestFalse(TEXT("Not recording"), Fixture.Playtest->IsRecordingVideo());
	TestFalse(TEXT("The video ticker does not run"), Fixture.Playtest->IsVideoTickerRunningForTesting());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoRecordingWritesNothingAfterTheSizeLimitTest,
	"Protokite.Playtest.Video.Recording.WritesNothingAfterTheSizeLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoRecordingWritesNothingAfterTheSizeLimitTest::RunTest(const FString& Parameters)
{
	const FString Folder = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("ProtokitePlaytestTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));

	// Eleven frames of 320 by 180, the sixth of them noise, which takes far more room than the smooth frames around it.
	const auto RecordElevenFrames = [&Folder](const TCHAR* Name, int64 MaxBytes, FProtokitePlaytestVideoRecordingSummary& OutSummary, FString& OutError)
	{
		FProtokitePlaytestVideoSettings VideoSettings;
		VideoSettings.BitrateKbps = 20000;
		VideoSettings.MaxBytes = MaxBytes;
		const TSharedRef<FTestVideoFrameSource> Source = MakeShared<FTestVideoFrameSource>(FIntPoint(320, 180));
		const TSharedPtr<FProtokitePlaytestVideoRecording> Recording = FProtokitePlaytestVideoRecording::Start(Source, VideoSettings,
			FPaths::Combine(Folder, Name), OutError);
		if (!Recording.IsValid())
		{
			return false;
		}
		for (int32 Index = 0; Index < 11; ++Index)
		{
			Source->bNoisyPictures = Index == 5;
			Recording->AddFrame(1.0 / 30.0);
			Recording->WaitUntilWritten();
		}
		Recording->StopCapturing(EProtokitePlaytestVideoStopReason::StoppedByGame);
		Recording->WaitUntilWritten();
		OutSummary = Recording->GetSummary();
		return true;
	};

	FProtokitePlaytestVideoRecordingSummary Unlimited;
	FString Error;
	FVideoFileRead Control;
	if (!TestTrue(TEXT("The control recording is made"), RecordElevenFrames(TEXT("control.webm"), 1LL << 40, Unlimited, Error))
		|| !TestTrue(TEXT("And reads back"), ReadVideoFile(Unlimited.FilePath, Control))
		|| !TestEqual(TEXT("With eleven frames"), Control.Frames.Num(), 11))
	{
		AddError(Error);
		IFileManager::Get().DeleteDirectory(*Folder, false, true);
		return true;
	}
	const int64 NoiseBytes = Control.Frames[5].Bytes.Num();
	const int64 SmoothBytes = Control.Frames[6].Bytes.Num();
	TestTrue(FString::Printf(TEXT("Precondition: the noise frame (%lld bytes) is far larger than the one after it (%lld)"), NoiseBytes, SmoothBytes),
		NoiseBytes > SmoothBytes * 2);

	// Room for the first five frames, then for the frame after the noise but not for the noise itself.
	int64 MaxBytes = FProtokitePlaytestVideoFile::FileHeaderBytes;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		MaxBytes += FProtokitePlaytestVideoFile::FrameHeaderBytes + Control.Frames[Index].Bytes.Num();
	}
	MaxBytes += FProtokitePlaytestVideoFile::FrameHeaderBytes + SmoothBytes + (NoiseBytes - SmoothBytes) / 2;

	FProtokitePlaytestVideoRecordingSummary Limited;
	if (TestTrue(TEXT("The limited recording is made"), RecordElevenFrames(TEXT("limited.webm"), MaxBytes, Limited, Error)))
	{
		TestEqual(TEXT("The five frames before the refused one are written, and none after it"), Limited.FramesWritten, 5);
		TestTrue(TEXT("Within the limit"), Limited.BytesWritten <= MaxBytes);
	}
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoRecordingAsksForNoFrameWhileEarlierOnesAreOnTheirWayTest,
	"Protokite.Playtest.Video.Recording.AsksForNoFrameWhileEarlierOnesAreOnTheirWay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoRecordingAsksForNoFrameWhileEarlierOnesAreOnTheirWayTest::RunTest(const FString& Parameters)
{
	const FString Folder = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("ProtokitePlaytestTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FProtokitePlaytestVideoSettings VideoSettings;
	const TSharedRef<FTestVideoFrameSource> Source = MakeShared<FTestVideoFrameSource>(FIntPoint(64, 36));
	Source->bReadyForAnotherFrame = false;
	FString Error;
	{
		const TSharedPtr<FProtokitePlaytestVideoRecording> Recording = FProtokitePlaytestVideoRecording::Start(Source, VideoSettings,
			FPaths::Combine(Folder, TEXT("waiting.webm")), Error);
		if (!TestTrue(TEXT("The recording starts"), Recording.IsValid()))
		{
			AddError(Error);
			return true;
		}

		for (int32 Index = 0; Index < 30; ++Index)
		{
			Recording->AddFrame(1.0 / 30.0);
		}
		TestEqual(TEXT("No frame is asked for while earlier ones are on their way"), Source->FramesAskedFor, 0);
		Source->bReadyForAnotherFrame = true;
		Recording->AddFrame(1.0 / 30.0);
		TestEqual(TEXT("One is asked for once the source is ready"), Source->FramesAskedFor, 1);

		Recording->StopCapturing(EProtokitePlaytestVideoStopReason::StoppedByGame);
		Recording->StopCapturing(EProtokitePlaytestVideoStopReason::StoppedByGame);
		for (int32 Index = 0; Index < 5; ++Index)
		{
			Recording->AddFrame(1.0 / 30.0);
		}
		TestEqual(TEXT("Nothing is asked for after the stop"), Source->FramesAskedFor, 1);
		Recording->WaitUntilWritten();

		const FProtokitePlaytestVideoRecordingSummary Summary = Recording->GetSummary();
		TestEqual(TEXT("Each capture time that passed while waiting is counted, and none after the stop"), Summary.FramesNotReadyInTime, 30);
		TestEqual(TEXT("The one frame is written"), Summary.FramesWritten, 1);
		TestTrue(FString::Printf(TEXT("A second stop changed nothing (error: '%s')"), *Summary.Error), Summary.Error.IsEmpty());
		TestFalse(TEXT("And the file is kept"), Summary.FilePath.IsEmpty());
	}
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoRecordingAStalledWriteDoesNotHoldEncodingUpTest,
	"Protokite.Playtest.Video.Recording.AStalledWriteDoesNotHoldEncodingUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoRecordingAStalledWriteDoesNotHoldEncodingUpTest::RunTest(const FString& Parameters)
{
	const FString Folder = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("ProtokitePlaytestTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FEvent* WriteGate = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ true);
	FString Error;
	{
		const TSharedRef<FTestVideoFrameSource> Source = MakeShared<FTestVideoFrameSource>(FIntPoint(64, 36));
		const TSharedPtr<FProtokitePlaytestVideoRecording> Recording = FProtokitePlaytestVideoRecording::Start(Source, FProtokitePlaytestVideoSettings(),
			FPaths::Combine(Folder, TEXT("stalled.webm")), Error, nullptr, [WriteGate]() { WriteGate->Wait(); return true; });
		if (!TestTrue(TEXT("The recording starts"), Recording.IsValid()))
		{
			AddError(Error);
			WriteGate->Trigger();
			FPlatformProcess::ReturnSynchEventToPool(WriteGate);
			return true;
		}

		// The disk holds the first write for the whole run, the way a single write held it for 2.7 s in a live run. Frames
		// come at the pace a game hands them over, not all at once.
		for (int32 Index = 0; Index < 30; ++Index)
		{
			Recording->AddFrame(1.0 / 30.0);
			FPlatformProcess::Sleep(0.005f);
		}
		const int32 DroppedWhileHeld = Recording->GetSummary().FramesDroppedBecauseEncodingFellBehind;
		WriteGate->Trigger();
		Recording->StopCapturing(EProtokitePlaytestVideoStopReason::StoppedByGame);
		Recording->WaitUntilWritten();

		const FProtokitePlaytestVideoRecordingSummary Summary = Recording->GetSummary();
		TestEqual(TEXT("Encoding kept up while the write was held, so no frame was dropped"), DroppedWhileHeld, 0);
		TestEqual(TEXT("Every frame is written once the disk lets go"), Summary.FramesWritten, 30);
		TestTrue(FString::Printf(TEXT("The held write is measured (%.0f ms)"), Summary.LongestWriteMs), Summary.LongestWriteMs >= 100.0);
	}
	FPlatformProcess::ReturnSynchEventToPool(WriteGate);
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoRecordingDropsFramesWhenWritingFallsBehindTest,
	"Protokite.Playtest.Video.Recording.DropsFramesWhenWritingFallsBehind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoRecordingDropsFramesWhenWritingFallsBehindTest::RunTest(const FString& Parameters)
{
	const FString Folder = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("ProtokitePlaytestTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FEvent* WriteGate = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ true);
	FString Error;
	{
		const TSharedRef<FTestVideoFrameSource> Source = MakeShared<FTestVideoFrameSource>(FIntPoint(64, 36));
		const TSharedPtr<FProtokitePlaytestVideoRecording> Recording = FProtokitePlaytestVideoRecording::Start(Source, FProtokitePlaytestVideoSettings(),
			FPaths::Combine(Folder, TEXT("behind.webm")), Error, nullptr, [WriteGate]() { WriteGate->Wait(); return true; });
		if (!TestTrue(TEXT("The recording starts"), Recording.IsValid()))
		{
			AddError(Error);
			WriteGate->Trigger();
			FPlatformProcess::ReturnSynchEventToPool(WriteGate);
			return true;
		}

		// The disk holds every write while more frames arrive than may wait to be written.
		const int32 FramesPastTheLimit = 20;
		const int32 Frames = FProtokitePlaytestVideoRecording::MaxFramesWaitingToWrite + FramesPastTheLimit;
		for (int32 Index = 0; Index < Frames; ++Index)
		{
			Recording->AddFrame(1.0 / 30.0);
			FPlatformProcess::Sleep(0.002f);
		}
		WriteGate->Trigger();
		Recording->StopCapturing(EProtokitePlaytestVideoStopReason::StoppedByGame);
		Recording->WaitUntilWritten();

		const FProtokitePlaytestVideoRecordingSummary Summary = Recording->GetSummary();
		TestEqual(TEXT("Encoding itself kept up"), Summary.FramesDroppedBecauseEncodingFellBehind, 0);
		TestTrue(FString::Printf(TEXT("Frames past the limit were dropped before encoding (%d)"), Summary.FramesDroppedBecauseWritingFellBehind),
			Summary.FramesDroppedBecauseWritingFellBehind >= FramesPastTheLimit / 2);
		TestEqual(TEXT("Every frame was written or counted as dropped"), Summary.FramesWritten + Summary.FramesDroppedBecauseWritingFellBehind, Frames);
		TestTrue(FString::Printf(TEXT("And the file was kept (error: '%s')"), *Summary.Error), Summary.Error.IsEmpty() && !Summary.FilePath.IsEmpty());
	}
	FPlatformProcess::ReturnSynchEventToPool(WriteGate);
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoSubsystemCannotRecordWarnsOnceTest,
	"Protokite.Playtest.Video.Subsystem.CannotRecordWarnsOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoSubsystemCannotRecordWarnsOnceTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video;
	FPlaytestFixture Fixture;
	const TSharedRef<int32> Asked = MakeShared<int32>(0);
	Fixture.Playtest->SetVideoRecordingFolderForTesting(RecordingsFolder(Fixture));
	Fixture.Playtest->SetVideoFrameSourceFactoryForTesting([Asked](FIntPoint, FString& OutWhyNot) -> TSharedPtr<IProtokitePlaytestVideoFrameSource>
	{
		++*Asked;
		OutWhyNot = TEXT("a reason this test gives");
		return nullptr;
	});
	FPlaytestLogCapture Capture;
	StartWithVideo(Fixture, true);
	PlayVideoFrames(Fixture, 1.0);
	Fixture.Flock->ShutdownSdk();
	Fixture.StartFlock();
	PlayVideoFrames(Fixture, 1.0);

	const TArray<FPlaytestLogCapture::FLine> Lines = Capture.LinesContaining(TEXT("a reason this test gives"));
	if (TestEqual(TEXT("Logged once"), Lines.Num(), 1))
	{
		TestEqual(TEXT("As a warning"), static_cast<int32>(Lines[0].Verbosity), static_cast<int32>(ELogVerbosity::Warning));
		TestTrue(TEXT("Saying the rest carries on"), Lines[0].Message.Contains(TEXT("Everything else in the playtest carries on")));
	}
	TestEqual(TEXT("Asked once"), *Asked, 1);
	TestFalse(TEXT("The video ticker does not run"), Fixture.Playtest->IsVideoTickerRunningForTesting());
	TestFalse(TEXT("A test video is refused too"), Fixture.Playtest->StartTestVideoRecording(5.0));
	TestEqual(TEXT("No file"), RecordingFilesIn(RecordingsFolder(Fixture)).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoSubsystemWaitsForTheGameViewportTest,
	"Protokite.Playtest.Video.Subsystem.WaitsForTheGameViewport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoSubsystemWaitsForTheGameViewportTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video;
	FPlaytestFixture Fixture;
	const TSharedRef<FVideoSourceLog> Log = UseTestVideoFrames(Fixture);
	Log->bViewportExists = false;
	FPlaytestLogCapture Capture;
	StartWithVideo(Fixture, true);

	TestFalse(TEXT("No viewport yet"), Fixture.Playtest->IsRecordingVideo());
	TestTrue(TEXT("Precondition: it asked for one"), Log->SourcesAskedFor > 0);
	TestTrue(TEXT("The video ticker runs to ask again"), Fixture.Playtest->IsVideoTickerRunningForTesting());
	TestFalse(TEXT("A test video is refused while the playtest records video"), Fixture.Playtest->StartTestVideoRecording(2.0));
	const int32 AskedAtStart = Log->SourcesAskedFor;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Fixture.Playtest->TickVideoRecordingForTesting(1.f / 60.f);
	}
	TestFalse(TEXT("Still none after three more frames"), Fixture.Playtest->IsRecordingVideo());
	TestEqual(TEXT("Asked once on each frame"), Log->SourcesAskedFor - AskedAtStart, 3);

	Log->bViewportExists = true;
	Fixture.Playtest->TickVideoRecordingForTesting(1.f / 60.f);
	TestTrue(TEXT("Recording starts on the frame the viewport is there"), Fixture.Playtest->IsRecordingVideo());
	TestEqual(TEXT("One frame source"), Log->SourcesCreated, 1);
	TestEqual(TEXT("Nothing was warned"), Capture.LinesContaining(TEXT("No video is recorded")).Num(), 0);
	return true;
}

// The setting exists only where editor-only data does; a game build compiles these tests without it.
#if WITH_EDITORONLY_DATA

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoSubsystemPlayInEditorSettingTest,
	"Protokite.Playtest.Video.Subsystem.PlayInEditorSettingRecordsOnlyInPlayInEditor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoSubsystemPlayInEditorSettingTest::RunTest(const FString& Parameters)
{
	UProtokitePlaytestLocalSettings* LocalSettings = GetMutableDefault<UProtokitePlaytestLocalSettings>();
	const bool bSavedRecordInPlayInEditor = LocalSettings->bRecordVideoInPlayInEditor;
	LocalSettings->bRecordVideoInPlayInEditor = false;
	{
		// Playtesting is off: the setting needs no playtest.
		FScopedPlaytestSettings Settings(false, UsableUrl);
		FScopedVideoSettings Video;
		FPlaytestFixture Fixture;
		const TSharedRef<FVideoSourceLog> Log = UseTestVideoFrames(Fixture);
		const FScopedTestWorld TestWorld(TEXT("PlayInEditorTest"), Fixture.GameInstance);
		FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::PIE);
		Context.OwningGameInstance = Fixture.GameInstance;
		Context.SetCurrentWorld(TestWorld.World);

		Fixture.StartFlock();
		TestFalse(TEXT("Playing in the editor with the setting off records nothing"), Fixture.Playtest->IsRecordingVideo());

		LocalSettings->bRecordVideoInPlayInEditor = true;
		Context.WorldType = EWorldType::Game;
		Fixture.Flock->ShutdownSdk();
		TestFalse(TEXT("A game world is not playing in the editor"), Fixture.Playtest->IsRecordingVideo());
		TestEqual(TEXT("No frame source"), Log->SourcesAskedFor, 0);

		Context.WorldType = EWorldType::PIE;
		Fixture.StartFlock();
		TestTrue(TEXT("Playing in the editor with the setting on records"), Fixture.Playtest->IsRecordingVideo());

		PlayVideoFrames(Fixture, 1.0);
		Fixture.Playtest->StopVideoRecording();
		TestTrue(TEXT("Named as a test recording"),
			FPaths::GetCleanFilename(WaitForTheFile(Fixture)).StartsWith(TEXT("test-recording-")));

		LocalSettings->bRecordVideoInPlayInEditor = false;
		Fixture.Playtest->Deinitialize();
		GEngine->DestroyWorldContext(TestWorld.World);
	}
	LocalSettings->bRecordVideoInPlayInEditor = bSavedRecordInPlayInEditor;
	return true;
}

#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoSubsystemTestRecordingStopsAfterItsSecondsTest,
	"Protokite.Playtest.Video.Subsystem.TestRecordingStopsAfterItsSeconds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoSubsystemTestRecordingStopsAfterItsSecondsTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(false, UsableUrl);
	FScopedVideoSettings Video;
	FPlaytestFixture Fixture;
	const TSharedRef<FVideoSourceLog> Log = UseTestVideoFrames(Fixture);
	Fixture.StartFlock();

	TestFalse(TEXT("No length is refused"), Fixture.Playtest->StartTestVideoRecording(0.0));
	TestTrue(TEXT("A test video starts with playtesting off"), Fixture.Playtest->StartTestVideoRecording(2.0));
	TestTrue(TEXT("Recording"), Fixture.Playtest->IsRecordingVideo());
	PlayVideoFrames(Fixture, 3.0);
	TestFalse(TEXT("It stopped after two seconds"), Fixture.Playtest->IsRecordingVideo());

	const FString Path = WaitForTheFile(Fixture);
	TestTrue(TEXT("Named as a test recording"), FPaths::GetCleanFilename(Path).StartsWith(TEXT("test-recording-")));
	TestEqual(TEXT("In a run's folder under TestVideos"), FPaths::GetCleanFilename(FPaths::GetPath(FPaths::GetPath(Path))),
		FString(FProtokitePlaytestRecordingsFolder::TestVideoFolderName));
	FVideoFileRead File;
	if (TestTrue(TEXT("The file reads back"), ReadVideoFile(Path, File)))
	{
		TestEqual(TEXT("Two seconds of frames"), File.Frames.Num(), 60);
		TestTrue(TEXT("None at or after two seconds"), File.Frames.Num() > 0 && File.Frames.Last().TimestampMs < 2000);
	}
	TestFalse(TEXT("A second test video is refused this launch"), Fixture.Playtest->StartTestVideoRecording(2.0));
	TestEqual(TEXT("One frame source"), Log->SourcesCreated, 1);
	return true;
}

namespace
{
	const TCHAR* const VideoRecordingsSecondGameVersionId = TEXT("pt-test-version-2");

	/** The session saved in a run's folder, read the way a later launch reads it, and the file's text. */
	FProtokitePlaytestRecordingSession ReadSavedRecordingSession(const FString& RunFolder, FString& OutText)
	{
		FProtokitePlaytestRecordingSession Session;
		const FString Path = FPaths::Combine(RunFolder, FProtokitePlaytestRecordingsFolder::SessionFileName);
		if (!IFileManager::Get().FileExists(*Path) || !FFileHelper::LoadFileToString(OutText, *Path))
		{
			return Session;
		}
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutText);
		if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid())
		{
			Object->TryGetStringField(TEXT("protokite_session_id"), Session.ProtokiteSessionId);
			Object->TryGetStringField(TEXT("protokite_api_url"), Session.ProtokiteApiUrl);
			Object->TryGetStringField(TEXT("flock_game_version_id"), Session.FlockGameVersionId);
		}
		return Session;
	}

	/** How many run folders are under the recordings folder for Kind. */
	int32 CountRecordingRunFolders(const FString& Recordings, EProtokitePlaytestRecordingKind Kind)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *FPaths::Combine(FProtokitePlaytestRecordingsFolder::GetKindFolder(Recordings, Kind), TEXT("*")),
			/*Files*/ false, /*Directories*/ true);
		return Names.Num();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoRecordingsSavesTheSessionTest,
	"Protokite.Playtest.Video.Recordings.SavesTheSessionBesideThePlaytestRecording",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoRecordingsSavesTheSessionTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video;
	{
		// The recording starts first, and the Protokite session after it.
		FPlaytestFixture Fixture;
		UseTestVideoFrames(Fixture);
		StartWithVideo(Fixture, true);
		PlayVideoFrames(Fixture, 0.5);
		const TArray<FString> Files = RecordingFilesIn(RecordingsFolder(Fixture));
		if (!TestEqual(TEXT("Precondition: one recording"), Files.Num(), 1))
		{
			return true;
		}
		const FString RunFolder = FPaths::GetPath(Files[0]);
		FString Text;
		TestTrue(TEXT("No session is saved before one starts"), ReadSavedRecordingSession(RunFolder, Text).IsEmpty());

		Fixture.RegisterFlockSession(FirstFlockSessionId);
		TestEqual(TEXT("Precondition: the Protokite session started"), Fixture.SessionStarts(), 1);
		const FProtokitePlaytestRecordingSession Saved = ReadSavedRecordingSession(RunFolder, Text);
		TestEqual(TEXT("The session the recording belongs to is saved beside it"), Saved.ProtokiteSessionId, FString(PlaytestSessionId));
		TestEqual(TEXT("With the address the session started at"), Saved.ProtokiteApiUrl, FString(UsableUrl));
		TestEqual(TEXT("And the Game Version ID it started with"), Saved.FlockGameVersionId, FString(GameVersionId));
		TestTrue(TEXT("Precondition: the requests carry the API key"),
			Fixture.Flock->GetRequestHeaders().FindRef(TEXT("X-Flock-API-Key")).Equals(TEXT("secret"), ESearchCase::CaseSensitive));
		TestFalse(TEXT("The API key is never saved"), Text.Contains(TEXT("secret"), ESearchCase::CaseSensitive));
	}
	{
		// The Protokite session starts while the recording waits for the game viewport, the Flock SDK is initialized again with
		// another Game Version ID, and only then does the recording start.
		FPlaytestFixture Fixture;
		const TSharedRef<FVideoSourceLog> Log = UseTestVideoFrames(Fixture);
		Log->bViewportExists = false;
		StartWithVideo(Fixture, true);
		Fixture.RegisterFlockSession(FirstFlockSessionId);
		TestEqual(TEXT("Precondition: the Protokite session started"), Fixture.SessionStarts(), 1);
		Fixture.Flock->ShutdownSdk();
		Fixture.AnswerConfig(FProtokitePlaytestFakeTransport::Status(200, ConfigBody(VideoRecordingsSecondGameVersionId, false, true)));
		Fixture.Flock->InitializeWithConfig(MakeFlockConfig(VideoRecordingsSecondGameVersionId));
		ExpectPlaytestStatus(*this, TEXT("Ready with the other version"), Fixture.Playtest->GetStatus(), EProtokitePlaytestStatus::Ready);
		Log->bViewportExists = true;
		Fixture.Playtest->TickVideoRecordingForTesting(1.f / 60.f);
		TestTrue(TEXT("Precondition: recording"), Fixture.Playtest->IsRecordingVideo());
		const TArray<FString> Files = RecordingFilesIn(RecordingsFolder(Fixture));
		if (TestEqual(TEXT("Precondition: one recording"), Files.Num(), 1))
		{
			FString Text;
			const FProtokitePlaytestRecordingSession Saved = ReadSavedRecordingSession(FPaths::GetPath(Files[0]), Text);
			TestEqual(TEXT("A session that started first is saved when the recording starts"), Saved.ProtokiteSessionId, FString(PlaytestSessionId));
			TestEqual(TEXT("With the Game Version ID the session started with, not the one the Flock SDK has now"), Saved.FlockGameVersionId, FString(GameVersionId));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoRecordingsKeptForTheNextLaunchTest,
	"Protokite.Playtest.Video.Recordings.KeptForALaterLaunchToUpload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoRecordingsKeptForTheNextLaunchTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video;
	FPlaytestFixture Earlier;
	UseTestVideoFrames(Earlier);
	StartWithVideo(Earlier, true);
	Earlier.RegisterFlockSession(FirstFlockSessionId);
	PlayVideoFrames(Earlier, 1.0);
	// The game instance shuts down while still recording, before anything uploaded the file.
	Earlier.Playtest->Deinitialize();
	const FString EarlierVideo = Earlier.Playtest->GetFinishedVideoRecordingPath();
	if (!TestFalse(TEXT("Precondition: the earlier launch saved its recording"), EarlierVideo.IsEmpty()))
	{
		return true;
	}

	// The next launch runs a build with another Game Version ID, and records a video of its own.
	FPlaytestFixture Next;
	UseTestVideoFrames(Next);
	Next.Playtest->SetVideoRecordingFolderForTesting(RecordingsFolder(Earlier));
	FPlaytestLogCapture Capture;
	Next.AnswerConfig(FProtokitePlaytestFakeTransport::Status(200, ConfigBody(VideoRecordingsSecondGameVersionId, false, true)));
	Next.Playtest->FollowFlockLifecycleForTesting(Next.Flock);
	Next.Playtest->WaitUntilRecordingsFolderFinishedForTesting();
	Next.Flock->InitializeWithConfig(MakeFlockConfig(VideoRecordingsSecondGameVersionId));

	TestTrue(TEXT("The earlier recording is kept"), IFileManager::Get().FileExists(*EarlierVideo));
	const TArray<FProtokitePlaytestRecordingWaitingToUpload> Waiting = FProtokitePlaytestRecordingsFolder::FindRecordingsWaitingToUpload(RecordingsFolder(Earlier));
	if (TestEqual(TEXT("It is the one recording waiting to be uploaded"), Waiting.Num(), 1))
	{
		TestTrue(TEXT("Its file"), FPaths::IsSamePath(Waiting[0].VideoFilePath, EarlierVideo));
		TestEqual(TEXT("To the earlier launch's session"), Waiting[0].Session.ProtokiteSessionId, FString(PlaytestSessionId));
		TestEqual(TEXT("Under the Game Version ID that session started with"), Waiting[0].Session.FlockGameVersionId, FString(GameVersionId));
	}
	TestEqual(TEXT("The launch says so, once"), Capture.LinesContaining(TEXT("waiting to be uploaded")).Num(), 1);
	TestEqual(TEXT("Nothing ends the earlier session"), Next.SessionEnds(), 0);
	TestEqual(TEXT("Nothing but the playtest config is asked of Protokite"), Next.Transport->Requests.Num(), Next.ConfigRequests());

	TestTrue(TEXT("The next launch records a video of its own"), Next.Playtest->IsRecordingVideo());
	PlayVideoFrames(Next, 0.5);
	Next.Playtest->StopVideoRecording();
	const FString NextVideo = WaitForTheFile(Next);
	TestTrue(TEXT("In a file of its own"), !NextVideo.IsEmpty() && !FPaths::IsSamePath(NextVideo, EarlierVideo));
	TestTrue(TEXT("And the earlier one is still there"), IFileManager::Get().FileExists(*EarlierVideo));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoRecordingsKeepsWhatItsLaunchOwnsTest,
	"Protokite.Playtest.Video.Recordings.NothingTouchesARecordingItsLaunchStillOwns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoRecordingsKeepsWhatItsLaunchOwnsTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video;
	FPlaytestFixture Owner;
	UseTestVideoFrames(Owner);
	StartWithVideo(Owner, true);
	PlayVideoFrames(Owner, 1.0);
	Owner.Playtest->StopVideoRecording();
	const FString Path = WaitForTheFile(Owner);
	if (!TestFalse(TEXT("Precondition: the recording is saved"), Path.IsEmpty()))
	{
		return true;
	}
	// No Protokite session started for it, so once its launch has ended a later launch deletes it.

	Owner.Flock->ShutdownSdk();
	Owner.AnswerConfig(FProtokitePlaytestFakeTransport::Status(200, ConfigBody(VideoRecordingsSecondGameVersionId, false, true)));
	Owner.Flock->InitializeWithConfig(MakeFlockConfig(VideoRecordingsSecondGameVersionId));
	ExpectPlaytestStatus(*this, TEXT("Ready with the other version"), Owner.Playtest->GetStatus(), EProtokitePlaytestStatus::Ready);
	TestTrue(TEXT("A Game Version ID change during the launch deletes nothing"), IFileManager::Get().FileExists(*Path));

	{
		// Another launch starts while this one still runs.
		FPlaytestFixture Another;
		Another.Playtest->SetVideoRecordingFolderForTesting(RecordingsFolder(Owner));
		Another.StartFlock();
		TestTrue(TEXT("Another launch leaves it alone while its own launch runs"), IFileManager::Get().FileExists(*Path));
		TestTrue(TEXT("With its folder"), IFileManager::Get().DirectoryExists(*FPaths::GetPath(Path)));
	}

	Owner.Playtest->Deinitialize();
	TestTrue(TEXT("Its own launch ending deletes nothing"), IFileManager::Get().FileExists(*Path));
	{
		FPlaytestFixture Later;
		Later.Playtest->SetVideoRecordingFolderForTesting(RecordingsFolder(Owner));
		Later.StartFlock();
		TestFalse(TEXT("Control: once its launch has ended, a later launch deletes it, since no session started for it"),
			IFileManager::Get().FileExists(*Path));
		TestFalse(TEXT("Control: with its folder"), IFileManager::Get().DirectoryExists(*FPaths::GetPath(Path)));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoRecordingsKeepsTestVideosTest,
	"Protokite.Playtest.Video.Recordings.ATestVideoIsKeptByLaterLaunches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoRecordingsKeepsTestVideosTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(false, UsableUrl);
	FScopedVideoSettings Video;
	FPlaytestFixture Earlier;
	UseTestVideoFrames(Earlier);
	Earlier.StartFlock();
	TestTrue(TEXT("Precondition: a test video starts"), Earlier.Playtest->StartTestVideoRecording(1.0));
	PlayVideoFrames(Earlier, 2.0);
	const FString Path = WaitForTheFile(Earlier);
	if (!TestFalse(TEXT("Precondition: it is saved"), Path.IsEmpty()))
	{
		return true;
	}
	Earlier.Playtest->Deinitialize();

	FPlaytestFixture Next;
	Next.Playtest->SetVideoRecordingFolderForTesting(RecordingsFolder(Earlier));
	Next.StartFlock();
	TestTrue(TEXT("A test video, which no session is saved for, is kept by the next launch"), IFileManager::Get().FileExists(*Path));
	TestEqual(TEXT("And is never listed for upload"), FProtokitePlaytestRecordingsFolder::FindRecordingsWaitingToUpload(RecordingsFolder(Earlier)).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoRecordingsCutToFitTheBudgetTest,
	"Protokite.Playtest.Video.Recordings.ARecordingIsCutToFitTheDiskBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoRecordingsCutToFitTheBudgetTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video(FIntPoint(320, 180), /*MaxMinutes*/ 60.f, /*MaxSizeMb*/ 1536, /*BitrateKbps*/ 20000, /*DiskBudgetMb*/ 1);
	FPlaytestFixture Fixture;
	FString OldTestVideo;
	{
		// A test video an ended launch kept.
		FString Error;
		const TSharedPtr<FProtokitePlaytestRecordingRun> Old = FProtokitePlaytestRecordingRun::CreateNamedForTesting(RecordingsFolder(Fixture),
			EProtokitePlaytestRecordingKind::TestVideo, TEXT("20200101-000000-00000000"), /*ReservedBytes*/ 0, Error);
		if (!TestTrue(TEXT("Precondition: the old test video's run is made"), Old.IsValid()))
		{
			return true;
		}
		TArray<uint8> Bytes;
		Bytes.SetNumZeroed(300 * 1024);
		OldTestVideo = Old->GetVideoFilePath();
		TestTrue(TEXT("Precondition: the old test video is saved"), FFileHelper::SaveArrayToFile(Bytes, *OldTestVideo));
	}
	FString OlderPlaytestRecording;
	{
		// An even older launch's playtest recording, still waiting to be uploaded.
		FString Error;
		const TSharedPtr<FProtokitePlaytestRecordingRun> Older = FProtokitePlaytestRecordingRun::CreateNamedForTesting(RecordingsFolder(Fixture),
			EProtokitePlaytestRecordingKind::Playtest, TEXT("20190101-000000-00000000"), /*ReservedBytes*/ 0, Error);
		if (!TestTrue(TEXT("Precondition: the older playtest recording's run is made"), Older.IsValid()))
		{
			return true;
		}
		TArray<uint8> Bytes;
		Bytes.SetNumZeroed(300 * 1024);
		OlderPlaytestRecording = Older->GetVideoFilePath();
		TestTrue(TEXT("Precondition: the older playtest recording is saved"), FFileHelper::SaveArrayToFile(Bytes, *OlderPlaytestRecording));
		FProtokitePlaytestRecordingSession Session;
		Session.ProtokiteSessionId = TEXT("01KX0OLDERSESSION000000001");
		Session.ProtokiteApiUrl = UsableUrl;
		Session.FlockGameVersionId = GameVersionId;
		TestTrue(TEXT("Precondition: its session is saved"), Older->SaveSession(Session, Error));
	}
	const TSharedRef<FVideoSourceLog> Log = UseTestVideoFrames(Fixture, /*bNoisyPictures*/ true);
	FPlaytestLogCapture Capture;
	StartWithVideo(Fixture, true);

	TestTrue(TEXT("Recording starts"), Fixture.Playtest->IsRecordingVideo());
	const TArray<FString> Recording = RecordingFilesIn(RecordingsFolder(Fixture));
	if (TestEqual(TEXT("Precondition: only the new recording is left"), Recording.Num(), 1))
	{
		FString Reserved;
		FFileHelper::LoadFileToString(Reserved, *FPaths::Combine(FPaths::GetPath(Recording[0]), FProtokitePlaytestRecordingsFolder::ReservedBytesFileName));
		TestEqual(TEXT("Its folder says it may grow to the megabyte the budget has left"), Reserved, LexToString(1024 * 1024));
	}
	TestFalse(TEXT("The old test video is deleted to make room"), IFileManager::Get().FileExists(*OldTestVideo));
	TestEqual(TEXT("And that is logged once"), Capture.LinesContaining(TEXT("Deleted the test video")).Num(), 1);
	TestFalse(TEXT("So is the older playtest recording, first"), IFileManager::Get().FileExists(*OlderPlaytestRecording));
	const TArray<FPlaytestLogCapture::FLine> DeletedRecording = Capture.LinesContaining(TEXT("the oldest playtest recording not yet uploaded"));
	if (TestEqual(TEXT("Which is logged once"), DeletedRecording.Num(), 1))
	{
		TestEqual(TEXT("As a warning, since that video is lost"), static_cast<int32>(DeletedRecording[0].Verbosity), static_cast<int32>(ELogVerbosity::Warning));
	}
	const TArray<FPlaytestLogCapture::FLine> Started = Capture.LinesContaining(TEXT("Recording video for the playtest"));
	if (TestEqual(TEXT("The start is logged once"), Started.Num(), 1))
	{
		TestTrue(TEXT("Saying the budget is what cut it shorter"), Started[0].Message.Contains(TEXT("Recordings Disk Budget (MB) has only this much left")));
	}

	for (int32 Index = 0; Index < 1800 && Fixture.Playtest->IsRecordingVideo(); ++Index)
	{
		PlayVideoFrames(Fixture, 1.0 / 30.0, 1.0 / 30.0);
	}
	TestFalse(TEXT("Recording stopped"), Fixture.Playtest->IsRecordingVideo());
	FVideoFileRead File;
	if (TestTrue(TEXT("The file reads back"), ReadVideoFile(WaitForTheFile(Fixture), File)))
	{
		TestTrue(FString::Printf(TEXT("The file, %lld bytes, stays within the one megabyte budget"), File.FileBytes), File.FileBytes <= 1024 * 1024);
		TestTrue(TEXT("And came close to it"), File.FileBytes > 512 * 1024);
	}
	TestEqual(TEXT("It stopped at its size limit"), Capture.LinesContaining(TEXT("it reached its size limit")).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoRecordingsNoRoomRecordsNothingTest,
	"Protokite.Playtest.Video.Recordings.NoRoomInTheDiskBudgetRecordsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoRecordingsNoRoomRecordsNothingTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video(FIntPoint(64, 36), /*MaxMinutes*/ 60.f, /*MaxSizeMb*/ 1536, /*BitrateKbps*/ 2000, /*DiskBudgetMb*/ 1);
	FPlaytestFixture Fixture;
	// Another game, still running, has only just started a recording that may grow to the whole budget.
	FString Error;
	const TSharedPtr<FProtokitePlaytestRecordingRun> StillRunning = FProtokitePlaytestRecordingRun::Create(RecordingsFolder(Fixture),
		EProtokitePlaytestRecordingKind::TestVideo, /*ReservedBytes*/ 1024 * 1024, Error);
	if (!TestTrue(TEXT("Precondition: the running game's run is made"), StillRunning.IsValid()))
	{
		return true;
	}
	const TSharedRef<FVideoSourceLog> Log = UseTestVideoFrames(Fixture);
	FPlaytestLogCapture Capture;
	StartWithVideo(Fixture, true);
	PlayVideoFrames(Fixture, 1.0);

	TestFalse(TEXT("Nothing records"), Fixture.Playtest->IsRecordingVideo());
	const TArray<FPlaytestLogCapture::FLine> Lines = Capture.LinesContaining(TEXT("Recordings Disk Budget (MB)"));
	if (TestEqual(TEXT("Warned once"), Lines.Num(), 1))
	{
		TestEqual(TEXT("As a warning"), static_cast<int32>(Lines[0].Verbosity), static_cast<int32>(ELogVerbosity::Warning));
		TestTrue(TEXT("Saying no video is recorded"), Lines[0].Message.Contains(TEXT("No video is recorded this launch")));
	}
	TestEqual(TEXT("No run folder is made for it"), CountRecordingRunFolders(RecordingsFolder(Fixture), EProtokitePlaytestRecordingKind::Playtest), 0);
	TestTrue(TEXT("The running game's run is untouched"), IFileManager::Get().DirectoryExists(*StillRunning->GetFolder()));
	TestFalse(TEXT("The video ticker does not run"), Fixture.Playtest->IsVideoTickerRunningForTesting());
	TestEqual(TEXT("Tried once"), Log->SourcesCreated, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoRecordingKeepsWhatWasWrittenWhenAWriteFailsTest,
	"Protokite.Playtest.Video.Recording.KeepsWhatWasWrittenWhenAWriteFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoRecordingKeepsWhatWasWrittenWhenAWriteFailsTest::RunTest(const FString& Parameters)
{
	const FString Folder = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("ProtokitePlaytestTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(Folder, TEXT("disk-full.webm")));
	FString Error;
	{
		const TSharedRef<FTestVideoFrameSource> Source = MakeShared<FTestVideoFrameSource>(FIntPoint(64, 36));
		// The disk takes ten frames and refuses the eleventh, the way a full disk does. Only the file thread counts.
		const TSharedRef<int32> WritesTried = MakeShared<int32>(0);
		const TSharedPtr<FProtokitePlaytestVideoRecording> Recording = FProtokitePlaytestVideoRecording::Start(Source, FProtokitePlaytestVideoSettings(), Path, Error,
			nullptr, [WritesTried]() { return ++*WritesTried <= 10; });
		if (!TestTrue(TEXT("The recording starts"), Recording.IsValid()))
		{
			AddError(Error);
			return true;
		}

		TOptional<EProtokitePlaytestVideoStopReason> StopReason;
		for (int32 Index = 0; Index < 60 && !StopReason.IsSet(); ++Index)
		{
			StopReason = Recording->AddFrame(1.0 / 30.0);
			Recording->WaitUntilWritten();
		}
		TestTrue(TEXT("The recording asks to stop because its file could not be written"),
			StopReason.IsSet() && StopReason.GetValue() == EProtokitePlaytestVideoStopReason::CouldNotWrite);
		Recording->StopCapturing(StopReason.Get(EProtokitePlaytestVideoStopReason::StoppedByGame));
		Recording->WaitUntilWritten();

		const FProtokitePlaytestVideoRecordingSummary Summary = Recording->GetSummary();
		TestFalse(TEXT("The failure is reported"), Summary.Error.IsEmpty());
		TestTrue(FString::Printf(TEXT("The file is kept all the same (%s)"), *Summary.FilePath), FPaths::IsSamePath(Summary.FilePath, Path));
		TestEqual(TEXT("With the ten frames written before"), Summary.FramesWritten, 10);
		TestFalse(TEXT("No unfinished file is left"), IFileManager::Get().FileExists(*(Path + TEXT(".part"))));
		FVideoFileRead File;
		if (TestTrue(TEXT("It reads back"), ReadVideoFile(Path, File)))
		{
			TestEqual(TEXT("Ten frames"), File.Frames.Num(), 10);
			TestTrue(TEXT("The finished file states its segment's size"), File.bSegmentSizeWritten);
			TestEqual(TEXT("The summary counts its bytes"), Summary.BytesWritten, File.FileBytes);
			FIntPoint PictureSize;
			double Brightness = -1.0;
			TestEqual(TEXT("Every frame decodes"), DecodeVideoFile(File, PictureSize, Brightness), 10);
		}
	}
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoRecordingsLaunchDoesNotWaitForTheFolderTest,
	"Protokite.Playtest.Video.Recordings.GoingThroughEarlierRecordingsDoesNotHoldUpTheLaunch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoRecordingsLaunchDoesNotWaitForTheFolderTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(false, UsableUrl);
	FPlaytestFixture Fixture;
	// Stands in for an hour's cut-off recording: going through the folder takes ten seconds unless the test lets it go.
	FEvent* Gate = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ true);
	Fixture.Playtest->SetBeforeFinishingWhatEndedRunsLeftForTesting([Gate]() { Gate->Wait(FTimespan::FromSeconds(10.0)); });

	const double StartSeconds = FPlatformTime::Seconds();
	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	const double FollowingSeconds = FPlatformTime::Seconds() - StartSeconds;
	TestTrue(FString::Printf(TEXT("Following starts at once, while earlier recordings are still being gone through (%.2f s)"), FollowingSeconds),
		FollowingSeconds < 5.0);

	Gate->Trigger();
	Fixture.Playtest->WaitUntilRecordingsFolderFinishedForTesting();
	FPlatformProcess::ReturnSynchEventToPool(Gate);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoRecordingsTestVideoDeletesNoWaitingUploadTest,
	"Protokite.Playtest.Video.Recordings.AShortTestVideoDeletesNoRecordingWaitingToUpload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoRecordingsTestVideoDeletesNoWaitingUploadTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(false, UsableUrl);
	FScopedVideoSettings Video(FIntPoint(64, 36), /*MaxMinutes*/ 60.f, /*MaxSizeMb*/ 1536, /*BitrateKbps*/ 2000, /*DiskBudgetMb*/ 9);
	FPlaytestFixture Fixture;
	FString WaitingRecording;
	{
		// An earlier launch's playtest recording, waiting to be uploaded, filling most of the budget.
		FString Error;
		const TSharedPtr<FProtokitePlaytestRecordingRun> Earlier = FProtokitePlaytestRecordingRun::CreateNamedForTesting(RecordingsFolder(Fixture),
			EProtokitePlaytestRecordingKind::Playtest, TEXT("20190101-000000-00000000"), /*ReservedBytes*/ 0, Error);
		if (!TestTrue(TEXT("Precondition: the earlier recording's run is made"), Earlier.IsValid()))
		{
			return true;
		}
		TArray<uint8> Bytes;
		Bytes.SetNumZeroed(7 * 1024 * 1024);
		WaitingRecording = Earlier->GetVideoFilePath();
		TestTrue(TEXT("Precondition: the earlier recording is saved"), FFileHelper::SaveArrayToFile(Bytes, *WaitingRecording));
		FProtokitePlaytestRecordingSession Session;
		Session.ProtokiteSessionId = TEXT("01KX0OLDERSESSION000000001");
		Session.ProtokiteApiUrl = UsableUrl;
		Session.FlockGameVersionId = GameVersionId;
		TestTrue(TEXT("Precondition: its session is saved"), Earlier->SaveSession(Session, Error));
	}
	UseTestVideoFrames(Fixture);
	FPlaytestLogCapture Capture;
	Fixture.StartFlock();

	TestTrue(TEXT("A two-second test video starts"), Fixture.Playtest->StartTestVideoRecording(2.0));
	TestTrue(TEXT("The recording waiting to be uploaded is kept: two seconds need far less than the size limit"),
		IFileManager::Get().FileExists(*WaitingRecording));
	TestEqual(TEXT("And nothing says it was deleted"), Capture.LinesContaining(TEXT("the oldest playtest recording not yet uploaded")).Num(), 0);
	PlayVideoFrames(Fixture, 3.0);
	TestFalse(TEXT("The test video is saved"), WaitForTheFile(Fixture).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoRecordingsKeepsWhatWasWrittenWhenAWriteFailsTest,
	"Protokite.Playtest.Video.Recordings.TheLaunchKeepsWhatWasWrittenWhenAWriteFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoRecordingsKeepsWhatWasWrittenWhenAWriteFailsTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video;
	FPlaytestFixture Fixture;
	UseTestVideoFrames(Fixture);
	// The disk takes ten frames and refuses the eleventh. Only the writing thread counts.
	const TSharedRef<int32> WritesTried = MakeShared<int32>(0);
	Fixture.Playtest->SetBeforeEachVideoWriteForTesting([WritesTried]() { return ++*WritesTried <= 10; });
	FPlaytestLogCapture Capture;
	StartWithVideo(Fixture, true);

	for (int32 Index = 0; Index < 120 && Fixture.Playtest->IsRecordingVideo(); ++Index)
	{
		PlayVideoFrames(Fixture, 1.0 / 30.0, 1.0 / 30.0);
	}
	TestFalse(TEXT("Recording stopped"), Fixture.Playtest->IsRecordingVideo());
	const FString Path = WaitForTheFile(Fixture);
	TestFalse(TEXT("The frames written before are kept as the launch's recording"), Path.IsEmpty());
	const TArray<FPlaytestLogCapture::FLine> Lines = Capture.LinesContaining(TEXT("could not be written to the end"));
	if (TestEqual(TEXT("Warned once"), Lines.Num(), 1))
	{
		TestEqual(TEXT("As a warning"), static_cast<int32>(Lines[0].Verbosity), static_cast<int32>(ELogVerbosity::Warning));
	}
	FVideoFileRead File;
	if (TestTrue(TEXT("The kept file reads back"), ReadVideoFile(Path, File)))
	{
		TestEqual(TEXT("With the ten frames written"), File.Frames.Num(), 10);
	}
	return true;
}

#endif
