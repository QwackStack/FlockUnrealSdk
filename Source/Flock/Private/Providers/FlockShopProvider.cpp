// Copyright 2022, Qwacks. All Rights Reserved.

#include "Providers/FlockShopProvider.h"

#include "Http/FlockEndpoints.h"
#include "Http/FlockJsonUtils.h"
#include "Providers/FlockAnalyticsProvider.h"

const TCHAR* const FFlockShopProvider::SnapshotCategory = TEXT("shop");

FFlockShopProvider::FFlockShopProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
	const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
	const FString& InVersionedApiUrl, const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore,
	const FString& InGameVersionId)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, VersionedApiUrl(InVersionedApiUrl)
{
	SetSnapshotStore(InSnapshotStore, InGameVersionId);
}

// ─────────────────────────────────── Catalog ───────────────────────────────────

void FFlockShopProvider::GetAll(int32 Page, int32 Limit, TFunction<void(TFlockResult<FFlockShopPage>)> OnComplete)
{
	const FString PageKey = FString::Printf(TEXT("all_p%d_l%d"), Page, Limit);
	if (const FFlockShopPage* Cached = ShopPages.Find(PageKey))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockShopPage>::Ok(*Cached));
		}
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FString::Printf(TEXT("%s?page=%d&limit=%d"), FlockEndpoints::Shop, Page, Limit));
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockShopProvider> WeakSelf = AsShared();

	FetchWithSnapshot<FFlockShopPage>(SnapshotCategory, PageKey,
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockShopPage>)> OnAttempt)
		{
			// GetPaged returns the template TFlockPage; copy it into the concrete, snapshot-able page.
			return ClientRef->GetPaged<FFlockShop>(Url, Headers,
				[OnAttempt](TFlockResult<TFlockPage<FFlockShop>> Result)
				{
					if (!Result.bSuccess)
					{
						OnAttempt(TFlockResult<FFlockShopPage>::Fail(Result.Error));
						return;
					}
					FFlockShopPage Page;
					Page.Items = Result.Value.Items;
					Page.Total = Result.Value.Total;
					Page.Page = Result.Value.Page;
					Page.Limit = Result.Value.Limit;
					OnAttempt(TFlockResult<FFlockShopPage>::Ok(Page));
				});
		},
		TEXT("Fetch shops"),
		[WeakSelf, PageKey, OnComplete](TFlockResult<FFlockShopPage> Result)
		{
			if (const TSharedPtr<FFlockShopProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess)
				{
					Self->ShopPages.Add(PageKey, Result.Value);
					for (const FFlockShop& Shop : Result.Value.Items)
					{
						Self->IndexShop(Shop);
					}
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

void FFlockShopProvider::GetById(const FString& ShopId, TFunction<void(TFlockResult<FFlockShop>)> OnComplete)
{
	if (!RequireNotEmpty(ShopId, TEXT("Shop ID"), OnComplete))
	{
		return;
	}
	if (const FFlockShop* Cached = ShopsById.Find(ShopId))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockShop>::Ok(*Cached));
		}
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::ShopById(ShopId));
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockShopProvider> WeakSelf = AsShared();

	FetchWithSnapshot<FFlockShop>(SnapshotCategory, FString::Printf(TEXT("shop_%s"), *ShopId),
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockShop>)> OnAttempt)
		{
			return ClientRef->GetRaw<FFlockShop>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch shop"),
		[WeakSelf, ShopId, OnComplete](TFlockResult<FFlockShop> Result)
		{
			// Keyed by the requested id (not shop.Id), so the next call with this id is a hit.
			if (const TSharedPtr<FFlockShopProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess)
				{
					Self->ShopsById.Add(ShopId, Result.Value);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

void FFlockShopProvider::GetByName(const FString& Name, TFunction<void(TFlockResult<FFlockShop>)> OnComplete)
{
	if (!RequireNotEmpty(Name, TEXT("Shop Name"), OnComplete))
	{
		return;
	}
	if (const FString* Id = ShopIdByName.Find(Name))
	{
		if (const FFlockShop* Cached = ShopsById.Find(*Id))
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<FFlockShop>::Ok(*Cached));
			}
			return;
		}
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::ShopByName(Name));
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockShopProvider> WeakSelf = AsShared();

	FetchWithSnapshot<FFlockShop>(SnapshotCategory, FString::Printf(TEXT("shop_name_%s"), *Name),
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockShop>)> OnAttempt)
		{
			return ClientRef->GetRaw<FFlockShop>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch shop by name"),
		[WeakSelf, Name, OnComplete](TFlockResult<FFlockShop> Result)
		{
			if (const TSharedPtr<FFlockShopProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess)
				{
					Self->IndexShop(Result.Value);
					if (!Result.Value.Id.IsEmpty())
					{
						Self->ShopIdByName.Add(Name, Result.Value.Id);
					}
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

void FFlockShopProvider::GetItem(const FString& ShopItemId, TFunction<void(TFlockResult<FFlockShopItem>)> OnComplete)
{
	if (!RequireNotEmpty(ShopItemId, TEXT("Shop Item ID"), OnComplete))
	{
		return;
	}
	if (const FFlockShopItem* Cached = ItemsById.Find(ShopItemId))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockShopItem>::Ok(*Cached));
		}
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::ShopItemById(ShopItemId));
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockShopProvider> WeakSelf = AsShared();

	FetchWithSnapshot<FFlockShopItem>(SnapshotCategory, FString::Printf(TEXT("item_%s"), *ShopItemId),
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockShopItem>)> OnAttempt)
		{
			return ClientRef->GetRaw<FFlockShopItem>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch shop item"),
		[WeakSelf, ShopItemId, OnComplete](TFlockResult<FFlockShopItem> Result)
		{
			if (const TSharedPtr<FFlockShopProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess)
				{
					Self->ItemsById.Add(ShopItemId, Result.Value);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

void FFlockShopProvider::GetItemsByShop(const FString& ShopId, const FString& PatchId,
	TFunction<void(TFlockResult<TArray<FFlockShopItem>>)> OnComplete)
{
	if (!RequireNotEmpty(ShopId, TEXT("Shop ID"), OnComplete))
	{
		return;
	}

	const FString CacheKey = FString::Printf(TEXT("items_shop_%s_%s"), *ShopId,
		PatchId.IsEmpty() ? TEXT("current") : *PatchId);
	if (const TArray<FString>* CachedIds = ItemIdsByShop.Find(CacheKey))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<TArray<FFlockShopItem>>::Ok(ResolveItems(*CachedIds)));
		}
		return;
	}

	FString Path = FlockEndpoints::ShopItemsByShop(ShopId);
	if (!PatchId.IsEmpty())
	{
		Path += FString::Printf(TEXT("?patch_id=%s"), *PatchId);
	}
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(Path);
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockShopProvider> WeakSelf = AsShared();

	// Enveloped list ({error,response,result:[...]}) — GetList unwraps `result` as a bare array.
	FetchListWithSnapshot<FFlockShopItem>(SnapshotCategory, CacheKey,
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<TArray<FFlockShopItem>>)> OnAttempt)
		{
			return ClientRef->GetList<FFlockShopItem>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch shop items"),
		[WeakSelf, CacheKey, OnComplete](TFlockResult<TArray<FFlockShopItem>> Result)
		{
			if (const TSharedPtr<FFlockShopProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess)
				{
					TArray<FString> Ids;
					Ids.Reserve(Result.Value.Num());
					for (const FFlockShopItem& Item : Result.Value)
					{
						if (!Item.Id.IsEmpty())
						{
							Self->ItemsById.Add(Item.Id, Item);
							Ids.Add(Item.Id);
						}
					}
					Self->ItemIdsByShop.Add(CacheKey, Ids);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

// ───────────────────────────── Purchase + inventory ────────────────────────────

void FFlockShopProvider::Purchase(const FString& ShopItemId, const FString& PlayerId,
	TFunction<void(TFlockResult<FFlockPlayerInventory>)> OnComplete)
{
	if (!RequireNotEmpty(ShopItemId, TEXT("Shop Item ID"), OnComplete))
	{
		return;
	}
	// Default to the signed-in player; callers no longer need to pass it explicitly.
	const FString ResolvedPlayerId = PlayerId.IsEmpty() ? Session->GetPlayerId() : PlayerId;
	if (!RequireNotEmpty(ResolvedPlayerId, TEXT("Player ID (sign in first)"), OnComplete))
	{
		return;
	}

	TWeakPtr<FFlockShopProvider> WeakSelf = AsShared();

	// The item is fetched first — the transaction record needs its price/currency, and a failed fetch
	// fails the purchase (mirrors the Unity flow, which awaits it).
	GetItem(ShopItemId,
		[WeakSelf, ShopItemId, ResolvedPlayerId, OnComplete](TFlockResult<FFlockShopItem> ItemResult)
		{
			const TSharedPtr<FFlockShopProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockPlayerInventory>::Fail(FFlockError::Make(
						EFlockErrorType::Cancelled, TEXT("Shop provider was destroyed"))));
				}
				return;
			}
			if (!ItemResult.bSuccess)
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockPlayerInventory>::Fail(ItemResult.Error));
				}
				return;
			}

			const FFlockShopItem Item = ItemResult.Value;

			FFlockShopTransactionRequest Request;
			Request.ShopItemId = ShopItemId;
			Request.PlayerId = ResolvedPlayerId;
			FString Body;
			if (!FFlockJsonUtils::StructToWireJson(Request, Body))
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockPlayerInventory>::Fail(FFlockError::Make(
						EFlockErrorType::Serialization, TEXT("Failed to serialize purchase request"))));
				}
				return;
			}

			// Fire the Started record and move straight on — the purchase never waits on telemetry.
			Self->RecordPurchaseStatus(TEXT("Started"), Item);

			const TSharedRef<FFlockHttpClient> ClientRef = Self->Client;
			const FString Url = Self->MakeUrl(FlockEndpoints::ShopTransaction);
			const TMap<FString, FString> Headers = Self->HeadersNow();

			// Non-idempotent (money): an ambiguous failure may mean the charge cleared, so it surfaces
			// rather than being re-sent. Bare response — PostJsonRaw reads the model at the root.
			Self->Execute<FFlockPlayerInventory>(
				[ClientRef, Url, Headers, Body](TFunction<void(TFlockResult<FFlockPlayerInventory>)> OnAttempt)
				{
					return ClientRef->PostJsonRaw<FFlockPlayerInventory>(Url, Headers, Body, MoveTemp(OnAttempt));
				},
				[WeakSelf, Item, OnComplete](TFlockResult<FFlockPlayerInventory> PurchaseResult)
				{
					// Fire the outcome record (best-effort) and deliver the result immediately — neither
					// waits on the other. If the provider is gone, the record is simply skipped.
					if (const TSharedPtr<FFlockShopProvider> Self2 = WeakSelf.Pin())
					{
						Self2->RecordPurchaseStatus(PurchaseResult.bSuccess ? TEXT("Purchased") : TEXT("Failed"), Item);
					}
					if (OnComplete)
					{
						OnComplete(PurchaseResult);
					}
				},
				TEXT("Purchase shop item"), /*bIdempotent*/ false);
		});
}

void FFlockShopProvider::GetPlayerInventory(const FString& PlayerId, int32 Page, int32 Limit,
	TFunction<void(TFlockResult<FFlockPlayerInventoryPage>)> OnComplete)
{
	const FString ResolvedPlayerId = PlayerId.IsEmpty() ? Session->GetPlayerId() : PlayerId;
	if (!RequireNotEmpty(ResolvedPlayerId, TEXT("Player ID (sign in first)"), OnComplete))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FString::Printf(TEXT("%s?page=%d&limit=%d"),
		*FlockEndpoints::PlayerInventoryByPlayer(ResolvedPlayerId), Page, Limit));
	const TMap<FString, FString> Headers = HeadersNow();

	// Inventory changes on every purchase — intentionally never cached (no dict, no snapshot); always
	// fresh, so offline it fails rather than serving stale ownership. Same rule as bans.
	Execute<FFlockPlayerInventoryPage>(
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockPlayerInventoryPage>)> OnAttempt)
		{
			return ClientRef->GetPaged<FFlockPlayerInventory>(Url, Headers,
				[OnAttempt](TFlockResult<TFlockPage<FFlockPlayerInventory>> Result)
				{
					if (!Result.bSuccess)
					{
						OnAttempt(TFlockResult<FFlockPlayerInventoryPage>::Fail(Result.Error));
						return;
					}
					FFlockPlayerInventoryPage Page;
					Page.Items = Result.Value.Items;
					Page.Total = Result.Value.Total;
					Page.Page = Result.Value.Page;
					Page.Limit = Result.Value.Limit;
					OnAttempt(TFlockResult<FFlockPlayerInventoryPage>::Ok(Page));
				});
		},
		MoveTemp(OnComplete), TEXT("Get player inventory"));
}

void FFlockShopProvider::ClearCache()
{
	ShopPages.Reset();
	ShopsById.Reset();
	ShopIdByName.Reset();
	ItemsById.Reset();
	ItemIdsByShop.Reset();
	DeleteSnapshotCategory(SnapshotCategory);
}

// ─────────────────────────────── Internals ─────────────────────────────────────

void FFlockShopProvider::IndexShop(const FFlockShop& Shop)
{
	if (!Shop.Id.IsEmpty())
	{
		ShopsById.Add(Shop.Id, Shop);
	}
}

TArray<FFlockShopItem> FFlockShopProvider::ResolveItems(const TArray<FString>& Ids) const
{
	TArray<FFlockShopItem> Result;
	Result.Reserve(Ids.Num());
	for (const FString& Id : Ids)
	{
		if (const FFlockShopItem* Item = ItemsById.Find(Id))
		{
			Result.Add(*Item);
		}
	}
	return Result;
}

void FFlockShopProvider::RecordPurchaseStatus(const FString& Status, const FFlockShopItem& Item)
{
	const TSharedPtr<FFlockAnalyticsProvider> AnalyticsPtr = Analytics.Pin();
	if (!AnalyticsPtr.IsValid())
	{
		// Analytics disabled or gone: recording is best-effort, so there is simply nothing to do.
		return;
	}

	FFlockAnalyticsTransactionRequest Request;
	Request.Amount = static_cast<double>(Item.Price);
	Request.CurrencyCode = Item.Currency;
	Request.ShopItemId = Item.Id;
	// PascalCase spellings, matching the Unity provider's enum names (nameof) — deliberately distinct
	// from the request struct's lowercase defaults.
	Request.TransactionType = TEXT("Purchase");
	Request.Status = Status;
	// PlayerId/SessionId/CreatedAt left empty — RecordTransaction fills them from the current session.

	// Fire-and-forget: no completion, so the purchase neither waits on nor reacts to the record.
	AnalyticsPtr->RecordTransaction(Request);
}
