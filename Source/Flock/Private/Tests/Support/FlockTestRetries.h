// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

#if WITH_AUTOMATION_TESTS

namespace FlockTestRetries
{
	/**
	 * Runs the retry handler's scheduled retries, which go through the core ticker, with no provider frame between.
	 *
	 * A retry is never synchronous, even at a zero delay: a test that only calls and asserts sees the first try alone, reads
	 * a working retry as an absent one, and proves nothing with a "not tried again" assertion.
	 */
	inline void Pump()
	{
		for (int32 Index = 0; Index < 8; ++Index)
		{
			FTSTicker::GetCoreTicker().Tick(1.f);
		}
	}
}

#endif
