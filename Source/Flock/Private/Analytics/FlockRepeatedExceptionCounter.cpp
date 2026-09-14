// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Analytics/FlockRepeatedExceptionCounter.h"

namespace
{
	/**
	 * Frames that join the same-fault key. Enough to tell two call sites with the same message apart, few enough
	 * that a deeper frame cannot split one bug into many.
	 */
	constexpr int32 SameFaultKeyFrames = 2;

	/**
	 * A captured log line's stack starts inside the UE_LOG -> GLog -> output-device dispatch, which is the
	 * same for every error. Those frames would make every same-fault key agree, so they are skipped. In a
	 * monolithic build every frame is in one executable and nothing matches, which leaves the message to
	 * tell faults apart — a coarser same-fault key, never a wrong merge of different messages.
	 */
	bool IsDispatchFrame(const FString& Frame)
	{
		return Frame.Contains(TEXT("-Core.")) || Frame.Contains(TEXT("-Core+"));
	}

	bool IsHexDigit(TCHAR Char)
	{
		return FChar::IsDigit(Char) || (Char >= TEXT('a') && Char <= TEXT('f')) || (Char >= TEXT('A') && Char <= TEXT('F'));
	}
}

FFlockRepeatedExceptionCounter::FFlockRepeatedExceptionCounter(float InWindowSeconds, int32 InMaxTracked)
	: WindowSeconds(FMath::Max(InWindowSeconds, 0.f))
	, MaxTracked(FMath::Max(InMaxTracked, 0))
{
}

FString FFlockRepeatedExceptionCounter::MakeSameFaultKey(FName Category, const FString& Message, const FString& StackTrace)
{
	FString Normalized;
	Normalized.Reserve(Message.Len());
	int32 Index = 0;
	while (Index < Message.Len())
	{
		const TCHAR Char = Message[Index];
		// A 0x-prefixed run is one value, so its hex letters are not left behind to split the same-fault key.
		if (Char == TEXT('0') && Index + 1 < Message.Len() && (Message[Index + 1] == TEXT('x') || Message[Index + 1] == TEXT('X')))
		{
			Index += 2;
			while (Index < Message.Len() && IsHexDigit(Message[Index]))
			{
				++Index;
			}
			Normalized.AppendChar(TEXT('#'));
			continue;
		}
		if (FChar::IsDigit(Char))
		{
			while (Index < Message.Len() && FChar::IsDigit(Message[Index]))
			{
				++Index;
			}
			Normalized.AppendChar(TEXT('#'));
			continue;
		}
		Normalized.AppendChar(Char);
		++Index;
	}

	TArray<FString> Lines;
	StackTrace.ParseIntoArrayLines(Lines);
	FString Frames;
	int32 Taken = 0;
	for (const FString& Line : Lines)
	{
		const FString Frame = Line.TrimStartAndEnd();
		if (Frame.IsEmpty() || IsDispatchFrame(Frame))
		{
			continue;
		}
		Frames += Frame;
		Frames.AppendChar(TEXT('\n'));
		if (++Taken == SameFaultKeyFrames)
		{
			break;
		}
	}

	return FString::Printf(TEXT("%s|%s|%s"), *Category.ToString(), *Normalized.TrimStartAndEnd(), *Frames);
}

bool FFlockRepeatedExceptionCounter::ShouldReportNow(const FString& SameFaultKey, const FString& Message, const FString& StackTrace,
	const FFlockLogDetails& Details, double NowSeconds)
{
	if (WindowSeconds <= 0.f)
	{
		return true;
	}

	if (FWindow* Open = Windows.Find(SameFaultKey))
	{
		if (NowSeconds - Open->OpenedSeconds < WindowSeconds)
		{
			++Open->Repeats;
			Open->LastSeenSeconds = NowSeconds;
			return false;
		}
		// The window elapsed before a tick expired it. Its repeats still happened, so they are kept for the
		// next CollectFinished rather than overwritten by the window this occurrence opens.
		if (Open->Repeats > 0)
		{
			FinishedReports.Add(ToRepeatReport(SameFaultKey, *Open));
		}
		Windows.Remove(SameFaultKey);
	}

	if (Windows.Num() >= MaxTracked)
	{
		CollectFinishedInto(NowSeconds, FinishedReports);
		if (Windows.Num() >= MaxTracked)
		{
			// No room to count repeats of this one. Report it untracked rather than suppress a fault nobody has seen.
			return true;
		}
	}

	FWindow& Window = Windows.Add(SameFaultKey);
	Window.Message = Message;
	Window.Details = Details;
	Window.StackTrace = StackTrace;
	Window.OpenedSeconds = NowSeconds;
	Window.LastSeenSeconds = NowSeconds;
	return true;
}

TArray<FFlockRepeatedExceptionCounter::FRepeatReport> FFlockRepeatedExceptionCounter::CollectFinished(double NowSeconds)
{
	TArray<FRepeatReport> Out = MoveTemp(FinishedReports);
	FinishedReports.Reset();
	CollectFinishedInto(NowSeconds, Out);
	return Out;
}

TArray<FFlockRepeatedExceptionCounter::FRepeatReport> FFlockRepeatedExceptionCounter::CollectAll()
{
	TArray<FRepeatReport> Out = MoveTemp(FinishedReports);
	FinishedReports.Reset();
	for (const TPair<FString, FWindow>& Pair : Windows)
	{
		if (Pair.Value.Repeats > 0)
		{
			Out.Add(ToRepeatReport(Pair.Key, Pair.Value));
		}
	}
	Windows.Reset();
	return Out;
}

FFlockRepeatedExceptionCounter::FRepeatReport FFlockRepeatedExceptionCounter::ToRepeatReport(const FString& SameFaultKey, const FWindow& Window)
{
	FRepeatReport Summary;
	Summary.SameFaultKey = SameFaultKey;
	Summary.Details = Window.Details;
	Summary.Message = Window.Message;
	Summary.StackTrace = Window.StackTrace;
	Summary.Repeats = Window.Repeats;
	Summary.FirstSeenSeconds = Window.OpenedSeconds;
	Summary.LastSeenSeconds = Window.LastSeenSeconds;
	return Summary;
}

void FFlockRepeatedExceptionCounter::CollectFinishedInto(double NowSeconds, TArray<FRepeatReport>& Out)
{
	for (auto It = Windows.CreateIterator(); It; ++It)
	{
		if (NowSeconds - It.Value().OpenedSeconds >= WindowSeconds)
		{
			if (It.Value().Repeats > 0)
			{
				Out.Add(ToRepeatReport(It.Key(), It.Value()));
			}
			It.RemoveCurrent();
		}
	}
}
