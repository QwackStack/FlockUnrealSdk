// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockShopModels.h"
#include "UObject/WeakObjectPtrTemplates.h"

class FFlockAnalyticsProvider;

/**
 * Shop catalog + purchase + player inventory.
 *
 * Catalog reads (all shops, shop by id/name, item by id, items by shop) are memoized in-process and
 * backed by the offline snapshot, so a second ask is free and a fetch survives an outage. Like the game
 * provider — and unlike the config provider — there is no in-flight coalescing; this mirrors the Unity
 * SDK's simpler memoize.
 *
 * A purchase is money-moving and non-idempotent: an ambiguous failure may mean the charge already
 * cleared, so it is never retried on such a failure and never queued. Player inventory changes on every
 * purchase and is therefore never cached — always a fresh fetch, so offline it fails rather than serving
 * stale ownership.
 *
 * Around a purchase the provider records a Started / Purchased / Failed analytics transaction
 * (best-effort: a missing analytics provider or a failed record never fails the purchase). It holds a
 * weak reference to the analytics provider because analytics is built conditionally and may be absent.
 *
 * Completion-lambda rule: capture shared refs / weak self / values only — never `this`. Continuations
 * that re-enter the provider pin a TWeakPtr to itself, so teardown with requests in flight is safe.
 */
class FLOCK_API FFlockShopProvider
	: public FFlockProviderBase
	, public TSharedFromThis<FFlockShopProvider>
{
public:
	FFlockShopProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
		const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
		const FString& InVersionedApiUrl, const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore,
		const FString& InGameVersionId);

	/**
	 * Wires the analytics provider used to record purchase transactions. Weak, and may be null (analytics
	 * disabled) — recording is skipped when it cannot be pinned. Set once at construction by the subsystem.
	 */
	void SetAnalyticsProvider(const TWeakPtr<FFlockAnalyticsProvider>& InAnalytics) { Analytics = InAnalytics; }

	// ── Catalog ──

	/** A page of shops. Page metadata is not part of the Shop entity, so pages cache separately from ShopsById. */
	void GetAll(int32 Page, int32 Limit, TFunction<void(TFlockResult<FFlockShopPage>)> OnComplete);
	void GetById(const FString& ShopId, TFunction<void(TFlockResult<FFlockShop>)> OnComplete);
	void GetByName(const FString& Name, TFunction<void(TFlockResult<FFlockShop>)> OnComplete);

	void GetItem(const FString& ShopItemId, TFunction<void(TFlockResult<FFlockShopItem>)> OnComplete);
	/** Items for a shop, optionally as of a patch (empty PatchId = current). */
	void GetItemsByShop(const FString& ShopId, const FString& PatchId,
		TFunction<void(TFlockResult<TArray<FFlockShopItem>>)> OnComplete);

	// ── Purchase + inventory ──

	/**
	 * Buys an item for a player (empty PlayerId = the signed-in player). Posts the purchase
	 * (non-idempotent — no retry on an ambiguous failure) and fires Started (before the post) and
	 * Purchased/Failed (after) analytics transactions fire-and-forget: they are dispatched alongside the
	 * purchase, never awaited, so a slow, failed, or absent analytics endpoint never delays or fails it.
	 */
	void Purchase(const FString& ShopItemId, const FString& PlayerId,
		TFunction<void(TFlockResult<FFlockPlayerInventory>)> OnComplete);

	/** A page of a player's owned items (empty PlayerId = the signed-in player). Never cached. */
	void GetPlayerInventory(const FString& PlayerId, int32 Page, int32 Limit,
		TFunction<void(TFlockResult<FFlockPlayerInventoryPage>)> OnComplete);

	/** Drops every in-process cache and the shop snapshot category, so the next fetch hits the backend. */
	void ClearCache();

private:
	FString MakeUrl(const FString& Path) const { return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Path); }
	TMap<FString, FString> HeadersNow() const { return Session->GetAuthHeaders(); }

	void IndexShop(const FFlockShop& Shop);
	TArray<FFlockShopItem> ResolveItems(const TArray<FString>& Ids) const;

	/**
	 * Fires one purchase-transaction record and returns immediately (fire-and-forget) — the purchase
	 * never waits on or reacts to it. A no-op when the analytics provider is absent (analytics disabled).
	 */
	void RecordPurchaseStatus(const FString& Status, const FFlockShopItem& Item);

	TSharedRef<FFlockAuthSession> Session;
	TWeakPtr<FFlockAnalyticsProvider> Analytics;
	FString VersionedApiUrl;

	// Full paginated pages cache separately (page metadata isn't part of a Shop). Every shop/item lives
	// once in ShopsById/ItemsById, keyed by id; the index maps hold ids per query.
	TMap<FString, FFlockShopPage> ShopPages;
	TMap<FString, FFlockShop> ShopsById;
	TMap<FString, FFlockShopItem> ItemsById;
	TMap<FString, FString> ShopIdByName;
	TMap<FString, TArray<FString>> ItemIdsByShop;

	static const TCHAR* const SnapshotCategory;
};
