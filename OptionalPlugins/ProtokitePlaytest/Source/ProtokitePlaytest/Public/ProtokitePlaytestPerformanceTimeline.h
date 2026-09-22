// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"

/** The category and names of the events a playtest sends to the Flock SDK when the playtest turns heavy analytics on. */
namespace ProtokitePlaytestEvents
{
	/** Every playtest event is filed under this category, so a dashboard can show or hide them as a group. */
	inline constexpr const TCHAR* Category = TEXT("playtest");

	/** One per ten seconds of play: frame times, hitches and memory. */
	inline constexpr const TCHAR* PerformanceWindow = TEXT("performance_window");

	/** One per map the game instance finishes loading. */
	inline constexpr const TCHAR* LevelLoaded = TEXT("level_loaded");
}

/** What one window of play measured. */
struct PROTOKITEPLAYTEST_API FProtokitePlaytestPerformanceWindow
{
	/** The play time the window covers, added up from frame times, so a change to the clock cannot stretch it. */
	double Seconds = 0.0;

	int32 Frames = 0;

	double MedianFrameTimeMs = 0.0;
	double FrameTime95thPercentileMs = 0.0;
	double FrameTime99thPercentileMs = 0.0;

	/** Frames that took HitchThresholdMs or longer. */
	int32 Hitches = 0;
	double HitchThresholdMs = 0.0;

	/** The physical memory the process used when the window closed, and the most it has used since it started. */
	int64 MemoryUsedMb = 0;
	int64 MemoryPeakMb = 0;
};

/**
 * Turns frame times into windows of ten seconds of play. It is handed each frame's time and hands back each finished
 * window; it owns no timer and knows nothing about maps or where a window is sent.
 */
class PROTOKITEPLAYTEST_API FProtokitePlaytestPerformanceTimeline
{
public:
	static constexpr double WindowSeconds = 10.0;

	struct FMemoryUse
	{
		uint64 UsedBytes = 0;
		uint64 PeakUsedBytes = 0;
	};

	/**
	 * Without readers, the hitch threshold is the engine's t.HitchFrameTimeThreshold and the memory is the process's
	 * physical memory. Both are read when a window closes.
	 */
	explicit FProtokitePlaytestPerformanceTimeline(TFunction<double()> InReadHitchThresholdMs = nullptr,
		TFunction<FMemoryUse()> InReadMemoryUse = nullptr);

	/**
	 * Adds one frame. EngineFrameNumber is the engine's frame counter when the frame's time is handed over. Returns true,
	 * with OutWindow filled in, when this frame finishes a window. A frame with no time is not a frame.
	 */
	bool AddFrame(double FrameSeconds, uint64 EngineFrameNumber, FProtokitePlaytestPerformanceWindow& OutWindow);

	/**
	 * Leaves out the next frame added, because its time includes time that was not play. For time in the background: the
	 * next frame time handed over after the platform announces the return is the one that carries the time away.
	 */
	void LeaveOutNextFrame() { bLeaveOutNextFrame = true; }

	/**
	 * Leaves out the first frame added under an engine frame number later than EngineFrameNumber. For a level load that
	 * held the game up: the load runs inside the engine's update, and the frame time handed over later in that same
	 * engine frame was measured before the load began, so the next engine frame's time is the one that carries the load.
	 */
	void LeaveOutFirstFrameAfter(uint64 EngineFrameNumber) { LeaveOutFirstFrameAfterEngineFrame = EngineFrameNumber; }

	/** Drops the window in progress, and forgets every frame it was told to leave out. */
	void Reset();

	/** The frame time that Percent of the frames took or less (nearest rank), from times sorted shortest first. 0 with no frames. */
	static double Percentile(const TArray<double>& SortedFrameTimesMs, double Percent);

private:
	TFunction<double()> ReadHitchThresholdMs;
	TFunction<FMemoryUse()> ReadMemoryUse;
	TArray<double> FrameTimesMs;
	double ElapsedSeconds = 0.0;
	bool bLeaveOutNextFrame = false;
	TOptional<uint64> LeaveOutFirstFrameAfterEngineFrame;
};
