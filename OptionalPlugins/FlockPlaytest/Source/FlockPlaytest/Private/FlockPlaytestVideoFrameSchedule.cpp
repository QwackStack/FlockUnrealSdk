// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestVideoFrameSchedule.h"

#include "FlockPlaytestSettings.h"
#include "FlockPlaytestVideoFile.h"

FFlockPlaytestVideoSettings FFlockPlaytestVideoSettings::FromProjectSettings(const UFlockPlaytestSettings& Settings)
{
	// The editor keeps these in range, but DefaultGame.ini can hold anything.
	FFlockPlaytestVideoSettings Video;
	Video.MaxVideoSize = FIntPoint(FMath::Clamp(Settings.VideoWidth, 16, 3840), FMath::Clamp(Settings.VideoHeight, 16, 2160));
	Video.FramesPerSecond = FMath::Clamp(Settings.VideoFramesPerSecond, 1, 60);
	Video.BitrateKbps = FMath::Clamp(Settings.VideoBitrateKbps, 100, 50000);
	Video.MaxSeconds = FMath::Max(0.1, static_cast<double>(Settings.MaxRecordingMinutes)) * 60.0;
	Video.MaxBytes = static_cast<int64>(FMath::Max(1, Settings.MaxRecordingSizeMb)) * 1024 * 1024;
	Video.DiskBudgetBytes = static_cast<int64>(FMath::Max(1, Settings.RecordingsDiskBudgetMb)) * 1024 * 1024;
	return Video;
}

int64 FFlockPlaytestVideoSettings::BytesToMakeRoomFor() const
{
	// Measured, the encoder went about 5% over its bitrate, so a quarter more leaves room to spare.
	const double Seconds = FMath::Max(0.0, MaxSeconds);
	const double VideoBytes = BitrateKbps * 1000.0 / 8.0 * Seconds * 1.25;
	const double HeaderBytes = FFlockPlaytestVideoFile::FileHeaderBytes + FMath::CeilToDouble(Seconds * FramesPerSecond) * FFlockPlaytestVideoFile::FrameHeaderBytes;
	return FMath::Min(MaxBytes, static_cast<int64>(FMath::CeilToDouble(VideoBytes + HeaderBytes)));
}

FIntPoint FitVideoSizeInside(FIntPoint ScreenSize, FIntPoint MaxVideoSize)
{
	if (ScreenSize.X <= 0 || ScreenSize.Y <= 0)
	{
		return FIntPoint::ZeroValue;
	}
	const double Scale = FMath::Min3(1.0, static_cast<double>(MaxVideoSize.X) / ScreenSize.X,
		static_cast<double>(MaxVideoSize.Y) / ScreenSize.Y);
	const int32 Width = FMath::Min(FMath::RoundToInt(ScreenSize.X * Scale), MaxVideoSize.X) & ~1;
	const int32 Height = FMath::Min(FMath::RoundToInt(ScreenSize.Y * Scale), MaxVideoSize.Y) & ~1;
	return FIntPoint(FMath::Max(Width, 2), FMath::Max(Height, 2));
}

FString DecideWhyVideoCannotBeRecorded(bool bBuiltWithVideo, bool bCanRender, int32 FrameGrabberLatency)
{
	if (!bBuiltWithVideo)
	{
		return TEXT("this build has no video encoder: video recording is built for 64-bit Windows only");
	}
	if (!bCanRender)
	{
		return TEXT("this process draws nothing (a dedicated server, or a run started with -nullrhi)");
	}
	if (FrameGrabberLatency != 0)
	{
		// With a latency, the engine's frame grabber waits on a frame that is never read back once capture stops.
		return FString::Printf(TEXT("the console variable framegrabber.framelatency is %d, and recording needs it at 0"), FrameGrabberLatency);
	}
	return FString();
}

FFlockPlaytestVideoFrameSchedule::FFlockPlaytestVideoFrameSchedule(int32 FramesPerSecond, double InMaxSeconds)
	: CaptureIntervalSeconds(1.0 / FMath::Max(1, FramesPerSecond))
	, MaxSeconds(InMaxSeconds)
{
}

FFlockPlaytestVideoFrameSchedule::EFrameResult FFlockPlaytestVideoFrameSchedule::AddFrame(double FrameSeconds, int64& OutTimestampMs)
{
	if (FrameSeconds <= 0.0)
	{
		return EFrameResult::Skip;
	}
	if (bLeaveOutNextFrame)
	{
		bLeaveOutNextFrame = false;
		return EFrameResult::Skip;
	}

	const double FrameStartSeconds = RecordedSeconds;
	if (FrameStartSeconds >= MaxSeconds)
	{
		return EFrameResult::ReachedLengthLimit;
	}
	RecordedSeconds += FrameSeconds;

	// The frame whose middle has reached the capture time is the one nearest it. Deciding on the frame's start instead
	// takes the frame before or the frame after as its time wobbles either side, and the video stutters.
	const double FrameMiddleSeconds = FrameStartSeconds + FrameSeconds * 0.5;
	if (FrameMiddleSeconds < NextCaptureSeconds)
	{
		return EFrameResult::Skip;
	}
	while (NextCaptureSeconds <= FrameMiddleSeconds)
	{
		NextCaptureSeconds += CaptureIntervalSeconds;
	}

	// Frames far shorter than a millisecond could share one; the video format needs every time later than the last.
	const int64 TimestampMs = FMath::RoundToInt64(FrameStartSeconds * 1000.0);
	if (TimestampMs <= LastTimestampMs)
	{
		return EFrameResult::Skip;
	}
	LastTimestampMs = TimestampMs;
	OutTimestampMs = TimestampMs;
	return EFrameResult::Capture;
}
