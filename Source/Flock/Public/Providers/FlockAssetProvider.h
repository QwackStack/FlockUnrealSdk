// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Assets/FlockAssetCache.h"
#include "Assets/FlockAssetDownloader.h"
#include "Auth/FlockAuthSession.h"
#include "Http/FlockInFlight.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockAssetModels.h"

class UTexture2D;

/** Reports transfer progress. Bytes total is 0 until the server declares a Content-Length. */
DECLARE_DELEGATE_TwoParams(FFlockAssetProgress, int64 /*BytesReceived*/, int64 /*BytesTotal*/);

/**
 * Assets: the metadata index, and the bytes behind it.
 *
 * Two halves with different rules, which is the thing to hold onto when reading this file:
 *
 * 1. **Metadata** rides the normal stack — enveloped GETs through FFlockHttpClient, wrapped in the
 *    provider base's snapshot policy, so a cached index survives offline. Both routes are enveloped
 *    (`GenericResponse_list_AssetSchema__` / `GenericResponse_AssetSchema_`) and neither declares
 *    `security`, so there is no sign-in gate.
 * 2. **Bytes** ride IFlockAssetDownloader to presigned object storage: no bearer, no envelope, its own
 *    retry budget (`AssetDownloadRetryCount`), and streamed to disk rather than held in memory.
 *
 * There is no by-name route on the backend, so GetByName fetches the index and filters it — the same
 * thing canonical does, and the reason the index is worth caching at all.
 *
 * **One download path, not two.** Canonical splits `DownloadAsync<T>` (buffers into memory) from
 * `DownloadToCacheAsync` (disk only, written specifically "to avoid a full byte[] allocation"), and
 * carries the size-cap and cache-enabled checks in both. Here every download streams into a file and the
 * typed getters load from it, so preload and download are the same code and the cap is checked once.
 * With the cache disabled the stream target is a temp file deleted after conversion.
 *
 * **Presigned URLs expire.** A record served from the snapshot can carry a signature the server no
 * longer honours. Rather than expire the snapshot — the metadata is still perfectly good — a download
 * refused with 401/403 refetches that one record and retries once with the fresh URL.
 *
 * Completion-lambda rule: capture shared refs / weak self / values only — never `this`.
 */
class FLOCK_API FFlockAssetProvider
	: public FFlockProviderBase
	, public TSharedFromThis<FFlockAssetProvider>
{
public:
	FFlockAssetProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
		const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
		const FString& InVersionedApiUrl, const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore,
		const FString& InGameVersionId, const TSharedRef<IFlockAssetDownloader>& InDownloader,
		const TSharedRef<FFlockAssetCache>& InCache);

	/** Runtime knobs lifted off UFlockConfig. Set once, right after construction. */
	void Configure(bool bInCacheEnabled, float InDownloadTimeoutSeconds, int32 InDownloadRetryCount,
		int32 InMaxConcurrentDownloads);

	// ─────────────────────────────── Metadata ────────────────────────────────

	/** Every asset for this game version. Snapshot-backed, memoized, and coalesced across callers. */
	void GetAll(TFunction<void(TFlockResult<TArray<FFlockAsset>>)> OnComplete);

	/** One asset by id. Served from the index when it is already loaded. */
	void GetById(const FString& AssetId, TFunction<void(TFlockResult<FFlockAsset>)> OnComplete);

	/** One asset by name. No backend route for this — it resolves against the index. */
	void GetByName(const FString& Name, TFunction<void(TFlockResult<FFlockAsset>)> OnComplete);

	/**
	 * One asset by whichever of the two the caller happens to have. Ids win: an index hit on the id is
	 * taken first, then a name match, and only then a by-id fetch. This is what the Blueprint nodes call,
	 * so a graph needs one string pin rather than a choice it cannot make at design time.
	 */
	void GetByIdOrName(const FString& IdOrName, TFunction<void(TFlockResult<FFlockAsset>)> OnComplete);

	// ─────────────────────────────── Downloads ───────────────────────────────
	//
	// Each flavour takes either a resolved record or an id/name. All of them go through the same
	// stream-to-disk core; they differ only in what they hand back.

	/** The local file path, once the bytes are on disk. The cheapest flavour — nothing is loaded. */
	void DownloadFile(const FFlockAsset& Asset, FFlockAssetProgress OnProgress, TFunction<void(TFlockResult<FString>)> OnComplete);
	void DownloadFile(const FString& IdOrName, FFlockAssetProgress OnProgress, TFunction<void(TFlockResult<FString>)> OnComplete);

	/** The raw bytes, read back off disk after the transfer. */
	void DownloadBytes(const FFlockAsset& Asset, FFlockAssetProgress OnProgress, TFunction<void(TFlockResult<TArray<uint8>>)> OnComplete);
	void DownloadBytes(const FString& IdOrName, FFlockAssetProgress OnProgress, TFunction<void(TFlockResult<TArray<uint8>>)> OnComplete);

	/** The file decoded as UTF-8 text — JSON, CSV, dialogue tables. */
	void DownloadText(const FFlockAsset& Asset, FFlockAssetProgress OnProgress, TFunction<void(TFlockResult<FString>)> OnComplete);
	void DownloadText(const FString& IdOrName, FFlockAssetProgress OnProgress, TFunction<void(TFlockResult<FString>)> OnComplete);

	/**
	 * The file decoded into a transient UTexture2D (png/jpg/bmp/exr/tga, whatever ImageWrapper handles).
	 *
	 * The returned texture has no referencer until the caller keeps one — assign it to a UPROPERTY before
	 * yielding. Audio deliberately has no counterpart here: UE has no engine API that turns mp3 or ogg
	 * bytes into a USoundWave, and half-supporting it (PCM WAV only) would fail silently on exactly the
	 * files a CDN usually holds. Use DownloadFile and hand the path to whatever imports audio.
	 */
	void DownloadTexture(const FFlockAsset& Asset, FFlockAssetProgress OnProgress, TFunction<void(TFlockResult<UTexture2D*>)> OnComplete);
	void DownloadTexture(const FString& IdOrName, FFlockAssetProgress OnProgress, TFunction<void(TFlockResult<UTexture2D*>)> OnComplete);

	// ──────────────────────────────── Preload ────────────────────────────────

	/**
	 * Warms the cache for a set of assets, honouring the concurrency cap. Reports 0..1 completion and
	 * hands back how many landed. Individual failures do not fail the batch — a preload is best-effort by
	 * definition, and one unreachable asset should not cost the other forty.
	 */
	void Preload(const TArray<FFlockAsset>& Assets, TFunction<void(float)> OnProgress,
		TFunction<void(TFlockResult<int32>)> OnComplete);

	/** Preload everything in the index matching a predicate. */
	void PreloadWhere(TFunction<bool(const FFlockAsset&)> Predicate, TFunction<void(float)> OnProgress,
		TFunction<void(TFlockResult<int32>)> OnComplete);

	// ───────────────────────────── Cache queries ─────────────────────────────

	/** Literal on-disk presence of this exact version. Does not consult the cache-enabled setting. */
	bool IsCached(const FFlockAsset& Asset) const;

	/** Those of Assets that IsCached reports false for — the "what would a preload actually fetch" list. */
	TArray<FFlockAsset> GetUncached(const TArray<FFlockAsset>& Assets) const;

	/** The cached path for this exact version, or empty. Does not download. */
	FString GetCachedFilePath(const FFlockAsset& Asset) const;

	/** Where the binary cache lives. */
	FString GetCacheDirectory() const { return Cache->GetDirectory(); }

	/** Drops the binary cache, the in-process index, and the asset snapshot category. */
	void ClearCache();

private:
	FString MakeUrl(const FString& Path) const { return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Path); }
	TMap<FString, FString> HeadersNow() const { return Session->GetAuthHeaders(); }

	void IndexAssets(const TArray<FFlockAsset>& Assets);

	/** Fetches one record straight from the network, bypassing the index — the presigned-URL refresh. */
	void RefetchRecord(const FString& AssetId, TFunction<void(TFlockResult<FFlockAsset>)> OnComplete);

	/** Whether this asset's bytes belong in the cache: enabled, and not larger than the whole budget. */
	bool ShouldCache(const FFlockAsset& Asset) const;

	/**
	 * The single download entry point. Serves a cache hit without touching the network, otherwise
	 * registers with the coalescer and hands off to BeginTransfer.
	 *
	 * Coalescing lives *here* and not in BeginTransfer on purpose: an expired-URL retry re-enters the
	 * transfer with the same asset and therefore the same coalesce key, and a key whose waiters have not
	 * yet been served reads as "someone else is already fetching this" — the retry would return
	 * immediately and nothing would ever complete the original callers.
	 */
	void DownloadToPath(const FFlockAsset& Asset, FFlockAssetProgress OnProgress,
		TFunction<void(TFlockResult<FString>)> OnComplete);

	/**
	 * One transfer, retries included: queues behind the concurrency cap, streams to a temp file, commits
	 * it, and on a refused signature refetches the record and comes back through here once with
	 * bAllowUrlRefresh cleared. Calls OnComplete exactly once and never touches the coalescer.
	 */
	void BeginTransfer(const FFlockAsset& Asset, FFlockAssetProgress OnProgress, bool bAllowUrlRefresh,
		TFunction<void(TFlockResult<FString>)> OnComplete);

	/** Deletes a path only if it is one of our scratch files — a cached path belongs to the cache. */
	void DiscardIfTemporary(const FString& Path) const;

	/** Runs Start now if a slot is free, else queues it. Every path that runs must call ReleaseSlot. */
	void AcquireSlot(TFunction<void()> Start);
	void ReleaseSlot();

	/** Resolves an id-or-name, then runs Then with the record. Failure short-circuits to OnFailure. */
	void WithResolvedAsset(const FString& IdOrName, TFunction<void(const FFlockAsset&)> Then,
		TFunction<void(const FFlockError&)> OnFailure);

	TSharedRef<FFlockAuthSession> Session;
	FString VersionedApiUrl;
	TSharedRef<IFlockAssetDownloader> Downloader;
	TSharedRef<FFlockAssetCache> Cache;

	bool bCacheEnabled = true;
	float DownloadTimeoutSeconds = 0.f;
	int32 DownloadRetryCount = 3;
	int32 MaxConcurrentDownloads = 4;

	/** The index. Populated by GetAll and by every single-record fetch. */
	TMap<FString, FFlockAsset> AssetsById;
	bool bAllFetched = false;

	/** Coalescers: one index fetch and one transfer per asset, however many callers ask. */
	TFlockInFlight<TArray<FFlockAsset>> AllInFlight;
	TFlockInFlight<FString> DownloadsInFlight;

	int32 ActiveDownloads = 0;
	TArray<TFunction<void()>> PendingDownloads;

	static const TCHAR* const SnapshotCategory;
	static const TCHAR* const IndexKey;
};
