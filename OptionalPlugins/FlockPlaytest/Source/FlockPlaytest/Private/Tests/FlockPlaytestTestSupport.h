// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

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

#endif
