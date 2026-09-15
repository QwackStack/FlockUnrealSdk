// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS && WITH_FLOCK_PLAYTEST_VIDEO

#include "Engine/Engine.h"
#include "FlockPlaytestLibrary.h"
#include "FlockPlaytestLocalSettings.h"
#include "FlockPlaytestVideoFile.h"
#include "FlockPlaytestVideoRecording.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Tests/FlockPlaytestSubsystemTestSupport.h"
#include "Tests/FlockPlaytestTestSupport.h"
#include "Tests/FlockPlaytestVideoTestSupport.h"

using namespace FlockPlaytestSubsystemTesting;
using namespace FlockPlaytestFixtures;
using namespace FlockPlaytestVideoTesting;

namespace
{
	/** Sets the project's Video Recording settings for one test and puts the previous values back when it ends. */
	struct FScopedVideoSettings
	{
		UFlockPlaytestSettings* Settings = GetMutableDefault<UFlockPlaytestSettings>();
		int32 SavedWidth = Settings->VideoWidth;
		int32 SavedHeight = Settings->VideoHeight;
		int32 SavedFramesPerSecond = Settings->VideoFramesPerSecond;
		int32 SavedBitrateKbps = Settings->VideoBitrateKbps;
		float SavedMaxMinutes = Settings->MaxRecordingMinutes;
		int32 SavedMaxSizeMb = Settings->MaxRecordingSizeMb;

		explicit FScopedVideoSettings(FIntPoint VideoSize = FIntPoint(64, 36), float MaxMinutes = 60.f, int32 MaxSizeMb = 1536, int32 BitrateKbps = 2000)
		{
			Settings->VideoWidth = VideoSize.X;
			Settings->VideoHeight = VideoSize.Y;
			Settings->VideoFramesPerSecond = 30;
			Settings->VideoBitrateKbps = BitrateKbps;
			Settings->MaxRecordingMinutes = MaxMinutes;
			Settings->MaxRecordingSizeMb = MaxSizeMb;
		}

		~FScopedVideoSettings()
		{
			Settings->VideoWidth = SavedWidth;
			Settings->VideoHeight = SavedHeight;
			Settings->VideoFramesPerSecond = SavedFramesPerSecond;
			Settings->VideoBitrateKbps = SavedBitrateKbps;
			Settings->MaxRecordingMinutes = SavedMaxMinutes;
			Settings->MaxRecordingSizeMb = SavedMaxSizeMb;
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
			-> TSharedPtr<IFlockPlaytestVideoFrameSource>
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
		Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200, ConfigBody(GameVersionId, /*bHeavyAnalytics*/ false, bVideoRecording)));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoSubsystemVideoOffCreatesNothingTest,
	"Flock.Playtest.Video.Subsystem.VideoOffCreatesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoSubsystemVideoOffCreatesNothingTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video;
	FPlaytestFixture Fixture;
	const TSharedRef<FVideoSourceLog> Log = UseTestVideoFrames(Fixture);
	StartWithVideo(Fixture, /*bVideoRecording*/ false);
	ExpectPlaytestStatus(*this, TEXT("Ready"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);

	PlayVideoFrames(Fixture, 2.0);
	TestEqual(TEXT("No frame source is asked for"), Log->SourcesAskedFor, 0);
	TestFalse(TEXT("Nothing records"), Fixture.Playtest->IsRecordingVideo());
	TestFalse(TEXT("The video ticker does not run"), Fixture.Playtest->IsVideoTickerRunningForTesting());
	TestEqual(TEXT("No file is written"), FilesIn(RecordingsFolder(Fixture)).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoSubsystemRecordsWhenThePlaytestTurnsVideoOnTest,
	"Flock.Playtest.Video.Subsystem.RecordsWhenThePlaytestTurnsVideoOn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoSubsystemRecordsWhenThePlaytestTurnsVideoOnTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video(FIntPoint(64, 64));
	FPlaytestFixture Fixture;
	const TSharedRef<FVideoSourceLog> Log = UseTestVideoFrames(Fixture);
	FPlaytestLogCapture Capture;
	StartWithVideo(Fixture, /*bVideoRecording*/ true);

	TestTrue(TEXT("Recording starts once the config is loaded, before any Flock session"), Fixture.Playtest->IsRecordingVideo());
	TestFalse(TEXT("The Blueprint stop node answers false with no playtest subsystem"), UFlockPlaytestLibrary::StopVideoRecording(nullptr));
	TestFalse(TEXT("The Blueprint recording check answers false with no playtest subsystem"), UFlockPlaytestLibrary::IsRecordingVideo(nullptr));
	TestEqual(TEXT("No Protokite session was needed"), Fixture.SessionStarts(), 0);
	TestEqual(TEXT("One frame source"), Log->SourcesCreated, 1);
	if (const TSharedPtr<FTestVideoFrameSource> Source = Log->LastSource.Pin())
	{
		TestEqual(TEXT("The screen's shape fitted inside Video Width by Video Height"), Source->FrameSize, FIntPoint(64, 36));
	}

	PlayVideoFrames(Fixture, 1.0);
	const TArray<FString> WhileRecording = FilesIn(RecordingsFolder(Fixture));
	if (TestEqual(TEXT("One file while recording"), WhileRecording.Num(), 1))
	{
		TestTrue(TEXT("Named as unfinished"), WhileRecording[0].EndsWith(TEXT(".ivf.part")));
	}

	TestTrue(TEXT("The game stops it"), Fixture.Playtest->StopVideoRecording());
	TestFalse(TEXT("A second stop changes nothing"), Fixture.Playtest->StopVideoRecording());
	const FString Path = WaitForTheFile(Fixture);
	TestTrue(TEXT("The file is saved under its final name"), Path.EndsWith(TEXT(".ivf")));
	TestTrue(TEXT("Named as the playtest's recording"), FPaths::GetCleanFilename(Path).StartsWith(TEXT("recording-")));
	const TArray<FString> Afterwards = FilesIn(RecordingsFolder(Fixture));
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
		TestEqual(TEXT("The header counts them"), File.FrameCountInHeader, 30);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoSubsystemStopsForGoodAtTheLengthLimitTest,
	"Flock.Playtest.Video.Subsystem.StopsForGoodAtTheLengthLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoSubsystemStopsForGoodAtTheLengthLimitTest::RunTest(const FString& Parameters)
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
	ExpectPlaytestStatus(*this, TEXT("Ready again"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);
	PlayVideoFrames(Fixture, 1.0);
	TestEqual(TEXT("Still one frame source"), Log->SourcesCreated, 1);
	TestFalse(TEXT("Not recording"), Fixture.Playtest->IsRecordingVideo());
	TestEqual(TEXT("Still one file"), FilesIn(RecordingsFolder(Fixture)).Num(), 1);

	TestTrue(TEXT("The session can still be ended"), Fixture.Playtest->EndPlaytestSession());
	TestEqual(TEXT("And its end is sent"), Fixture.SessionEnds(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoSubsystemStopsForGoodAtTheSizeLimitTest,
	"Flock.Playtest.Video.Subsystem.StopsForGoodAtTheSizeLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoSubsystemStopsForGoodAtTheSizeLimitTest::RunTest(const FString& Parameters)
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoSubsystemBackgroundTimeIsNotRecordedTest,
	"Flock.Playtest.Video.Subsystem.BackgroundTimeIsNotRecorded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoSubsystemBackgroundTimeIsNotRecordedTest::RunTest(const FString& Parameters)
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoSubsystemStopsWhenPlaytestingStopsTest,
	"Flock.Playtest.Video.Subsystem.StopsWhenPlaytestingStopsAndNeverRecordsTwice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoSubsystemStopsWhenPlaytestingStopsTest::RunTest(const FString& Parameters)
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
	ExpectPlaytestStatus(*this, TEXT("Ready again"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);
	TestFalse(TEXT("No second recording starts"), Fixture.Playtest->IsRecordingVideo());
	TestEqual(TEXT("One frame source"), Log->SourcesCreated, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoSubsystemGameInstanceShutDownFinishesTheFileTest,
	"Flock.Playtest.Video.Subsystem.GameInstanceShutDownFinishesTheFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoSubsystemGameInstanceShutDownFinishesTheFileTest::RunTest(const FString& Parameters)
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
	const TArray<FString> Files = FilesIn(RecordingsFolder(Fixture));
	if (TestEqual(TEXT("One file"), Files.Num(), 1))
	{
		TestTrue(TEXT("Under its final name"), Files[0].EndsWith(TEXT(".ivf")));
	}
	FVideoFileRead File;
	if (TestTrue(TEXT("The file reads back"), ReadVideoFile(Path, File)))
	{
		TestEqual(TEXT("The header counts every frame"), File.FrameCountInHeader, File.Frames.Num());
		TestEqual(TEXT("A second of frames"), File.Frames.Num(), 30);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoRecordingDropsFramesWhenEncodingFallsBehindTest,
	"Flock.Playtest.Video.Recording.DropsFramesWhenEncodingFallsBehind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoRecordingDropsFramesWhenEncodingFallsBehindTest::RunTest(const FString& Parameters)
{
	const FString Folder = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockPlaytestTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FFlockPlaytestVideoSettings VideoSettings;
	FEvent* Gate = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ true);
	FString Error;
	{
		const TSharedRef<FTestVideoFrameSource> Source = MakeShared<FTestVideoFrameSource>(FIntPoint(64, 36));
		const TSharedPtr<FFlockPlaytestVideoRecording> Recording = FFlockPlaytestVideoRecording::Start(Source, VideoSettings,
			FPaths::Combine(Folder, TEXT("held.ivf")), Error, [Gate]() { Gate->Wait(); });
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
		Recording->StopCapturing(EFlockPlaytestVideoStopReason::StoppedByGame);
		Recording->WaitUntilWritten();

		const FFlockPlaytestVideoRecordingSummary Summary = Recording->GetSummary();
		TestEqual(TEXT("Eight frames waited, and the one handed over at the stop"), Summary.FramesWritten, 9);
		TestEqual(TEXT("The rest were dropped and counted"), Summary.FramesDroppedBecauseEncodingFellBehind, 51);
	}
	FPlatformProcess::ReturnSynchEventToPool(Gate);
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoSubsystemNothingCapturedKeepsNoFileTest,
	"Flock.Playtest.Video.Subsystem.NothingCapturedKeepsNoFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoSubsystemNothingCapturedKeepsNoFileTest::RunTest(const FString& Parameters)
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
	TestEqual(TEXT("And none is left behind"), FilesIn(RecordingsFolder(Fixture)).Num(), 0);
	TestEqual(TEXT("The log says so"), Capture.LinesContaining(TEXT("before any frame was captured")).Num(), 1);
	TestFalse(TEXT("The video ticker stops"), Fixture.Playtest->IsVideoTickerRunningForTesting());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoSubsystemCouldNotStartWarnsOnceTest,
	"Flock.Playtest.Video.Subsystem.CouldNotStartWarnsOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoSubsystemCouldNotStartWarnsOnceTest::RunTest(const FString& Parameters)
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoRecordingWritesNothingAfterTheSizeLimitTest,
	"Flock.Playtest.Video.Recording.WritesNothingAfterTheSizeLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoRecordingWritesNothingAfterTheSizeLimitTest::RunTest(const FString& Parameters)
{
	const FString Folder = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockPlaytestTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));

	// Eleven frames of 320 by 180, the sixth of them noise, which takes far more room than the smooth frames around it.
	const auto RecordElevenFrames = [&Folder](const TCHAR* Name, int64 MaxBytes, FFlockPlaytestVideoRecordingSummary& OutSummary, FString& OutError)
	{
		FFlockPlaytestVideoSettings VideoSettings;
		VideoSettings.BitrateKbps = 20000;
		VideoSettings.MaxBytes = MaxBytes;
		const TSharedRef<FTestVideoFrameSource> Source = MakeShared<FTestVideoFrameSource>(FIntPoint(320, 180));
		const TSharedPtr<FFlockPlaytestVideoRecording> Recording = FFlockPlaytestVideoRecording::Start(Source, VideoSettings,
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
		Recording->StopCapturing(EFlockPlaytestVideoStopReason::StoppedByGame);
		Recording->WaitUntilWritten();
		OutSummary = Recording->GetSummary();
		return true;
	};

	FFlockPlaytestVideoRecordingSummary Unlimited;
	FString Error;
	FVideoFileRead Control;
	if (!TestTrue(TEXT("The control recording is made"), RecordElevenFrames(TEXT("control.ivf"), 1LL << 40, Unlimited, Error))
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
	int64 MaxBytes = FFlockPlaytestVideoFile::FileHeaderBytes;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		MaxBytes += FFlockPlaytestVideoFile::FrameHeaderBytes + Control.Frames[Index].Bytes.Num();
	}
	MaxBytes += FFlockPlaytestVideoFile::FrameHeaderBytes + SmoothBytes + (NoiseBytes - SmoothBytes) / 2;

	FFlockPlaytestVideoRecordingSummary Limited;
	if (TestTrue(TEXT("The limited recording is made"), RecordElevenFrames(TEXT("limited.ivf"), MaxBytes, Limited, Error)))
	{
		TestEqual(TEXT("The five frames before the refused one are written, and none after it"), Limited.FramesWritten, 5);
		TestTrue(TEXT("Within the limit"), Limited.BytesWritten <= MaxBytes);
	}
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoRecordingAsksForNoFrameWhileEarlierOnesAreOnTheirWayTest,
	"Flock.Playtest.Video.Recording.AsksForNoFrameWhileEarlierOnesAreOnTheirWay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoRecordingAsksForNoFrameWhileEarlierOnesAreOnTheirWayTest::RunTest(const FString& Parameters)
{
	const FString Folder = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockPlaytestTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FFlockPlaytestVideoSettings VideoSettings;
	const TSharedRef<FTestVideoFrameSource> Source = MakeShared<FTestVideoFrameSource>(FIntPoint(64, 36));
	Source->bReadyForAnotherFrame = false;
	FString Error;
	{
		const TSharedPtr<FFlockPlaytestVideoRecording> Recording = FFlockPlaytestVideoRecording::Start(Source, VideoSettings,
			FPaths::Combine(Folder, TEXT("waiting.ivf")), Error);
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

		Recording->StopCapturing(EFlockPlaytestVideoStopReason::StoppedByGame);
		Recording->StopCapturing(EFlockPlaytestVideoStopReason::StoppedByGame);
		for (int32 Index = 0; Index < 5; ++Index)
		{
			Recording->AddFrame(1.0 / 30.0);
		}
		TestEqual(TEXT("Nothing is asked for after the stop"), Source->FramesAskedFor, 1);
		Recording->WaitUntilWritten();

		const FFlockPlaytestVideoRecordingSummary Summary = Recording->GetSummary();
		TestEqual(TEXT("Each capture time that passed while waiting is counted, and none after the stop"), Summary.FramesNotReadyInTime, 30);
		TestEqual(TEXT("The one frame is written"), Summary.FramesWritten, 1);
		TestTrue(FString::Printf(TEXT("A second stop changed nothing (error: '%s')"), *Summary.Error), Summary.Error.IsEmpty());
		TestFalse(TEXT("And the file is kept"), Summary.FilePath.IsEmpty());
	}
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoRecordingAStalledWriteDoesNotHoldEncodingUpTest,
	"Flock.Playtest.Video.Recording.AStalledWriteDoesNotHoldEncodingUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoRecordingAStalledWriteDoesNotHoldEncodingUpTest::RunTest(const FString& Parameters)
{
	const FString Folder = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockPlaytestTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FEvent* WriteGate = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ true);
	FString Error;
	{
		const TSharedRef<FTestVideoFrameSource> Source = MakeShared<FTestVideoFrameSource>(FIntPoint(64, 36));
		const TSharedPtr<FFlockPlaytestVideoRecording> Recording = FFlockPlaytestVideoRecording::Start(Source, FFlockPlaytestVideoSettings(),
			FPaths::Combine(Folder, TEXT("stalled.ivf")), Error, nullptr, [WriteGate]() { WriteGate->Wait(); });
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
		Recording->StopCapturing(EFlockPlaytestVideoStopReason::StoppedByGame);
		Recording->WaitUntilWritten();

		const FFlockPlaytestVideoRecordingSummary Summary = Recording->GetSummary();
		TestEqual(TEXT("Encoding kept up while the write was held, so no frame was dropped"), DroppedWhileHeld, 0);
		TestEqual(TEXT("Every frame is written once the disk lets go"), Summary.FramesWritten, 30);
		TestTrue(FString::Printf(TEXT("The held write is measured (%.0f ms)"), Summary.LongestWriteMs), Summary.LongestWriteMs >= 100.0);
	}
	FPlatformProcess::ReturnSynchEventToPool(WriteGate);
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoRecordingDropsFramesWhenWritingFallsBehindTest,
	"Flock.Playtest.Video.Recording.DropsFramesWhenWritingFallsBehind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoRecordingDropsFramesWhenWritingFallsBehindTest::RunTest(const FString& Parameters)
{
	const FString Folder = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockPlaytestTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FEvent* WriteGate = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ true);
	FString Error;
	{
		const TSharedRef<FTestVideoFrameSource> Source = MakeShared<FTestVideoFrameSource>(FIntPoint(64, 36));
		const TSharedPtr<FFlockPlaytestVideoRecording> Recording = FFlockPlaytestVideoRecording::Start(Source, FFlockPlaytestVideoSettings(),
			FPaths::Combine(Folder, TEXT("behind.ivf")), Error, nullptr, [WriteGate]() { WriteGate->Wait(); });
		if (!TestTrue(TEXT("The recording starts"), Recording.IsValid()))
		{
			AddError(Error);
			WriteGate->Trigger();
			FPlatformProcess::ReturnSynchEventToPool(WriteGate);
			return true;
		}

		// The disk holds every write while more frames arrive than may wait to be written.
		const int32 FramesPastTheLimit = 20;
		const int32 Frames = FFlockPlaytestVideoRecording::MaxFramesWaitingToWrite + FramesPastTheLimit;
		for (int32 Index = 0; Index < Frames; ++Index)
		{
			Recording->AddFrame(1.0 / 30.0);
			FPlatformProcess::Sleep(0.002f);
		}
		WriteGate->Trigger();
		Recording->StopCapturing(EFlockPlaytestVideoStopReason::StoppedByGame);
		Recording->WaitUntilWritten();

		const FFlockPlaytestVideoRecordingSummary Summary = Recording->GetSummary();
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoSubsystemCannotRecordWarnsOnceTest,
	"Flock.Playtest.Video.Subsystem.CannotRecordWarnsOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoSubsystemCannotRecordWarnsOnceTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedVideoSettings Video;
	FPlaytestFixture Fixture;
	const TSharedRef<int32> Asked = MakeShared<int32>(0);
	Fixture.Playtest->SetVideoRecordingFolderForTesting(RecordingsFolder(Fixture));
	Fixture.Playtest->SetVideoFrameSourceFactoryForTesting([Asked](FIntPoint, FString& OutWhyNot) -> TSharedPtr<IFlockPlaytestVideoFrameSource>
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
	TestEqual(TEXT("No file"), FilesIn(RecordingsFolder(Fixture)).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoSubsystemWaitsForTheGameViewportTest,
	"Flock.Playtest.Video.Subsystem.WaitsForTheGameViewport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoSubsystemWaitsForTheGameViewportTest::RunTest(const FString& Parameters)
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoSubsystemPlayInEditorSettingTest,
	"Flock.Playtest.Video.Subsystem.PlayInEditorSettingRecordsOnlyInPlayInEditor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoSubsystemPlayInEditorSettingTest::RunTest(const FString& Parameters)
{
	UFlockPlaytestLocalSettings* LocalSettings = GetMutableDefault<UFlockPlaytestLocalSettings>();
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoSubsystemTestRecordingStopsAfterItsSecondsTest,
	"Flock.Playtest.Video.Subsystem.TestRecordingStopsAfterItsSeconds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoSubsystemTestRecordingStopsAfterItsSecondsTest::RunTest(const FString& Parameters)
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

#endif
