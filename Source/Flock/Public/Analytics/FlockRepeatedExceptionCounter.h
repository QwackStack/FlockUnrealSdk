// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Models/FlockAnalyticsModels.h"

/**
 * Collapses an error storm into one report plus a count, per kind of fault and per window.
 *
 * An engine error inside a tick fires every frame, so without this a single bug fills the spool within
 * seconds and buries every other fault behind it. The first occurrence in a window is reported at once;
 * repeats inside the window are counted, and the count comes back as a summary once the window closes.
 *
 * Pure policy: no clock, no disk, no network. The caller supplies the time, which is what lets every rule
 * here be tested without waiting.
 *
 * Nothing is swallowed silently. When the table of open windows is full, an occurrence that cannot be
 * tracked is reported rather than suppressed — losing a count is acceptable, losing the only report of a
 * new fault is not.
 */
class FLOCK_API FFlockRepeatedExceptionCounter
{
public:
	/** A closed window that saw repeats: what the provider reports as the storm's summary. */
	struct FRepeatReport
	{
		FString SameFaultKey;
		FString Message;
		FString StackTrace;
		/** What the first report carried — category, source, kind — so the summary is counted alongside it. */
		FFlockLogDetails Details;
		/** Occurrences after the first, which was reported when the window opened. */
		int32 Repeats = 0;
		double FirstSeenSeconds = 0.0;
		double LastSeenSeconds = 0.0;
	};

	static constexpr int32 DefaultMaxTracked = 256;

	/** A window of 0 turns repeat counting off: every occurrence is admitted. */
	explicit FFlockRepeatedExceptionCounter(float InWindowSeconds, int32 InMaxTracked = DefaultMaxTracked);

	/**
	 * Category, the message with its digit and hex runs collapsed, and the first frames of the stack that
	 * are not the logging machinery. "Actor_12 failed at 0x7ff3" and "Actor_13 failed at 0x7ab9" are one bug.
	 */
	static FString MakeSameFaultKey(FName Category, const FString& Message, const FString& StackTrace);

	/**
	 * True when this occurrence should be reported now: the first in its window, repeat counting off, or no
	 * room left to track it. False for a repeat inside an open window, which is counted instead.
	 */
	bool ShouldReportNow(const FString& SameFaultKey, const FString& Message, const FString& StackTrace,
		const FFlockLogDetails& Details, double NowSeconds);

	/** Windows that have closed. The ones that saw repeats come back as summaries; every closed window is forgotten. */
	TArray<FRepeatReport> CollectFinished(double NowSeconds);

	/** Every window that saw repeats, however young — for shutdown, when no later tick will expire them. */
	TArray<FRepeatReport> CollectAll();

	int32 OpenWindowCount() const { return Windows.Num(); }

	float GetRepeatWindowSeconds() const { return WindowSeconds; }

private:
	struct FWindow
	{
		FString Message;
		FString StackTrace;
		FFlockLogDetails Details;
		double OpenedSeconds = 0.0;
		double LastSeenSeconds = 0.0;
		int32 Repeats = 0;
	};

	static FRepeatReport ToRepeatReport(const FString& SameFaultKey, const FWindow& Window);
	void CollectFinishedInto(double NowSeconds, TArray<FRepeatReport>& Out);

	float WindowSeconds = 0.f;
	int32 MaxTracked = DefaultMaxTracked;
	TMap<FString, FWindow> Windows;

	/** Windows replaced by a later occurrence before a tick expired them; handed out by the next CollectFinished. */
	TArray<FRepeatReport> FinishedReports;
};
