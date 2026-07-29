// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Providers/FlockAssetProvider.h"

#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Http/FlockEndpoints.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

const TCHAR* const FFlockAssetProvider::SnapshotCategory = TEXT("asset");
const TCHAR* const FFlockAssetProvider::IndexKey = TEXT("asset_index");

namespace
{
	/** Scratch space for downloads that must not be cached. One file per call, so nothing races. */
	FString TempDownloadRoot()
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("Flock"), TEXT("asset_tmp"));
	}

	/**
	 * Turns a transfer outcome into the SDK's error model, so the retry handler's existing rules apply
	 * unchanged: Auth/Validation/Cancelled are never retried, Connection and 5xx are.
	 */
	FFlockError ErrorFromOutcome(const FFlockAssetDownloadOutcome& Outcome, const FString& AssetName)
	{
		switch (Outcome.Result)
		{
		case EFlockHttpResult::Cancelled:
			return FFlockError::Make(EFlockErrorType::Cancelled,
				FString::Printf(TEXT("Download of '%s' was cancelled"), *AssetName));
		case EFlockHttpResult::Timeout:
			return FFlockError::Make(EFlockErrorType::Timeout,
				FString::Printf(TEXT("Download of '%s' timed out"), *AssetName));
		case EFlockHttpResult::ConnectionError:
			return FFlockError::Make(EFlockErrorType::Connection,
				FString::Printf(TEXT("Couldn't reach storage for '%s': %s"), *AssetName, *Outcome.ErrorDetail));
		default:
			break;
		}

		const int32 Status = Outcome.StatusCode;
		EFlockErrorType Type = EFlockErrorType::Network;
		if (Status == 401 || Status == 403)
		{
			// Object storage refuses an expired signature this way; the provider reads it as "resign", not
			// as "the player is signed out" — there is no bearer on this request to be stale.
			Type = EFlockErrorType::Auth;
		}
		else if (Status == 400 || Status == 422)
		{
			Type = EFlockErrorType::Validation;
		}

		return FFlockError::Make(Type,
			FString::Printf(TEXT("Download of '%s' failed with status %d: %s"), *AssetName, Status, *Outcome.ErrorDetail),
			Status);
	}

	/** Shared tally for a preload batch; the per-asset completions all close over one of these. */
	struct FPreloadTally
	{
		int32 Completed = 0;
		int32 Succeeded = 0;
		int32 Total = 0;
	};
}

FFlockAssetProvider::FFlockAssetProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
	const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
	const FString& InVersionedApiUrl, const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore,
	const FString& InGameVersionId, const TSharedRef<IFlockAssetDownloader>& InDownloader,
	const TSharedRef<FFlockAssetCache>& InCache)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, VersionedApiUrl(InVersionedApiUrl)
	, Downloader(InDownloader)
	, Cache(InCache)
{
	SetSnapshotStore(InSnapshotStore, InGameVersionId);
	SetAuthSession(InSession);
}

void FFlockAssetProvider::Configure(bool bInCacheEnabled, float InDownloadTimeoutSeconds, int32 InDownloadRetryCount,
	int32 InMaxConcurrentDownloads)
{
	bCacheEnabled = bInCacheEnabled;
	DownloadTimeoutSeconds = InDownloadTimeoutSeconds;
	DownloadRetryCount = FMath::Max(InDownloadRetryCount, 0);
	// 0 or less would stall every download forever rather than meaning "unlimited".
	MaxConcurrentDownloads = FMath::Max(InMaxConcurrentDownloads, 1);
}

// ─────────────────────────────────── Metadata ────────────────────────────────────

void FFlockAssetProvider::GetAll(TFunction<void(TFlockResult<TArray<FFlockAsset>>)> OnComplete)
{
	if (bAllFetched)
	{
		TArray<FFlockAsset> Assets;
		AssetsById.GenerateValueArray(Assets);
		if (OnComplete)
		{
			OnComplete(TFlockResult<TArray<FFlockAsset>>::Ok(Assets));
		}
		return;
	}

	// Later callers queue behind the first; one index fetch fans out to all of them.
	if (!AllInFlight.Register(IndexKey, MoveTemp(OnComplete)))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::Asset);
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockAssetProvider> WeakSelf = AsShared();

	FetchListWithSnapshot<FFlockAsset>(SnapshotCategory, IndexKey,
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<TArray<FFlockAsset>>)> OnAttempt)
		{
			return ClientRef->GetList<FFlockAsset>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch assets"),
		[WeakSelf](TFlockResult<TArray<FFlockAsset>> Result)
		{
			const TSharedPtr<FFlockAssetProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			if (Result.bSuccess)
			{
				Self->IndexAssets(Result.Value);
				Self->bAllFetched = true;
			}
			Self->AllInFlight.Complete(IndexKey, Result);
		});
}

void FFlockAssetProvider::GetById(const FString& AssetId, TFunction<void(TFlockResult<FFlockAsset>)> OnComplete)
{
	if (!RequireNotEmpty(AssetId, TEXT("Asset ID"), OnComplete))
	{
		return;
	}
	if (const FFlockAsset* Indexed = AssetsById.Find(AssetId))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockAsset>::Ok(*Indexed));
		}
		return;
	}

	// Load the index first: it is snapshot-backed, so this is also what makes a by-id read work offline.
	// The direct route is the fallback, not the first move — it costs a round trip the index usually saves.
	TWeakPtr<FFlockAssetProvider> WeakSelf = AsShared();
	GetAll([WeakSelf, AssetId, OnComplete](TFlockResult<TArray<FFlockAsset>> /*Unused*/)
	{
		const TSharedPtr<FFlockAssetProvider> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}
		if (const FFlockAsset* Indexed = Self->AssetsById.Find(AssetId))
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<FFlockAsset>::Ok(*Indexed));
			}
			return;
		}
		Self->RefetchRecord(AssetId, OnComplete);
	});
}

void FFlockAssetProvider::GetByName(const FString& Name, TFunction<void(TFlockResult<FFlockAsset>)> OnComplete)
{
	if (!RequireNotEmpty(Name, TEXT("Asset Name"), OnComplete))
	{
		return;
	}

	// No by-name route exists on the backend, so this resolves against the index — which is exactly why
	// the index is worth caching.
	TWeakPtr<FFlockAssetProvider> WeakSelf = AsShared();
	GetAll([WeakSelf, Name, OnComplete](TFlockResult<TArray<FFlockAsset>> Result)
	{
		const TSharedPtr<FFlockAssetProvider> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}
		if (!OnComplete)
		{
			return;
		}

		for (const TPair<FString, FFlockAsset>& Pair : Self->AssetsById)
		{
			if (Pair.Value.Name == Name)
			{
				OnComplete(TFlockResult<FFlockAsset>::Ok(Pair.Value));
				return;
			}
		}

		// A failed index fetch is the more useful error to report; "not found" would blame the name for
		// what was actually a network problem.
		if (!Result.bSuccess)
		{
			OnComplete(TFlockResult<FFlockAsset>::Fail(Result.Error));
			return;
		}
		OnComplete(TFlockResult<FFlockAsset>::Fail(FFlockError::Make(EFlockErrorType::Validation,
			FString::Printf(TEXT("No asset named '%s'"), *Name))));
	});
}

void FFlockAssetProvider::GetByIdOrName(const FString& IdOrName, TFunction<void(TFlockResult<FFlockAsset>)> OnComplete)
{
	if (!RequireNotEmpty(IdOrName, TEXT("Asset"), OnComplete))
	{
		return;
	}
	if (const FFlockAsset* Indexed = AssetsById.Find(IdOrName))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockAsset>::Ok(*Indexed));
		}
		return;
	}

	TWeakPtr<FFlockAssetProvider> WeakSelf = AsShared();
	GetAll([WeakSelf, IdOrName, OnComplete](TFlockResult<TArray<FFlockAsset>> Result)
	{
		const TSharedPtr<FFlockAssetProvider> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}

		// Id first: an id is unambiguous, a name is only unique by convention.
		if (const FFlockAsset* ById = Self->AssetsById.Find(IdOrName))
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<FFlockAsset>::Ok(*ById));
			}
			return;
		}
		for (const TPair<FString, FFlockAsset>& Pair : Self->AssetsById)
		{
			if (Pair.Value.Name == IdOrName)
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockAsset>::Ok(Pair.Value));
				}
				return;
			}
		}

		if (!Result.bSuccess)
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<FFlockAsset>::Fail(Result.Error));
			}
			return;
		}
		// Not in the index under either reading — it may still be an id the list route doesn't carry.
		Self->RefetchRecord(IdOrName, OnComplete);
	});
}

void FFlockAssetProvider::RefetchRecord(const FString& AssetId, TFunction<void(TFlockResult<FFlockAsset>)> OnComplete)
{
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::AssetById(AssetId));
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockAssetProvider> WeakSelf = AsShared();

	// Straight to the network on purpose: this is both the "not in the index" fallback and the
	// expired-presigned-URL refresh, and a cached record would defeat the second one entirely.
	Execute<FFlockAsset>(
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockAsset>)> OnAttempt)
		{
			return ClientRef->Get<FFlockAsset>(Url, Headers, MoveTemp(OnAttempt));
		},
		[WeakSelf, OnComplete](TFlockResult<FFlockAsset> Result)
		{
			if (const TSharedPtr<FFlockAssetProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess)
				{
					Self->AssetsById.Add(Result.Value.Id, Result.Value);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		},
		TEXT("Fetch asset"));
}

void FFlockAssetProvider::IndexAssets(const TArray<FFlockAsset>& Assets)
{
	for (const FFlockAsset& Asset : Assets)
	{
		if (Asset.IsValid())
		{
			AssetsById.Add(Asset.Id, Asset);
		}
	}
}

// ─────────────────────────────── Download core ───────────────────────────────

void FFlockAssetProvider::AcquireSlot(TFunction<void()> Start)
{
	if (ActiveDownloads < MaxConcurrentDownloads)
	{
		++ActiveDownloads;
		Start();
		return;
	}
	PendingDownloads.Add(MoveTemp(Start));
}

void FFlockAssetProvider::ReleaseSlot()
{
	if (PendingDownloads.Num() > 0)
	{
		// Hand the slot straight to the next in line rather than decrementing and re-incrementing.
		TFunction<void()> Next = MoveTemp(PendingDownloads[0]);
		PendingDownloads.RemoveAt(0);
		Next();
		return;
	}
	ActiveDownloads = FMath::Max(ActiveDownloads - 1, 0);
}

bool FFlockAssetProvider::ShouldCache(const FFlockAsset& Asset) const
{
	// An asset larger than the whole budget would evict everything else and then itself.
	return bCacheEnabled
		&& !(Cache->GetMaxSizeBytes() > 0 && Asset.SizeBytes > Cache->GetMaxSizeBytes());
}

void FFlockAssetProvider::DownloadToPath(const FFlockAsset& Asset, FFlockAssetProgress OnProgress,
	TFunction<void(TFlockResult<FString>)> OnComplete)
{
	const FString Label = Asset.Name.IsEmpty() ? Asset.Id : Asset.Name;

	if (!Asset.IsValid())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FString>::Fail(FFlockError::Make(EFlockErrorType::Validation,
				TEXT("Asset record is empty"))));
		}
		return;
	}
	if (Asset.S3DownloadUrl.IsEmpty())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FString>::Fail(FFlockError::Make(EFlockErrorType::Validation,
				FString::Printf(TEXT("Asset '%s' has no download URL"), *Label))));
		}
		return;
	}

	const bool bCacheThis = ShouldCache(Asset);
	if (bCacheEnabled && !bCacheThis)
	{
		Logger->LogWarning(FString::Printf(
			TEXT("Asset '%s' (%lld bytes) exceeds the cache budget (%lld bytes); not caching it"),
			*Label, Asset.SizeBytes, Cache->GetMaxSizeBytes()));
	}

	if (!bCacheThis)
	{
		// Each uncached download owns a private scratch file, so there is nothing to share and nothing
		// to coalesce — two callers sharing one transfer would be handed the same path to delete.
		BeginTransfer(Asset, OnProgress, /*bAllowUrlRefresh*/ true, MoveTemp(OnComplete));
		return;
	}

	const FString Token = Asset.VersionToken();
	FString CachedPath;
	if (Cache->TryGetCachedPath(Asset.Id, Token, CachedPath))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FString>::Ok(CachedPath));
		}
		return;
	}

	const FString CoalesceKey = FString::Printf(TEXT("%s|%s"), *Asset.Id, *Token);
	if (!DownloadsInFlight.Register(CoalesceKey, MoveTemp(OnComplete)))
	{
		// Someone is already fetching this exact version; they will fan the result out to us.
		return;
	}

	TWeakPtr<FFlockAssetProvider> WeakSelf = AsShared();
	BeginTransfer(Asset, OnProgress, /*bAllowUrlRefresh*/ true,
		[WeakSelf, CoalesceKey](TFlockResult<FString> Result)
		{
			if (const TSharedPtr<FFlockAssetProvider> Self = WeakSelf.Pin())
			{
				Self->DownloadsInFlight.Complete(CoalesceKey, Result);
			}
		});
}

void FFlockAssetProvider::BeginTransfer(const FFlockAsset& Asset, FFlockAssetProgress OnProgress,
	bool bAllowUrlRefresh, TFunction<void(TFlockResult<FString>)> OnComplete)
{
	const FString Label = Asset.Name.IsEmpty() ? Asset.Id : Asset.Name;
	const FString Token = Asset.VersionToken();
	const bool bCacheThis = ShouldCache(Asset);

	const FString TempPath = bCacheThis
		? Cache->BeginWrite(Asset.Id, Token)
		: FPaths::Combine(TempDownloadRoot(), FString::Printf(TEXT("%s_%s.tmp"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits), *Token));

	const TSharedRef<IFlockAssetDownloader> DownloaderRef = Downloader;
	const FString Url = Asset.S3DownloadUrl;
	const float Timeout = DownloadTimeoutSeconds;
	TWeakPtr<FFlockAssetProvider> WeakSelf = AsShared();

	AcquireSlot([WeakSelf, DownloaderRef, Url, TempPath, Timeout, OnProgress, Asset, Token, Label,
		bCacheThis, bAllowUrlRefresh, OnComplete]()
	{
		const TSharedPtr<FFlockAssetProvider> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}

		// The retry handler drives the attempts, so a download inherits the SDK's backoff and its rule
		// that Auth/Validation/Cancelled are never retried — an expired URL must not be hammered.
		Self->Execute<FString>(
			[DownloaderRef, Url, TempPath, Timeout, OnProgress, Label](TFunction<void(TFlockResult<FString>)> OnAttempt)
			{
				return DownloaderRef->DownloadToFile(Url, TempPath, Timeout,
					[OnProgress](int64 Received, int64 Total)
					{
						OnProgress.ExecuteIfBound(Received, Total);
					},
					[OnAttempt, TempPath, Label](FFlockAssetDownloadOutcome Outcome)
					{
						const bool bOk = Outcome.Result == EFlockHttpResult::Success
							&& Outcome.StatusCode >= 200 && Outcome.StatusCode < 300;
						OnAttempt(bOk
							? TFlockResult<FString>::Ok(TempPath)
							: TFlockResult<FString>::Fail(ErrorFromOutcome(Outcome, Label)));
					});
			},
			[WeakSelf, Asset, Token, Label, TempPath, bCacheThis, bAllowUrlRefresh, OnProgress, OnComplete]
			(TFlockResult<FString> Result)
			{
				const TSharedPtr<FFlockAssetProvider> Self = WeakSelf.Pin();
				if (!Self.IsValid())
				{
					return;
				}

				if (Result.bSuccess && bCacheThis)
				{
					const FString Committed = Self->Cache->Commit(Asset.Id, Token, TempPath);
					// A failed commit still leaves usable bytes at TempPath, so serve those rather than
					// turning a successful transfer into an error.
					Result.Value = Committed.IsEmpty() ? TempPath : Committed;
				}
				else if (!Result.bSuccess)
				{
					Self->Cache->Abandon(TempPath);
				}

				Self->ReleaseSlot();

				// Storage refused the signature: the record went stale, not the asset. Refetch it and come
				// back through here once, with the refresh disarmed so this can't loop.
				const bool bRetryWithFreshUrl = !Result.bSuccess && bAllowUrlRefresh
					&& Result.Error.Type == EFlockErrorType::Auth;
				if (!bRetryWithFreshUrl)
				{
					if (OnComplete)
					{
						OnComplete(Result);
					}
					return;
				}

				Self->Logger->LogDebug(FString::Printf(
					TEXT("Download URL for '%s' was refused; refreshing the record and retrying once"), *Label));

				Self->RefetchRecord(Asset.Id,
					[WeakSelf, OnProgress, OnComplete, Result](TFlockResult<FFlockAsset> Refetched)
					{
						const TSharedPtr<FFlockAssetProvider> Inner = WeakSelf.Pin();
						if (!Inner.IsValid())
						{
							return;
						}
						if (!Refetched.bSuccess)
						{
							// Report the download failure, not the refetch's — the original is the one
							// that describes what the caller actually asked for.
							if (OnComplete)
							{
								OnComplete(Result);
							}
							return;
						}
						Inner->BeginTransfer(Refetched.Value, OnProgress, /*bAllowUrlRefresh*/ false, OnComplete);
					});
			},
			FString::Printf(TEXT("Download asset '%s'"), *Label),
			/*bIdempotent*/ true, /*MaxRetriesOverride*/ Self->DownloadRetryCount, /*bAllowAuthRetry*/ false,
			// A refusal we are about to recover from is not worth an error line.
			[bAllowUrlRefresh](const FFlockError& Error)
			{
				return bAllowUrlRefresh && Error.Type == EFlockErrorType::Auth;
			});
	});
}

void FFlockAssetProvider::WithResolvedAsset(const FString& IdOrName, TFunction<void(const FFlockAsset&)> Then,
	TFunction<void(const FFlockError&)> OnFailure)
{
	GetByIdOrName(IdOrName, [Then, OnFailure](TFlockResult<FFlockAsset> Result)
	{
		if (Result.bSuccess)
		{
			Then(Result.Value);
		}
		else
		{
			OnFailure(Result.Error);
		}
	});
}

// ───────────────────────────── Download flavours ─────────────────────────────

void FFlockAssetProvider::DownloadFile(const FFlockAsset& Asset, FFlockAssetProgress OnProgress,
	TFunction<void(TFlockResult<FString>)> OnComplete)
{
	DownloadToPath(Asset, OnProgress, MoveTemp(OnComplete));
}

void FFlockAssetProvider::DownloadFile(const FString& IdOrName, FFlockAssetProgress OnProgress,
	TFunction<void(TFlockResult<FString>)> OnComplete)
{
	TWeakPtr<FFlockAssetProvider> WeakSelf = AsShared();
	WithResolvedAsset(IdOrName,
		[WeakSelf, OnProgress, OnComplete](const FFlockAsset& Asset)
		{
			if (const TSharedPtr<FFlockAssetProvider> Self = WeakSelf.Pin())
			{
				Self->DownloadFile(Asset, OnProgress, OnComplete);
			}
		},
		[OnComplete](const FFlockError& Error)
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<FString>::Fail(Error));
			}
		});
}

void FFlockAssetProvider::DownloadBytes(const FFlockAsset& Asset, FFlockAssetProgress OnProgress,
	TFunction<void(TFlockResult<TArray<uint8>>)> OnComplete)
{
	TWeakPtr<FFlockAssetProvider> WeakSelf = AsShared();
	DownloadToPath(Asset, OnProgress,
		[WeakSelf, OnComplete](TFlockResult<FString> Result)
		{
			if (!OnComplete)
			{
				return;
			}
			if (!Result.bSuccess)
			{
				OnComplete(TFlockResult<TArray<uint8>>::Fail(Result.Error));
				return;
			}

			TArray<uint8> Bytes;
			const bool bRead = FFileHelper::LoadFileToArray(Bytes, *Result.Value);
			if (const TSharedPtr<FFlockAssetProvider> Self = WeakSelf.Pin())
			{
				Self->DiscardIfTemporary(Result.Value);
			}

			OnComplete(bRead
				? TFlockResult<TArray<uint8>>::Ok(Bytes)
				: TFlockResult<TArray<uint8>>::Fail(FFlockError::Make(EFlockErrorType::Serialization,
					FString::Printf(TEXT("Couldn't read downloaded asset from '%s'"), *Result.Value))));
		});
}

void FFlockAssetProvider::DownloadBytes(const FString& IdOrName, FFlockAssetProgress OnProgress,
	TFunction<void(TFlockResult<TArray<uint8>>)> OnComplete)
{
	TWeakPtr<FFlockAssetProvider> WeakSelf = AsShared();
	WithResolvedAsset(IdOrName,
		[WeakSelf, OnProgress, OnComplete](const FFlockAsset& Asset)
		{
			if (const TSharedPtr<FFlockAssetProvider> Self = WeakSelf.Pin())
			{
				Self->DownloadBytes(Asset, OnProgress, OnComplete);
			}
		},
		[OnComplete](const FFlockError& Error)
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<TArray<uint8>>::Fail(Error));
			}
		});
}

void FFlockAssetProvider::DownloadText(const FFlockAsset& Asset, FFlockAssetProgress OnProgress,
	TFunction<void(TFlockResult<FString>)> OnComplete)
{
	TWeakPtr<FFlockAssetProvider> WeakSelf = AsShared();
	DownloadToPath(Asset, OnProgress,
		[WeakSelf, OnComplete](TFlockResult<FString> Result)
		{
			if (!OnComplete)
			{
				return;
			}
			if (!Result.bSuccess)
			{
				OnComplete(Result);
				return;
			}

			FString Text;
			const bool bRead = FFileHelper::LoadFileToString(Text, *Result.Value);
			if (const TSharedPtr<FFlockAssetProvider> Self = WeakSelf.Pin())
			{
				Self->DiscardIfTemporary(Result.Value);
			}

			OnComplete(bRead
				? TFlockResult<FString>::Ok(Text)
				: TFlockResult<FString>::Fail(FFlockError::Make(EFlockErrorType::Serialization,
					FString::Printf(TEXT("Couldn't read downloaded asset from '%s'"), *Result.Value))));
		});
}

void FFlockAssetProvider::DownloadText(const FString& IdOrName, FFlockAssetProgress OnProgress,
	TFunction<void(TFlockResult<FString>)> OnComplete)
{
	TWeakPtr<FFlockAssetProvider> WeakSelf = AsShared();
	WithResolvedAsset(IdOrName,
		[WeakSelf, OnProgress, OnComplete](const FFlockAsset& Asset)
		{
			if (const TSharedPtr<FFlockAssetProvider> Self = WeakSelf.Pin())
			{
				Self->DownloadText(Asset, OnProgress, OnComplete);
			}
		},
		[OnComplete](const FFlockError& Error)
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<FString>::Fail(Error));
			}
		});
}

void FFlockAssetProvider::DownloadTexture(const FFlockAsset& Asset, FFlockAssetProgress OnProgress,
	TFunction<void(TFlockResult<UTexture2D*>)> OnComplete)
{
	const FString Label = Asset.Name.IsEmpty() ? Asset.Id : Asset.Name;
	TWeakPtr<FFlockAssetProvider> WeakSelf = AsShared();

	DownloadToPath(Asset, OnProgress,
		[WeakSelf, Label, OnComplete](TFlockResult<FString> Result)
		{
			if (!OnComplete)
			{
				return;
			}
			if (!Result.bSuccess)
			{
				OnComplete(TFlockResult<UTexture2D*>::Fail(Result.Error));
				return;
			}

			TArray<uint8> Bytes;
			const bool bRead = FFileHelper::LoadFileToArray(Bytes, *Result.Value);
			if (const TSharedPtr<FFlockAssetProvider> Self = WeakSelf.Pin())
			{
				Self->DiscardIfTemporary(Result.Value);
			}
			if (!bRead)
			{
				OnComplete(TFlockResult<UTexture2D*>::Fail(FFlockError::Make(EFlockErrorType::Serialization,
					FString::Printf(TEXT("Couldn't read downloaded asset from '%s'"), *Result.Value))));
				return;
			}

			// Decodes and creates the texture on the game thread. Large images will hitch; that is the
			// known cost of using the engine's own importer rather than shipping a decode path.
			UTexture2D* Texture = FImageUtils::ImportBufferAsTexture2D(Bytes);
			OnComplete(Texture
				? TFlockResult<UTexture2D*>::Ok(Texture)
				: TFlockResult<UTexture2D*>::Fail(FFlockError::Make(EFlockErrorType::Serialization,
					FString::Printf(TEXT("Asset '%s' isn't an image format this engine can decode"), *Label))));
		});
}

void FFlockAssetProvider::DownloadTexture(const FString& IdOrName, FFlockAssetProgress OnProgress,
	TFunction<void(TFlockResult<UTexture2D*>)> OnComplete)
{
	TWeakPtr<FFlockAssetProvider> WeakSelf = AsShared();
	WithResolvedAsset(IdOrName,
		[WeakSelf, OnProgress, OnComplete](const FFlockAsset& Asset)
		{
			if (const TSharedPtr<FFlockAssetProvider> Self = WeakSelf.Pin())
			{
				Self->DownloadTexture(Asset, OnProgress, OnComplete);
			}
		},
		[OnComplete](const FFlockError& Error)
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<UTexture2D*>::Fail(Error));
			}
		});
}

// ──────────────────────────────────── Preload ────────────────────────────────────

void FFlockAssetProvider::Preload(const TArray<FFlockAsset>& Assets, TFunction<void(float)> OnProgress,
	TFunction<void(TFlockResult<int32>)> OnComplete)
{
	if (!bCacheEnabled)
	{
		// Preloading into a cache that isn't there would download every asset and throw it away.
		Logger->LogWarning(TEXT("Preload skipped: the asset cache is disabled"));
		if (OnProgress)
		{
			OnProgress(1.f);
		}
		if (OnComplete)
		{
			OnComplete(TFlockResult<int32>::Ok(0));
		}
		return;
	}

	if (Assets.Num() == 0)
	{
		if (OnProgress)
		{
			OnProgress(1.f);
		}
		if (OnComplete)
		{
			OnComplete(TFlockResult<int32>::Ok(0));
		}
		return;
	}

	const TSharedRef<FPreloadTally> Tally = MakeShared<FPreloadTally>();
	Tally->Total = Assets.Num();

	for (const FFlockAsset& Asset : Assets)
	{
		DownloadToPath(Asset, FFlockAssetProgress(),
			[Tally, OnProgress, OnComplete](TFlockResult<FString> Result)
			{
				++Tally->Completed;
				if (Result.bSuccess)
				{
					++Tally->Succeeded;
				}
				if (OnProgress)
				{
					OnProgress(static_cast<float>(Tally->Completed) / static_cast<float>(Tally->Total));
				}
				if (Tally->Completed >= Tally->Total && OnComplete)
				{
					// Best-effort by definition: one unreachable asset must not fail the other forty.
					OnComplete(TFlockResult<int32>::Ok(Tally->Succeeded));
				}
			});
	}
}

void FFlockAssetProvider::PreloadWhere(TFunction<bool(const FFlockAsset&)> Predicate, TFunction<void(float)> OnProgress,
	TFunction<void(TFlockResult<int32>)> OnComplete)
{
	if (!Predicate)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<int32>::Fail(FFlockError::Make(EFlockErrorType::Validation,
				TEXT("Preload predicate cannot be null"))));
		}
		return;
	}

	TWeakPtr<FFlockAssetProvider> WeakSelf = AsShared();
	GetAll([WeakSelf, Predicate, OnProgress, OnComplete](TFlockResult<TArray<FFlockAsset>> Result)
	{
		const TSharedPtr<FFlockAssetProvider> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}
		if (!Result.bSuccess)
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<int32>::Fail(Result.Error));
			}
			return;
		}

		TArray<FFlockAsset> Targets;
		for (const FFlockAsset& Asset : Result.Value)
		{
			if (Predicate(Asset))
			{
				Targets.Add(Asset);
			}
		}
		Self->Preload(Targets, OnProgress, OnComplete);
	});
}

// ───────────────────────────────── Cache queries ─────────────────────────────────

bool FFlockAssetProvider::IsCached(const FFlockAsset& Asset) const
{
	return Asset.IsValid() && Cache->Contains(Asset.Id, Asset.VersionToken());
}

TArray<FFlockAsset> FFlockAssetProvider::GetUncached(const TArray<FFlockAsset>& Assets) const
{
	TArray<FFlockAsset> Result;
	for (const FFlockAsset& Asset : Assets)
	{
		if (!IsCached(Asset))
		{
			Result.Add(Asset);
		}
	}
	return Result;
}

FString FFlockAssetProvider::GetCachedFilePath(const FFlockAsset& Asset) const
{
	if (!IsCached(Asset))
	{
		return FString();
	}
	return Cache->GetFinalPath(Asset.Id, Asset.VersionToken());
}

void FFlockAssetProvider::ClearCache()
{
	Cache->Clear();
	AssetsById.Reset();
	bAllFetched = false;
	DeleteSnapshotCategory(SnapshotCategory);
}

void FFlockAssetProvider::DiscardIfTemporary(const FString& Path) const
{
	// Only scratch files are ours to delete. A cached path belongs to the cache, and a caller who asked
	// for DownloadFile with caching off owns theirs.
	if (Path.StartsWith(TempDownloadRoot()))
	{
		IFileManager::Get().Delete(*Path, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
	}
}
