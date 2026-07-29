// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Analytics/FlockEventCache.h"

/**
 * Default IFlockEventCache: one plain-JSON file per entry under
 * `<ProjectSavedDir>/Flock/analytics/<Subfolder>/`.
 *
 * One file per entry rather than one appended log, because entries are removed individually as
 * each send succeeds — a single file would mean rewriting the whole spool per acknowledgement.
 *
 * Not encrypted, on purpose: analytics payloads are not secrets, and a readable spool is worth far
 * more when debugging a delivery problem. The auth token store is the one that needs to hide.
 *
 * Handles are the file stems and are minted so that lexicographic order is age order, which is what
 * makes oldest-first batching and eviction fall out of a plain sort. The handle list is scanned once
 * at construction and then kept in memory, so PendingCount and eviction never re-hit the disk.
 */
class FLOCK_API FFlockFileEventCache : public IFlockEventCache
{
public:
	/**
	 * Subfolder separates consumers (e.g. `log_events`, `session_ends`). An empty InRootDirectory
	 * uses DefaultRoot(). MaxEntries of 0 or less retains nothing — use the caching config flag to
	 * turn the spool off, not the cap.
	 */
	FFlockFileEventCache(const FString& Subfolder, int32 InMaxEntries, const FString& InRootDirectory = FString());

	/** `<ProjectSavedDir>/Flock/analytics`. */
	static FString DefaultRoot();

	/** The directory this cache owns. */
	const FString& GetDirectory() const { return Directory; }

	virtual int32 PendingCount() const override;
	virtual FString Enqueue(const FString& Payload) override;
	virtual bool Read(const FString& Handle, FString& OutPayload) const override;
	virtual void Replace(const FString& Handle, const FString& Payload) override;
	virtual void Remove(const FString& Handle) override;
	virtual void PeekBatch(int32 MaxCount, TArray<FString>& OutHandles, TArray<FString>& OutPayloads) override;
	virtual TArray<FString> AllHandles() const override;
	virtual void Clear() override;

	/** How many consecutive read failures an entry gets before it is surfaced as undeliverable. */
	static constexpr int32 ReadFailuresBeforeDrop = 2;

private:
	FString PathForHandle(const FString& Handle) const;
	/** Where a write lands before it is moved onto PathForHandle. */
	FString TempPathForHandle(const FString& Handle) const;
	/** Monotonic within a run, and sortable across runs, so age order survives a restart. */
	FString MakeHandle();
	/** Trims from the front until the cap is satisfied. */
	void EvictToCap();
	/**
	 * Writes via a temp file and moves it into place, so a crash mid-write cannot leave a truncated entry
	 * that the next run loads, counts against the cap, and only discards once a flush tries to parse it.
	 * Same idiom as FFlockSnapshotStore::Write.
	 */
	bool WriteAtomic(const FString& Handle, const FString& Payload) const;

	FString Directory;
	int32 MaxEntries = 0;
	/** Oldest first. */
	TArray<FString> Handles;
	/** Disambiguates entries minted inside the same millisecond. */
	uint32 Sequence = 0;
	/**
	 * Consecutive failed reads per handle, cleared by a successful read. Two strikes rather than one so a
	 * momentary lock — a virus scanner, a backup agent — costs a retry instead of a good event: a
	 * single-strike rule would trade the slot leak for silent data loss.
	 */
	TMap<FString, int32> ReadFailures;
};
