// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockLogger.h"

/**
 * The binary half of the asset cache: one file per (asset, version) under a directory the SDK owns.
 *
 * Deliberately *not* the snapshot store. That one holds JSON payloads under Saved/, is unbudgeted, and
 * round-trips through a versioned envelope — none of which suits a 200 MB binary. This one has no
 * envelope (the file *is* the payload), carries an LRU byte budget, and defaults under
 * FPaths::ProjectPersistentDownloadDir(), the engine's designated home for downloaded content and the
 * only location reliably writable on console and mobile targets. Saved/ is not somewhere to put a
 * multi-hundred-megabyte cache on a packaged build.
 *
 * Writes are temp-then-move through FFlockTemporaryFiles, the same convention the snapshot store and the
 * file event cache use, for the same reason: a crash mid-write must not leave a truncated file that later
 * reads as a valid cache hit. Each download streams into a temporary file of its own, and temporary files
 * older than a minute are deleted at construction; a fresh one may be another launch's download.
 *
 * Keyed by asset id + version token, so a re-uploaded asset lands beside its predecessor rather than
 * over it, and the old copy is dropped only once the new one has committed.
 */
class FLOCK_API FFlockAssetCache
{
public:
	/** InDirectory empty = the default under the project's persistent download dir. InMaxSizeMB 0 = unbudgeted. */
	FFlockAssetCache(const FString& InDirectory, int32 InMaxSizeMB, const TSharedRef<IFlockLogger>& InLogger);

	const FString& GetDirectory() const { return Directory; }

	/** 0 when unbudgeted. */
	int64 GetMaxSizeBytes() const { return MaxSizeBytes; }

	/**
	 * True when this exact version is on disk, and stamps its access time so the LRU sweep sees it as
	 * recently used. Non-const because of that stamp — a lookup is what "recently used" means here.
	 */
	bool TryGetCachedPath(const FString& AssetId, const FString& VersionToken, FString& OutPath);

	/**
	 * Literal on-disk presence, without stamping. This is what the `IsCached` / `GetUncached` queries use:
	 * asking whether something is cached must not change which entry the budget evicts next, or a preload
	 * status screen listing every asset would reorder the whole cache as a side effect of drawing itself.
	 */
	bool Contains(const FString& AssetId, const FString& VersionToken) const;

	/** Where this version lives once committed. Does not touch the filesystem. */
	FString GetFinalPath(const FString& AssetId, const FString& VersionToken) const;

	/** A temporary file of its own for one download to stream into. Creates the cache directory as a side effect. */
	FString BeginWrite(const FString& AssetId, const FString& VersionToken);

	/**
	 * Moves a completed temp file into place, drops the asset's other versions, and enforces the budget.
	 * Returns the final path, or empty when the file was not kept (the caller still has usable bytes at TempPath): the move
	 * failed, or the download is larger than the whole budget, which only a file whose size the record left out can be.
	 */
	FString Commit(const FString& AssetId, const FString& VersionToken, const FString& TempPath);

	/**
	 * A file of its own for a download that is not kept (caching off), in a folder of this cache's, so it is as writable as
	 * the cache on every platform. Files a launch left there are deleted by a later one.
	 */
	FString MakeUnkeptPath(const FString& VersionToken) const;

	/** Whether Path is a download this cache does not keep (a temporary file under it), which the SDK deletes once read. */
	bool IsUnkeptDownload(const FString& Path) const;

	/** Deletes an abandoned temp file. Safe to call on a path that was never created. */
	void Abandon(const FString& TempPath) const;

	/** Removes the whole cache directory. */
	void Clear();

	/** Total bytes currently committed. Diagnostics and tests. */
	int64 GetTotalSizeBytes() const;

private:
	void DeleteOtherVersions(const FString& AssetId, const FString& KeepPath) const;

	/** Evicts least-recently-used files, never KeepPath, until the total fits the budget. No-op when unbudgeted. */
	void EnforceMaxSize(const FString& KeepPath);

	/** Whitelists filename-safe characters so an asset id can never escape the cache directory. */
	static FString Sanitize(const FString& In);

	FString Directory;
	/** The subfolder downloads that are not kept are written to. */
	static constexpr const TCHAR* UnkeptFolder = TEXT("unkept");
	int64 MaxSizeBytes = 0;
	TSharedRef<IFlockLogger> Logger;
};
