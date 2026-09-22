// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "ProtokitePlaytestPerformanceTimeline.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"

namespace
{
	constexpr uint64 Megabyte = 1024 * 1024;

	/** A timeline, and the engine frame number its next frame is added under. */
	struct FTimelineUnderTest
	{
		FProtokitePlaytestPerformanceTimeline Timeline;
		uint64 EngineFrame = 0;

		/** Reads the engine's hitch threshold and the process's memory. */
		FTimelineUnderTest() = default;

		/** A fixed hitch threshold, 512 MB in use and a 1024 MB peak. */
		explicit FTimelineUnderTest(double HitchThresholdMs)
			: Timeline(
				[HitchThresholdMs]() { return HitchThresholdMs; },
				[]()
				{
					FProtokitePlaytestPerformanceTimeline::FMemoryUse Use;
					Use.UsedBytes = 512 * Megabyte;
					Use.PeakUsedBytes = 1024 * Megabyte;
					return Use;
				})
		{
		}

		/** Adds one frame in an engine frame of its own. */
		bool AddFrame(double FrameSeconds, FProtokitePlaytestPerformanceWindow& OutWindow)
		{
			return Timeline.AddFrame(FrameSeconds, ++EngineFrame, OutWindow);
		}

		/** Adds Count frames, each in an engine frame of its own, and returns the windows they finished. */
		TArray<FProtokitePlaytestPerformanceWindow> AddFrames(int32 Count, double FrameSeconds)
		{
			TArray<FProtokitePlaytestPerformanceWindow> Windows;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				FProtokitePlaytestPerformanceWindow Window;
				if (AddFrame(FrameSeconds, Window))
				{
					Windows.Add(Window);
				}
			}
			return Windows;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestPerformanceTimelineTenSecondsOfPlayMakeAWindowTest,
	"Protokite.Playtest.PerformanceTimeline.TenSecondsOfPlayMakeAWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestPerformanceTimelineTenSecondsOfPlayMakeAWindowTest::RunTest(const FString& Parameters)
{
	// The engine's ticker hands out frame times as floats.
	{
		FTimelineUnderTest Test(60.0);
		const TArray<FProtokitePlaytestPerformanceWindow> Windows = Test.AddFrames(3600, static_cast<float>(1.0 / 60.0));
		TestEqual(TEXT("A minute at 60 frames a second makes six windows"), Windows.Num(), 6);
		for (const FProtokitePlaytestPerformanceWindow& Window : Windows)
		{
			TestEqual(TEXT("Each window holds ten seconds of frames"), Window.Frames, 600);
			TestTrue(TEXT("And covers ten seconds"), Window.Seconds >= 10.0 - 1e-6 && Window.Seconds < 10.02);
		}
	}

	// The same minute handed over in double precision.
	{
		FTimelineUnderTest Test(60.0);
		TestEqual(TEXT("Double-precision frame times make the same six windows"), Test.AddFrames(3600, 1.0 / 60.0).Num(), 6);
	}

	// A frame with no time is not a frame.
	{
		FTimelineUnderTest Test(60.0);
		FProtokitePlaytestPerformanceWindow Window;
		TestFalse(TEXT("A zero-length frame finishes nothing"), Test.AddFrame(0.0, Window));
		TestFalse(TEXT("Nor does a negative one"), Test.AddFrame(-1.0, Window));
		const TArray<FProtokitePlaytestPerformanceWindow> Windows = Test.AddFrames(1, 10.0);
		if (TestEqual(TEXT("A ten-second frame finishes a window"), Windows.Num(), 1))
		{
			TestEqual(TEXT("Holding only that frame"), Windows[0].Frames, 1);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestPerformanceTimelinePercentilesAndHitchesTest,
	"Protokite.Playtest.PerformanceTimeline.PercentilesAndHitches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestPerformanceTimelinePercentilesAndHitchesTest::RunTest(const FString& Parameters)
{
	// Frames of 1 to 99 ms, out of order, then one long frame that finishes the window: 100 frames in all.
	const double Threshold = (60 / 1000.0) * 1000.0;
	FTimelineUnderTest Test(Threshold);
	FProtokitePlaytestPerformanceWindow Window;
	bool bFinished = false;
	for (int32 Ms = 99; Ms >= 1; Ms -= 2)
	{
		bFinished |= Test.AddFrame(Ms / 1000.0, Window);
	}
	for (int32 Ms = 2; Ms <= 98; Ms += 2)
	{
		bFinished |= Test.AddFrame(Ms / 1000.0, Window);
	}
	TestFalse(TEXT("Precondition: 99 frames of 4.95 seconds finish no window"), bFinished);
	TestTrue(TEXT("The long frame finishes it"), Test.AddFrame(5.06, Window));

	TestEqual(TEXT("Frames"), Window.Frames, 100);
	TestEqual(TEXT("Seconds"), Window.Seconds, 10.01, 1e-6);
	TestEqual(TEXT("Median frame time"), Window.MedianFrameTimeMs, 50.0, 1e-6);
	TestEqual(TEXT("95th percentile"), Window.FrameTime95thPercentileMs, 95.0, 1e-6);
	TestEqual(TEXT("99th percentile"), Window.FrameTime99thPercentileMs, 99.0, 1e-6);
	TestEqual(TEXT("The threshold is reported"), Window.HitchThresholdMs, Threshold, 1e-9);
	TestEqual(TEXT("Hitches: 60 to 99 ms, the one exactly at the threshold included, and the long frame"), Window.Hitches, 41);
	TestEqual(TEXT("Memory in use"), Window.MemoryUsedMb, static_cast<int64>(512));
	TestEqual(TEXT("Peak memory"), Window.MemoryPeakMb, static_cast<int64>(1024));

	TArray<double> Empty;
	TestEqual(TEXT("No frames give no percentile"), FProtokitePlaytestPerformanceTimeline::Percentile(Empty, 95.0), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestPerformanceTimelineUsesTheEngineHitchThresholdTest,
	"Protokite.Playtest.PerformanceTimeline.UsesTheEngineHitchThreshold",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestPerformanceTimelineUsesTheEngineHitchThresholdTest::RunTest(const FString& Parameters)
{
	IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("t.HitchFrameTimeThreshold"));
	if (!TestNotNull(TEXT("The engine has a hitch threshold setting"), Variable))
	{
		return false;
	}
	const float Saved = Variable->GetFloat();
	TestNotEqual(TEXT("Precondition: the threshold is not already the one this test sets"), Saved, 25.f);
	Variable->Set(25.f, ECVF_SetByConsole);
	ON_SCOPE_EXIT
	{
		Variable->Set(Saved, ECVF_SetByConsole);
	};

	// With no readers given, the timeline reads the engine's own threshold and memory.
	FTimelineUnderTest Test;
	TArray<FProtokitePlaytestPerformanceWindow> Windows = Test.AddFrames(200, 0.030);
	Windows.Append(Test.AddFrames(210, 0.020));
	if (TestEqual(TEXT("One window"), Windows.Num(), 1))
	{
		TestEqual(TEXT("The engine's threshold is used"), Windows[0].HitchThresholdMs, 25.0, 1e-6);
		TestEqual(TEXT("So the 30 ms frames are hitches and the 20 ms ones are not"), Windows[0].Hitches, 200);
		TestTrue(TEXT("The process's memory is read"), Windows[0].MemoryUsedMb > 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestPerformanceTimelineLevelLoadLeavesOutTheNextEngineFrameTest,
	"Protokite.Playtest.PerformanceTimeline.LevelLoadLeavesOutTheNextEngineFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestPerformanceTimelineLevelLoadLeavesOutTheNextEngineFrameTest::RunTest(const FString& Parameters)
{
	FTimelineUnderTest Test(60.0);
	TestEqual(TEXT("Precondition: nine seconds finish no window"), Test.AddFrames(540, 1.0 / 60.0).Num(), 0);

	// A level load inside an engine frame's update. The same engine frame then hands over a time measured before the load.
	const uint64 LoadingEngineFrame = ++Test.EngineFrame;
	Test.Timeline.LeaveOutFirstFrameAfter(LoadingEngineFrame);
	FProtokitePlaytestPerformanceWindow Window;
	TestFalse(TEXT("The loading engine frame's own time is play"), Test.Timeline.AddFrame(1.0 / 60.0, LoadingEngineFrame, Window));

	// The next engine frame's time carries the load.
	TestFalse(TEXT("The next engine frame's time is left out, however long"), Test.AddFrame(30.0, Window));
	const TArray<FProtokitePlaytestPerformanceWindow> Windows = Test.AddFrames(59, 1.0 / 60.0);
	if (TestEqual(TEXT("The frames around the load make one window"), Windows.Num(), 1))
	{
		TestEqual(TEXT("With the loading engine frame's time in it"), Windows[0].Frames, 600);
		TestTrue(TEXT("And not the load"), Windows[0].Seconds >= 10.0 - 1e-6 && Windows[0].Seconds < 10.02);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestPerformanceTimelineLeftOutFramesAndDroppedWindowsTest,
	"Protokite.Playtest.PerformanceTimeline.LeftOutFramesAndDroppedWindows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestPerformanceTimelineLeftOutFramesAndDroppedWindowsTest::RunTest(const FString& Parameters)
{
	{
		FTimelineUnderTest Test(60.0);
		Test.Timeline.LeaveOutNextFrame();
		TestEqual(TEXT("A frame left out finishes no window, however long"), Test.AddFrames(1, 30.0).Num(), 0);
		TestEqual(TEXT("Only the next frame is left out"), Test.AddFrames(1, 9.0).Num(), 0);
		Test.Timeline.Reset();
		TestEqual(TEXT("After a reset, nine seconds finish nothing"), Test.AddFrames(1, 9.0).Num(), 0);
		const TArray<FProtokitePlaytestPerformanceWindow> Windows = Test.AddFrames(1, 1.0);
		if (TestEqual(TEXT("The dropped nine seconds do not count towards the window"), Windows.Num(), 1))
		{
			TestEqual(TEXT("The window holds the two frames since the reset"), Windows[0].Frames, 2);
			TestEqual(TEXT("And ten seconds"), Windows[0].Seconds, 10.0, 1e-6);
		}
	}
	{
		FTimelineUnderTest Test(60.0);
		Test.Timeline.LeaveOutNextFrame();
		FProtokitePlaytestPerformanceWindow Window;
		Test.AddFrame(0.0, Window);
		TestEqual(TEXT("A zero-length frame does not use up the frame to leave out"), Test.AddFrames(1, 10.0).Num(), 0);
	}
	{
		FTimelineUnderTest Test(60.0);
		Test.Timeline.LeaveOutNextFrame();
		Test.Timeline.LeaveOutFirstFrameAfter(Test.EngineFrame);
		Test.Timeline.Reset();
		TestEqual(TEXT("A reset also forgets the frames it was told to leave out"), Test.AddFrames(1, 10.0).Num(), 1);
	}
	return true;
}

#endif
