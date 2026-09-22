// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "ProtokitePlaytestPerformanceTimeline.h"

#include "Algo/BinarySearch.h"
#include "HAL/PlatformMemory.h"
#include "Performance/EnginePerformanceTargets.h"

namespace
{
	constexpr uint64 BytesPerMegabyte = 1024 * 1024;
}

FProtokitePlaytestPerformanceTimeline::FProtokitePlaytestPerformanceTimeline(TFunction<double()> InReadHitchThresholdMs,
	TFunction<FMemoryUse()> InReadMemoryUse)
	: ReadHitchThresholdMs(MoveTemp(InReadHitchThresholdMs))
	, ReadMemoryUse(MoveTemp(InReadMemoryUse))
{
	if (!ReadHitchThresholdMs)
	{
		ReadHitchThresholdMs = []()
		{
			return static_cast<double>(FEnginePerformanceTargets::GetHitchFrameTimeThresholdMS());
		};
	}
	if (!ReadMemoryUse)
	{
		ReadMemoryUse = []()
		{
			const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
			FMemoryUse Use;
			Use.UsedBytes = Stats.UsedPhysical;
			Use.PeakUsedBytes = Stats.PeakUsedPhysical;
			return Use;
		};
	}
}

bool FProtokitePlaytestPerformanceTimeline::AddFrame(double FrameSeconds, uint64 EngineFrameNumber, FProtokitePlaytestPerformanceWindow& OutWindow)
{
	if (FrameSeconds <= 0.0)
	{
		return false;
	}
	const bool bFirstFrameAfterTheMarkedEngineFrame = LeaveOutFirstFrameAfterEngineFrame.IsSet()
		&& EngineFrameNumber > LeaveOutFirstFrameAfterEngineFrame.GetValue();
	if (bFirstFrameAfterTheMarkedEngineFrame || bLeaveOutNextFrame)
	{
		if (bFirstFrameAfterTheMarkedEngineFrame)
		{
			LeaveOutFirstFrameAfterEngineFrame.Reset();
		}
		bLeaveOutNextFrame = false;
		return false;
	}

	FrameTimesMs.Add(FrameSeconds * 1000.0);
	ElapsedSeconds += FrameSeconds;
	if (ElapsedSeconds < WindowSeconds)
	{
		return false;
	}

	FrameTimesMs.Sort();
	OutWindow = FProtokitePlaytestPerformanceWindow();
	OutWindow.Seconds = ElapsedSeconds;
	OutWindow.Frames = FrameTimesMs.Num();
	OutWindow.MedianFrameTimeMs = Percentile(FrameTimesMs, 50.0);
	OutWindow.FrameTime95thPercentileMs = Percentile(FrameTimesMs, 95.0);
	OutWindow.FrameTime99thPercentileMs = Percentile(FrameTimesMs, 99.0);

	// A frame that took exactly the threshold is a hitch.
	OutWindow.HitchThresholdMs = ReadHitchThresholdMs();
	OutWindow.Hitches = FrameTimesMs.Num() - Algo::LowerBound(FrameTimesMs, OutWindow.HitchThresholdMs);

	const FMemoryUse Memory = ReadMemoryUse();
	OutWindow.MemoryUsedMb = static_cast<int64>(Memory.UsedBytes / BytesPerMegabyte);
	OutWindow.MemoryPeakMb = static_cast<int64>(Memory.PeakUsedBytes / BytesPerMegabyte);

	FrameTimesMs.Reset();
	ElapsedSeconds = 0.0;
	return true;
}

void FProtokitePlaytestPerformanceTimeline::Reset()
{
	FrameTimesMs.Reset();
	ElapsedSeconds = 0.0;
	bLeaveOutNextFrame = false;
	LeaveOutFirstFrameAfterEngineFrame.Reset();
}

double FProtokitePlaytestPerformanceTimeline::Percentile(const TArray<double>& SortedFrameTimesMs, double Percent)
{
	const int32 Count = SortedFrameTimesMs.Num();
	if (Count == 0)
	{
		return 0.0;
	}
	const int64 Rank = static_cast<int64>(FMath::CeilToDouble(Percent * Count / 100.0));
	return SortedFrameTimesMs[static_cast<int32>(FMath::Clamp<int64>(Rank - 1, 0, Count - 1))];
}
