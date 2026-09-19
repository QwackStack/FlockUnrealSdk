// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "FlockPlaytestStatus.h"
#include "Misc/AutomationTest.h"

/**
 * Whether Object has a top-level member spelled exactly Name. Read from the object's own keys: its lookup, HasField
 * included, ignores letter case, and Protokite reads keys letter for letter.
 */
inline bool HasMemberSpelled(const TSharedPtr<FJsonObject>& Object, const FString& Name)
{
	if (!Object.IsValid())
	{
		return false;
	}
	// `auto` and `*Pair.Key`: the key's type differs across engines, and dereferencing gives a TCHAR pointer on every one.
	for (const auto& Pair : Object->Values)
	{
		if (Name.Equals(FString(*Pair.Key), ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

/**
 * A string member of a JSON object, or "<absent>" so a missing member never passes for an empty one. Found only under
 * exactly this spelling, so a body that misspells a key's letter case reads as not having it.
 */
inline FString StringMember(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
{
	FString Value;
	return HasMemberSpelled(Object, Name) && Object->TryGetStringField(Name, Value) ? Value : FString(TEXT("<absent>"));
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
