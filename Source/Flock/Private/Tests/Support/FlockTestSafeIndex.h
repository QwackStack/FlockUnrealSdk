// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

/**
 * Element at Index, or a default-constructed value when the index is out of range.
 *
 * Automation tests routinely assert a collection's size and then index into it. When a regression
 * empties that collection, the size assertion fails (correct) and the very next line trips UE's
 * array bounds check (not correct) — which aborts the whole automation run, so one broken feature
 * reports as "12 tests passed" instead of one clear failure. That happened for real while
 * mutation-testing the log sink.
 *
 * Reading through FlockTestAt keeps a failing test a failing test: the size assertion reports the
 * real problem, the follow-up assertions report mismatches against the default, and the remaining
 * tests still run.
 */
template <typename T>
const T& FlockTestAt(const TArray<T>& Array, int32 Index)
{
	static const T Fallback{};
	return Array.IsValidIndex(Index) ? Array[Index] : Fallback;
}

#endif // WITH_AUTOMATION_TESTS
