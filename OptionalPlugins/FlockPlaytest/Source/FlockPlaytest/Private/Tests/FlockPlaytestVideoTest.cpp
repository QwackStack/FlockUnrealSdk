// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockPlaytestSettings.h"
#include "FlockPlaytestVideoEncoder.h"
#include "FlockPlaytestVideoFile.h"
#include "FlockPlaytestVideoFrameSchedule.h"
#include "Tests/FlockPlaytestVideoTestSupport.h"
#include "UObject/Package.h"

using namespace FlockPlaytestVideoTesting;

namespace
{
	using EFrameResult = FFlockPlaytestVideoFrameSchedule::EFrameResult;

	/** Hands the schedule each frame time, and returns the index and time of every frame it captured. */
	TArray<TPair<int32, int64>> CapturedFrames(FFlockPlaytestVideoFrameSchedule& Schedule, const TArray<double>& FrameSeconds)
	{
		TArray<TPair<int32, int64>> Captured;
		for (int32 Index = 0; Index < FrameSeconds.Num(); ++Index)
		{
			int64 TimestampMs = -1;
			if (Schedule.AddFrame(FrameSeconds[Index], TimestampMs) == EFrameResult::Capture)
			{
				Captured.Add({ Index, TimestampMs });
			}
		}
		return Captured;
	}

	TArray<double> RepeatedFrames(double Seconds, int32 Count)
	{
		TArray<double> Frames;
		Frames.Init(Seconds, Count);
		return Frames;
	}

	FString UniqueTestFilePath(const TCHAR* Name)
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockPlaytestTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits), Name);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoScheduleCapturesAtTheFrameRateTest,
	"Flock.Playtest.Video.Schedule.CapturesAtTheFrameRate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoScheduleCapturesAtTheFrameRateTest::RunTest(const FString& Parameters)
{
	{
		FFlockPlaytestVideoFrameSchedule Schedule(30, 3600.0);
		const TArray<TPair<int32, int64>> Captured = CapturedFrames(Schedule, RepeatedFrames(1.0 / 60.0, 60));
		if (TestEqual(TEXT("A second of a 60 fps game gives 30 frames"), Captured.Num(), 30))
		{
			TestEqual(TEXT("The first frame is shown at the start"), Captured[0].Value, 0LL);
			TestEqual(TEXT("The second is the third frame drawn"), Captured[1].Key, 2);
			TestEqual(TEXT("Shown 33 ms in"), Captured[1].Value, 33LL);
		}
	}
	{
		FFlockPlaytestVideoFrameSchedule Schedule(30, 3600.0);
		TestEqual(TEXT("A game slower than the capture rate has every frame captured"),
			CapturedFrames(Schedule, RepeatedFrames(0.05, 20)).Num(), 20);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoScheduleWobblyFrameTimesTest,
	"Flock.Playtest.Video.Schedule.WobblyFrameTimesStillCaptureEveryOtherFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoScheduleWobblyFrameTimesTest::RunTest(const FString& Parameters)
{
	// A 60 fps game whose frame times wobble either side of 16.667 ms. Every other frame is still the nearest one to each
	// capture time, so the video keeps a steady rhythm.
	const double Pattern[] = { 0.016267, 0.016667, 0.017067, 0.016667 };
	TArray<double> Frames;
	for (int32 Index = 0; Index < 120; ++Index)
	{
		Frames.Add(Pattern[Index % 4]);
	}
	FFlockPlaytestVideoFrameSchedule Schedule(30, 3600.0);
	const TArray<TPair<int32, int64>> Captured = CapturedFrames(Schedule, Frames);
	TestEqual(TEXT("Two seconds give 60 frames"), Captured.Num(), 60);
	for (int32 Index = 1; Index < Captured.Num(); ++Index)
	{
		if (Captured[Index].Key - Captured[Index - 1].Key != 2)
		{
			AddError(FString::Printf(TEXT("Frame %d was captured %d frames after the one before, not 2"), Captured[Index].Key,
				Captured[Index].Key - Captured[Index - 1].Key));
			break;
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoScheduleLeavesOutFramesTest,
	"Flock.Playtest.Video.Schedule.LeavesOutTheFrameAfterTheBackground",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoScheduleLeavesOutFramesTest::RunTest(const FString& Parameters)
{
	FFlockPlaytestVideoFrameSchedule Schedule(30, 3600.0);
	CapturedFrames(Schedule, RepeatedFrames(0.1, 10));
	TestEqual(TEXT("A second recorded"), Schedule.GetRecordedSeconds(), 1.0, 1e-9);

	Schedule.LeaveOutNextFrame();
	int64 TimestampMs = -1;
	TestTrue(TEXT("The frame carrying the time away is not captured"), Schedule.AddFrame(300.0, TimestampMs) == EFrameResult::Skip);
	TestEqual(TEXT("And its time is not recorded"), Schedule.GetRecordedSeconds(), 1.0, 1e-9);
	TestTrue(TEXT("The frame after it is"), Schedule.AddFrame(0.1, TimestampMs) == EFrameResult::Capture);
	TestEqual(TEXT("Shown straight after the time before it"), TimestampMs, 1000LL);

	TestTrue(TEXT("A frame time of zero is not a frame"), Schedule.AddFrame(0.0, TimestampMs) == EFrameResult::Skip);
	TestTrue(TEXT("Nor is a negative one"), Schedule.AddFrame(-1.0, TimestampMs) == EFrameResult::Skip);
	TestEqual(TEXT("Neither adds time"), Schedule.GetRecordedSeconds(), 1.1, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoScheduleStopsAtTheLengthLimitTest,
	"Flock.Playtest.Video.Schedule.StopsAtTheLengthLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoScheduleStopsAtTheLengthLimitTest::RunTest(const FString& Parameters)
{
	FFlockPlaytestVideoFrameSchedule Schedule(30, 1.0);
	int32 Captures = 0;
	int64 LastTimestampMs = -1;
	bool bReachedLimit = false;
	for (int32 Index = 0; Index < 100 && !bReachedLimit; ++Index)
	{
		int64 TimestampMs = -1;
		switch (Schedule.AddFrame(0.05, TimestampMs))
		{
		case EFrameResult::Capture:
			++Captures;
			LastTimestampMs = TimestampMs;
			break;
		case EFrameResult::ReachedLengthLimit:
			bReachedLimit = true;
			TestEqual(TEXT("The limit is reached by the frame that starts at one second"), Index, 20);
			break;
		default:
			break;
		}
	}
	TestTrue(TEXT("The limit is reached"), bReachedLimit);
	TestEqual(TEXT("Twenty frames fit in a second"), Captures, 20);
	TestTrue(TEXT("No frame is shown at or after the limit"), LastTimestampMs < 1000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoScheduleNeverRepeatsATimeTest,
	"Flock.Playtest.Video.Schedule.NeverRepeatsATime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoScheduleNeverRepeatsATimeTest::RunTest(const FString& Parameters)
{
	// A tiny frame between two long ones at 30 frames a second. The tiny one reaches a capture time, and so does the long
	// one after it, which starts 0.2 ms later: rounded to milliseconds, both would be shown at 40 ms.
	FFlockPlaytestVideoFrameSchedule Schedule(30, 3600.0);
	int64 TimestampMs = -1;
	TestTrue(TEXT("The long first frame is captured"), Schedule.AddFrame(0.0402, TimestampMs) == EFrameResult::Capture);
	TestEqual(TEXT("At the start"), TimestampMs, 0LL);
	TestTrue(TEXT("The tiny frame reaches the next capture time"), Schedule.AddFrame(0.0002, TimestampMs) == EFrameResult::Capture);
	TestEqual(TEXT("It is shown at 40 ms"), TimestampMs, 40LL);
	TimestampMs = -1;
	TestTrue(TEXT("The long frame after it, also at 40 ms once rounded, is not captured"),
		Schedule.AddFrame(0.060, TimestampMs) == EFrameResult::Skip);
	TestEqual(TEXT("And no time is handed back for it"), TimestampMs, -1LL);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoFitsInsideTheVideoSizeTest,
	"Flock.Playtest.Video.FitsInsideTheVideoSize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoFitsInsideTheVideoSizeTest::RunTest(const FString& Parameters)
{
	const FIntPoint Max(1280, 720);
	TestEqual(TEXT("A 1440p screen halves"), FitVideoSizeInside(FIntPoint(2560, 1440), Max), FIntPoint(1280, 720));
	TestEqual(TEXT("A 1080p screen"), FitVideoSizeInside(FIntPoint(1920, 1080), Max), FIntPoint(1280, 720));
	TestEqual(TEXT("An ultrawide screen keeps its shape, with even sides"), FitVideoSizeInside(FIntPoint(3440, 1440), Max), FIntPoint(1280, 536));
	TestEqual(TEXT("A 4:3 screen is held by its height"), FitVideoSizeInside(FIntPoint(1024, 768), Max), FIntPoint(960, 720));
	TestEqual(TEXT("A smaller window is not enlarged"), FitVideoSizeInside(FIntPoint(640, 360), Max), FIntPoint(640, 360));
	TestEqual(TEXT("Odd sides round down to even"), FitVideoSizeInside(FIntPoint(641, 361), Max), FIntPoint(640, 360));
	TestEqual(TEXT("A screen with no size has no video size"), FitVideoSizeInside(FIntPoint(0, 0), Max), FIntPoint(0, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoWhyVideoCannotBeRecordedTest,
	"Flock.Playtest.Video.WhyVideoCannotBeRecorded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoWhyVideoCannotBeRecordedTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Nothing stops it"), DecideWhyVideoCannotBeRecorded(true, true, 0).IsEmpty());
	TestTrue(TEXT("A build with no encoder names the platform it is built for"),
		DecideWhyVideoCannotBeRecorded(false, true, 0).Contains(TEXT("64-bit Windows")));
	TestTrue(TEXT("A process that draws nothing names -nullrhi"), DecideWhyVideoCannotBeRecorded(true, false, 0).Contains(TEXT("-nullrhi")));
	TestTrue(TEXT("A frame grabber latency names the console variable"),
		DecideWhyVideoCannotBeRecorded(true, true, 1).Contains(TEXT("framegrabber.framelatency is 1")));
	TestTrue(TEXT("No encoder is the first reason given"), DecideWhyVideoCannotBeRecorded(false, false, 1).Contains(TEXT("64-bit Windows")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoSettingsKeptInRangeTest,
	"Flock.Playtest.Video.SettingsAreKeptInRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoSettingsKeptInRangeTest::RunTest(const FString& Parameters)
{
	const UFlockPlaytestSettings* Defaults = NewObject<UFlockPlaytestSettings>(GetTransientPackage());
	const FFlockPlaytestVideoSettings FromDefaults = FFlockPlaytestVideoSettings::FromProjectSettings(*Defaults);
	TestEqual(TEXT("1280 by 720 by default"), FromDefaults.MaxVideoSize, FIntPoint(1280, 720));
	TestEqual(TEXT("30 frames a second"), FromDefaults.FramesPerSecond, 30);
	TestEqual(TEXT("2000 kbps"), FromDefaults.BitrateKbps, 2000);
	TestEqual(TEXT("60 minutes"), FromDefaults.MaxSeconds, 3600.0, 1e-6);
	TestEqual(TEXT("1.5 GB"), FromDefaults.MaxBytes, 1536LL * 1024 * 1024);

	UFlockPlaytestSettings* OutOfRange = NewObject<UFlockPlaytestSettings>(GetTransientPackage());
	OutOfRange->VideoWidth = 99999;
	OutOfRange->VideoHeight = 0;
	OutOfRange->VideoFramesPerSecond = 0;
	OutOfRange->VideoBitrateKbps = 1;
	OutOfRange->MaxRecordingMinutes = 0.f;
	OutOfRange->MaxRecordingSizeMb = -5;
	const FFlockPlaytestVideoSettings Kept = FFlockPlaytestVideoSettings::FromProjectSettings(*OutOfRange);
	TestEqual(TEXT("Width held at 3840, height at 16"), Kept.MaxVideoSize, FIntPoint(3840, 16));
	TestEqual(TEXT("At least one frame a second"), Kept.FramesPerSecond, 1);
	TestEqual(TEXT("At least 100 kbps"), Kept.BitrateKbps, 100);
	TestEqual(TEXT("At least six seconds"), Kept.MaxSeconds, 6.0, 1e-6);
	TestEqual(TEXT("At least one megabyte"), Kept.MaxBytes, 1024LL * 1024);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoFileWritesHeaderAndFramesTest,
	"Flock.Playtest.Video.File.WritesTheFrameCountOnClose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoFileWritesHeaderAndFramesTest::RunTest(const FString& Parameters)
{
	const FString Path = UniqueTestFilePath(TEXT("frames.ivf"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	{
		FFlockPlaytestVideoFile File;
		FString Error;
		if (!TestTrue(TEXT("The file opens"), File.Open(Path, FIntPoint(64, 36), Error)))
		{
			return true;
		}
		TestEqual(TEXT("The header is written"), File.GetBytesWritten(), FFlockPlaytestVideoFile::FileHeaderBytes);

		TestTrue(TEXT("Frame 1"), File.WriteFrame({ 1, 2, 3, 4, 5 }, 0));
		TestTrue(TEXT("Frame 2"), File.WriteFrame({ 6, 7, 8, 9, 10, 11 }, 33));
		TestTrue(TEXT("Frame 3"), File.WriteFrame({ 12, 13, 14, 15, 16, 17, 18 }, 67));
		TestEqual(TEXT("Bytes counted"), File.GetBytesWritten(), 32LL + 3 * 12 + 18);
		TestTrue(TEXT("It closes"), File.Close());
	}

	FVideoFileRead Read;
	if (TestTrue(TEXT("It reads back"), ReadVideoFile(Path, Read)))
	{
		TestEqual(TEXT("Signature"), Read.Signature, FString(TEXT("DKIF")));
		TestEqual(TEXT("Codec"), Read.Codec, FString(TEXT("VP90")));
		TestEqual(TEXT("Width"), Read.Width, 64);
		TestEqual(TEXT("Height"), Read.Height, 36);
		TestEqual(TEXT("Times are in milliseconds"), Read.TimeBaseDenominator, 1000u);
		TestEqual(TEXT("Time base numerator"), Read.TimeBaseNumerator, 1u);
		TestEqual(TEXT("The header holds the frame count"), Read.FrameCountInHeader, 3);
		TestEqual(TEXT("The file is the size counted"), Read.FileBytes, 32LL + 3 * 12 + 18);
		if (TestEqual(TEXT("Three frames"), Read.Frames.Num(), 3))
		{
			TestEqual(TEXT("Frame 3's time"), Read.Frames[2].TimestampMs, 67LL);
			TestEqual(TEXT("Frame 2's bytes"), Read.Frames[1].Bytes, TArray<uint8>({ 6, 7, 8, 9, 10, 11 }));
		}
	}
	IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Path), false, true);
	return true;
}

#if WITH_FLOCK_PLAYTEST_VIDEO

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoEncoderConvertsColoursTest,
	"Flock.Playtest.Video.Encoder.ConvertsColoursToVideoColours",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoEncoderConvertsColoursTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* Name;
		FColor Colour;
		uint8 Brightness;
		uint8 Blue;
		uint8 Red;
	};
	// BT.601 limited range, as players read VP9 that names no colour space.
	const FCase Cases[] = {
		{ TEXT("White"), FColor(255, 255, 255, 255), 235, 128, 128 },
		{ TEXT("Black"), FColor(0, 0, 0, 255), 16, 128, 128 },
		{ TEXT("Red"), FColor(255, 0, 0, 255), 82, 90, 240 },
		{ TEXT("Blue"), FColor(0, 0, 255, 255), 41, 240, 110 },
	};
	for (const FCase& Case : Cases)
	{
		TArray<FColor> Pixels;
		Pixels.Init(Case.Colour, 4);
		TArray<uint8> Planes;
		FFlockPlaytestVideoEncoder::ConvertToVideoColours(Pixels, FIntPoint(2, 2), Planes);
		if (TestEqual(FString::Printf(TEXT("%s: four brightness values and two colour values"), Case.Name), Planes.Num(), 6))
		{
			TestEqual(FString::Printf(TEXT("%s brightness"), Case.Name), Planes[0], Case.Brightness);
			TestEqual(FString::Printf(TEXT("%s blue difference"), Case.Name), Planes[4], Case.Blue);
			TestEqual(FString::Printf(TEXT("%s red difference"), Case.Name), Planes[5], Case.Red);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoEncoderEncodesVideoThatDecodesTest,
	"Flock.Playtest.Video.Encoder.EncodesVideoThatDecodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoEncoderEncodesVideoThatDecodesTest::RunTest(const FString& Parameters)
{
	const FIntPoint Size(320, 180);
	FString Error;
	{
		FFlockPlaytestVideoEncoder OddSize;
		TestFalse(TEXT("An odd side is refused"), OddSize.Initialize(FIntPoint(321, 180), 30, 2000, Error));
		TestTrue(TEXT("And says why"), Error.Contains(TEXT("even")));
	}

	FFlockPlaytestVideoEncoder Encoder;
	if (!TestTrue(TEXT("The encoder sets up"), Encoder.Initialize(Size, 30, 2000, Error)))
	{
		AddError(Error);
		return true;
	}
	TArray<FFlockPlaytestEncodedFrame> Refused;
	TestFalse(TEXT("A frame of another size is refused"), Encoder.Encode(MakeTestPixels(FIntPoint(64, 36), 0), 0, 33, Refused, Error));
	TestEqual(TEXT("And gives nothing"), Refused.Num(), 0);

	FVideoFileRead File;
	File.Width = Size.X;
	File.Height = Size.Y;
	TArray<FFlockPlaytestEncodedFrame> Encoded;
	for (int32 Index = 0; Index < 30; ++Index)
	{
		if (!Encoder.Encode(MakeTestPixels(Size, Index), Index * 33, 33, Encoded, Error))
		{
			AddError(FString::Printf(TEXT("Frame %d: %s"), Index, *Error));
			return true;
		}
	}
	TestTrue(TEXT("It finishes"), Encoder.Finish(Encoded, Error));
	if (!TestEqual(TEXT("One encoded frame for each frame handed over"), Encoded.Num(), 30))
	{
		return true;
	}
	TestTrue(TEXT("The first is a key frame"), Encoded[0].bKeyFrame);
	TestEqual(TEXT("Times carried through"), Encoded[29].TimestampMs, 29LL * 33);

	for (const FFlockPlaytestEncodedFrame& Frame : Encoded)
	{
		File.Frames.Add({ Frame.TimestampMs, Frame.Bytes });
	}
	FIntPoint PictureSize;
	double LastBrightness = -1.0;
	TestEqual(TEXT("Every frame decodes"), DecodeVideoFile(File, PictureSize, LastBrightness), 30);
	TestEqual(TEXT("At the size encoded"), PictureSize, Size);

	TArray<uint8> SourcePlanes;
	FFlockPlaytestVideoEncoder::ConvertToVideoColours(MakeTestPixels(Size, 29), Size, SourcePlanes);
	double SourceBrightness = 0.0;
	for (int32 Index = 0; Index < Size.X * Size.Y; ++Index)
	{
		SourceBrightness += SourcePlanes[Index];
	}
	SourceBrightness /= Size.X * Size.Y;
	TestEqual(TEXT("The last picture is about as bright as the frame it came from"), LastBrightness, SourceBrightness, 8.0);
	return true;
}

#endif

#endif
