// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "ProtokitePlaytestVideoFrameSchedule.h"

#include "ProtokitePlaytestSettings.h"
#include "ProtokitePlaytestVideoFile.h"

FProtokitePlaytestVideoSettings FProtokitePlaytestVideoSettings::FromProjectSettings(const UProtokitePlaytestSettings& Settings)
{
	// The editor keeps these in range, but DefaultGame.ini can hold anything.
	FProtokitePlaytestVideoSettings Video;
	Video.MaxVideoSize = FIntPoint(FMath::Clamp(Settings.VideoWidth, 16, 3840), FMath::Clamp(Settings.VideoHeight, 16, 2160));
	Video.FramesPerSecond = FMath::Clamp(Settings.VideoFramesPerSecond, 1, 60);
	Video.BitrateKbps = FMath::Clamp(Settings.VideoBitrateKbps, 100, 50000);
	Video.MaxSeconds = FMath::Max(0.1, static_cast<double>(Settings.MaxRecordingMinutes)) * 60.0;
	Video.MaxBytes = static_cast<int64>(FMath::Max(1, Settings.MaxRecordingSizeMb)) * 1024 * 1024;
	Video.DiskBudgetBytes = static_cast<int64>(FMath::Max(1, Settings.RecordingsDiskBudgetMb)) * 1024 * 1024;
	return Video;
}

int64 FProtokitePlaytestVideoSettings::BytesToMakeRoomFor() const
{
	// Measured, the encoder went about 5% over its bitrate, so a quarter more leaves room to spare.
	const double Seconds = FMath::Max(0.0, MaxSeconds);
	const double VideoBytes = BitrateKbps * 1000.0 / 8.0 * Seconds * 1.25;
	const double HeaderBytes = FProtokitePlaytestVideoFile::FileHeaderBytes + FMath::CeilToDouble(Seconds * FramesPerSecond) * FProtokitePlaytestVideoFile::FrameHeaderBytes;
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

FIntPoint FitPictureInsideTheVideo(FIntPoint ScreenSize, FIntPoint VideoSize)
{
	FIntPoint Picture = FitVideoSizeInside(ScreenSize, VideoSize);
	if (Picture.X <= 0 || Picture.Y <= 0)
	{
		return VideoSize;
	}
	if (VideoSize.X - Picture.X < SmallestBarsPixels)
	{
		Picture.X = VideoSize.X;
	}
	if (VideoSize.Y - Picture.Y < SmallestBarsPixels)
	{
		Picture.Y = VideoSize.Y;
	}
	return Picture;
}

bool PlaceInTheMiddleOfTheFrame(const TArray<FColor>& Picture, FIntPoint PictureSize, FIntPoint FrameSize, TArray<FColor>& OutFrame)
{
	if (FrameSize.X <= 0 || FrameSize.Y <= 0)
	{
		OutFrame.Reset();
		return false;
	}
	OutFrame.SetNumUninitialized(FrameSize.X * FrameSize.Y);
	const bool bReadable = PictureSize.X > 0 && PictureSize.Y > 0 && Picture.Num() == PictureSize.X * PictureSize.Y;
	const int32 Width = bReadable ? FMath::Min(PictureSize.X, FrameSize.X) : 0;
	const int32 Height = bReadable ? FMath::Min(PictureSize.Y, FrameSize.Y) : 0;
	const int32 FromColumn = (PictureSize.X - Width) / 2;
	const int32 FromRow = (PictureSize.Y - Height) / 2;
	const int32 ToColumn = (FrameSize.X - Width) / 2;
	const int32 ToRow = (FrameSize.Y - Height) / 2;
	// Each pixel is written once: black for the bars, the picture between them.
	for (int32 Row = 0; Row < FrameSize.Y; ++Row)
	{
		FColor* Line = &OutFrame[Row * FrameSize.X];
		if (Row < ToRow || Row >= ToRow + Height)
		{
			for (int32 Column = 0; Column < FrameSize.X; ++Column)
			{
				Line[Column] = FColor::Black;
			}
			continue;
		}
		for (int32 Column = 0; Column < ToColumn; ++Column)
		{
			Line[Column] = FColor::Black;
		}
		FMemory::Memcpy(Line + ToColumn, &Picture[(FromRow + Row - ToRow) * PictureSize.X + FromColumn], Width * sizeof(FColor));
		for (int32 Column = ToColumn + Width; Column < FrameSize.X; ++Column)
		{
			Line[Column] = FColor::Black;
		}
	}
	return bReadable;
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

FProtokitePlaytestVideoFrameSchedule::FProtokitePlaytestVideoFrameSchedule(int32 FramesPerSecond, double InMaxSeconds)
	: CaptureIntervalSeconds(1.0 / FMath::Max(1, FramesPerSecond))
	, MaxSeconds(InMaxSeconds)
{
}

FProtokitePlaytestVideoFrameSchedule::EFrameResult FProtokitePlaytestVideoFrameSchedule::AddFrame(double FrameSeconds, int64& OutTimestampMs)
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
