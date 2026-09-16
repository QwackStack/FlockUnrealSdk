// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

class UFlockPlaytestSettings;

/** The video settings one recording uses: Flock Playtest Settings' Video Recording values, kept in range. */
struct FFlockPlaytestVideoSettings
{
	FIntPoint MaxVideoSize = FIntPoint(1280, 720);
	int32 FramesPerSecond = 30;
	int32 BitrateKbps = 2000;
	double MaxSeconds = 3600.0;
	int64 MaxBytes = 1536LL * 1024 * 1024;
	/** What every recording kept on disk may take together: Recordings Disk Budget. */
	int64 DiskBudgetBytes = 4096LL * 1024 * 1024;

	/** The project's settings, with every value moved into the range the recording can use. */
	static FFlockPlaytestVideoSettings FromProjectSettings(const UFlockPlaytestSettings& Settings);

	/**
	 * The most disk the recording is expected to need: what its bitrate fills in its length limit, a quarter more for the
	 * encoder going over it, and each frame's header, never more than MaxBytes. Room is made for this much before it starts.
	 */
	int64 BytesToMakeRoomFor() const;

	/** How long each captured frame is shown, in milliseconds. */
	int64 FrameDurationMs() const { return FMath::Max<int64>(1, FMath::RoundToInt64(1000.0 / FramesPerSecond)); }
};

/**
 * The size a video of ScreenSize is recorded at: its shape kept, fitted inside MaxVideoSize, never enlarged, and each
 * side an even number of pixels, which the video format needs. (0, 0) for a screen with no size.
 */
FIntPoint FitVideoSizeInside(FIntPoint ScreenSize, FIntPoint MaxVideoSize);

/** Why this process cannot record video, or empty when it can. The same answer every time it is asked. */
FString DecideWhyVideoCannotBeRecorded(bool bBuiltWithVideo, bool bCanRender, int32 FrameGrabberLatency);

/**
 * Decides which frames a recording captures, and the time each one is shown at. Inert: it is handed each frame's time
 * and keeps no clock of its own.
 *
 * Time is the sum of the frame times handed over, so time the game is not drawing, or is in the background, is not
 * recorded. At each capture time the frame nearest it is taken, which keeps a steady rhythm when frame times wobble; a
 * game drawing fewer frames than the capture rate has every frame captured.
 */
class FFlockPlaytestVideoFrameSchedule
{
public:
	enum class EFrameResult : uint8
	{
		/** Not captured. */
		Skip,
		/** Captured, at the time handed back. */
		Capture,
		/** The recording holds as much time as it may; nothing more is captured. */
		ReachedLengthLimit,
	};

	FFlockPlaytestVideoFrameSchedule(int32 FramesPerSecond, double MaxSeconds);

	/** One frame's time. When it is captured, OutTimestampMs is when it is shown, from the start of the recording. */
	EFrameResult AddFrame(double FrameSeconds, int64& OutTimestampMs);

	/** The next frame's time is not recorded, for the frame that carries time spent in the background. */
	void LeaveOutNextFrame() { bLeaveOutNextFrame = true; }

	/** How much time the recording holds so far. */
	double GetRecordedSeconds() const { return RecordedSeconds; }

private:
	double CaptureIntervalSeconds;
	double MaxSeconds;
	double RecordedSeconds = 0.0;
	double NextCaptureSeconds = 0.0;
	int64 LastTimestampMs = -1;
	bool bLeaveOutNextFrame = false;
};
