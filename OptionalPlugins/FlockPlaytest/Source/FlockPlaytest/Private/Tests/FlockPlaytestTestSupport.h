// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "FlockPlaytestStatus.h"
#include "Misc/AutomationTest.h"

/** A string member of a JSON object, or "<absent>" so a missing member never passes for an empty one. */
inline FString StringMember(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
{
	FString Value;
	return Object.IsValid() && Object->TryGetStringField(Name, Value) ? Value : FString(TEXT("<absent>"));
}

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
