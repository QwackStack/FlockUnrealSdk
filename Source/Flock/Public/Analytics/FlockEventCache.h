// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Write-ahead spool for anything that must survive a crash or an offline stretch — log events now,
 * session snapshots for crash recovery next. Entries are opaque JSON payloads: the cache never
 * parses them, so one implementation serves every payload type without being templated, and the
 * caller keeps ownership of its own wire format.
 *
 * Each consumer constructs its own instance against a distinct subfolder.
 *
 * The cache deliberately does NOT send anything. Unity's equivalent owns a FlushAsync that drives
 * the sender; here the provider owns batching, retry, and the HTTP client, so the cache stays a
 * dumb store and the retry semantics live in exactly one place (FlockProviderBase).
 *
 * Handles are opaque, stable, and ordered oldest-first — that ordering is what makes eviction and
 * batching deterministic.
 *
 * Every operation is best-effort and non-throwing: a full disk or a stray permission error costs a
 * cached entry, never a crash. Enqueue answers with an empty handle when it could not store.
 */
class FLOCK_API IFlockEventCache
{
public:
	virtual ~IFlockEventCache() = default;

	/** How many entries are currently spooled. */
	virtual int32 PendingCount() const = 0;

	/** Stores a payload and returns its handle, or an empty string when it could not be stored. */
	virtual FString Enqueue(const FString& Payload) = 0;

	/** False when the handle is unknown (already sent, evicted, or never existed). */
	virtual bool Read(const FString& Handle, FString& OutPayload) const = 0;

	/** Overwrites an entry in place, keeping its position in the queue. No-op for unknown handles. */
	virtual void Replace(const FString& Handle, const FString& Payload) = 0;

	/** Drops one entry. Called after a successful send. Unknown handles are ignored. */
	virtual void Remove(const FString& Handle) = 0;

	/** Oldest-first, capped at MaxCount. Entries stay spooled until explicitly Removed. */
	virtual void PeekBatch(int32 MaxCount, TArray<FString>& OutHandles, TArray<FString>& OutPayloads) const = 0;

	/** Every handle, oldest first — the retag-after-auth pass walks this. */
	virtual TArray<FString> AllHandles() const = 0;

	/** Drops everything. Backs the "erase my analytics data" path. */
	virtual void Clear() = 0;
};
