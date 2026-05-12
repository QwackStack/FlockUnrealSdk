// Disk-backed FIFO of wire-ready JSON request bodies.
// Flow per entry: Enqueue (writes .tmp then atomically renames to .evt) ->
// caller sends via HTTP -> on 2xx or 4xx-permanent, Remove deletes the file;
// on transient failure the entry sits on disk until the next flush trigger.
// File names are zero-padded UTC ticks + GUID so a lexical sort drains oldest-first.
//
// Non-UObject by design: per-event entries would otherwise add GC pressure,
// and disk IO doesn't need the UObject lifecycle. Owned by the subsystem via TUniquePtr.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeCounter.h"

#include <atomic>

class QWACK_UE_SDK_API FFlockEventSpool
{
public:
	FFlockEventSpool(const FString& InDirectory, int32 InMaxEvents, int32 InBatchSize);

	int32 PendingCount() const { return PendingCounter.GetValue(); }
	int32 BatchSize() const    { return Batch; }

	// Returns the handle (filename only) on success, empty FString on failure.
	FString Enqueue(const FString& WireJson);

	void Remove(const FString& Handle);
	void RemoveMany(const TArray<FString>& Handles);

	// Reads up to BatchSize() oldest entries. OutHandles[i] pairs with OutBodies[i].
	void ReadBatch(TArray<FString>& OutHandles, TArray<FString>& OutBodies);

	// CAS guard so two flushes don't drain overlapping batches.
	bool TryBeginFlush();
	void EndFlush();

	void Clear();

private:
	void TrimOldest();
	int32 SweepStaleAndCount();
	TArray<FString> EnumerateEntryFiles() const;

	FString Directory;
	int32 MaxEvents;
	int32 Batch;
	mutable FCriticalSection Mutex;
	FThreadSafeCounter PendingCounter;
	std::atomic<int32> Flushing{0};
};
