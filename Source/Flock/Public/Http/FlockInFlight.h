// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockResult.h"

/**
 * De-duplicates concurrent asks for one key: the first caller starts the fetch, later callers queue, and
 * the single result fans out to all. With Blueprint async nodes in play, several widgets asking for the
 * same record on screen-open is the norm, so this collapses N identical round trips into one.
 *
 * Shared by the providers that coalesce (config, player). Game-thread only, like everything in the
 * provider layer — no locking.
 */
template <typename T>
class TFlockInFlight
{
public:
	/** Registers OnComplete under Key. Returns true only for the first caller — the one that starts the fetch. */
	bool Register(const FString& Key, TFunction<void(TFlockResult<T>)> OnComplete)
	{
		TArray<TFunction<void(TFlockResult<T>)>>& Waiters = Map.FindOrAdd(Key);
		const bool bFirst = Waiters.Num() == 0;
		Waiters.Add(MoveTemp(OnComplete));
		return bFirst;
	}

	/** Delivers Result to every caller waiting on Key and clears it. */
	void Complete(const FString& Key, const TFlockResult<T>& Result)
	{
		TArray<TFunction<void(TFlockResult<T>)>> Waiters;
		Map.RemoveAndCopyValue(Key, Waiters);
		for (TFunction<void(TFlockResult<T>)>& Waiter : Waiters)
		{
			if (Waiter)
			{
				Waiter(Result);
			}
		}
	}

private:
	TMap<FString, TArray<TFunction<void(TFlockResult<T>)>>> Map;
};
