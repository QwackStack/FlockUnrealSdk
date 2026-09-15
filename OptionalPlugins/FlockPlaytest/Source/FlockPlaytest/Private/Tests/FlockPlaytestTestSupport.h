// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "FlockPlaytestStatus.h"
#include "Misc/AutomationTest.h"

/** Fails the test with both statuses spelled out when Actual is not Expected. */
inline void ExpectPlaytestStatus(FAutomationTestBase& Test, const FString& What, EFlockPlaytestStatus Actual,
	EFlockPlaytestStatus Expected)
{
	if (Actual != Expected)
	{
		Test.AddError(FString::Printf(TEXT("%s: expected \"%s\", got \"%s\""), *What,
			*DescribePlaytestStatus(Expected), *DescribePlaytestStatus(Actual)));
	}
}

/**
 * Runs the retries that are waiting. A retry waits on the core ticker even with no delay, so a test that counts
 * attempts has to run it. Eight one-second ticks cover every retry these tests allow.
 */
inline void RunPendingPlaytestRetries()
{
	for (int32 Index = 0; Index < 8; ++Index)
	{
		FTSTicker::GetCoreTicker().Tick(1.f);
	}
}

#endif
