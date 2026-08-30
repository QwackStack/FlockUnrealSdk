// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockShopModels.h"
#include "UObject/WeakObjectPtrTemplates.h"

class FFlockAnalyticsProvider;
class FFlockPlayerProvider;

/**
 * Shop catalog + purchase + player inventory.
 *
 * Catalog reads (all shops, shop by id/name, item by id, items by shop) are memoized in-process and
 * backed by the offline snapshot, so a second ask is free and a fetch survives an outage. Like the game
 * provider — and unlike the config provider — there is no in-flight coalescing: a plain memoize is
 * enough, because catalog reads are not the ones several widgets race for on the same frame.
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

	/**
	 * Wires the player provider so a purchase or consume can fold the wallet it returns into the cached
	 * player row. Weak, and may be null — the write-through is skipped when it cannot be pinned.
	 *
	 * Without this a purchase leaves the per-player data cache holding **pre-purchase balances**, and the
	 * next `GetMyDataByTemplate` serves them: the money moved on the server while the game still reads the
	 * old number. Worse, a caller that then writes that stale row back — which is exactly what a
	 * read-modify-write does — silently undoes the purchase. Found live on 2026-08-30, where the
	 * self-test's own commands sweep echoed a stale wallet back and reset the balance a purchase had
	 * just changed.
	 */
	void SetPlayerProvider(const TWeakPtr<FFlockPlayerProvider>& InPlayerProvider) { PlayerProvider = InPlayerProvider; }

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
	 *
	 * Completes with the whole purchase result — the inventory row **under `.Inventory`**, plus anything
	 * the purchase granted on the spot (`.Granted`) and the wallet afterwards (`.Wallet`). The row can be
	 * empty: an item that hands its contents over outright creates nothing to own.
	 */
	void Purchase(const FString& ShopItemId, const FString& PlayerId,
		TFunction<void(TFlockResult<FFlockPurchaseResult>)> OnComplete);

	/**
	 * Buys for the **signed-in player** — the overload nearly every call site wants.
	 *
	 * An overload rather than a defaulted parameter because the callback trails: C++ cannot default a
	 * middle argument, so without this every ordinary purchase has to spell out `FString()` to say
	 * "whoever is signed in", which reads like an oversight rather than a choice.
	 */
	void Purchase(const FString& ShopItemId, TFunction<void(TFlockResult<FFlockPurchaseResult>)> OnComplete)
	{
		Purchase(ShopItemId, FString(), MoveTemp(OnComplete));
	}

	/**
	 * Consumes an owned inventory entry, granting whatever it carries. Completes with the updated row,
	 * what was granted, and the wallet afterwards.
	 *
	 * **Money-moving, so it takes the purchase's path and not the ordinary one.** Consuming credits
	 * currency, which means a re-send after an ambiguous failure double-grants — the mirror image of a
	 * double-charge and just as unrecoverable from the client. It posts with bIdempotent=false, so only a
	 * failure that proves the request was never processed (408/429) is retried and everything else
	 * surfaces for the caller to decide about. Never queued offline, for the same reason.
	 *
	 * The route takes **no request body**; an empty JSON object goes on the wire to keep the POST
	 * well-formed. It is not gated on sign-in: the entry is addressed by its own id and the spec declares
	 * neither `security` nor an Authorization header, so a client-side gate would be this SDK inventing a
	 * rule the server never stated.
	 */
	void Consume(const FString& InventoryId, TFunction<void(TFlockResult<FFlockConsumeResult>)> OnComplete);

	/** A page of a player's owned items (empty PlayerId = the signed-in player). Never cached. */
	void GetPlayerInventory(const FString& PlayerId, int32 Page, int32 Limit,
		TFunction<void(TFlockResult<FFlockPlayerInventoryPage>)> OnComplete);

	/** The signed-in player's inventory. See the Purchase overload for why this is not a default argument. */
	void GetPlayerInventory(int32 Page, int32 Limit,
		TFunction<void(TFlockResult<FFlockPlayerInventoryPage>)> OnComplete)
	{
		GetPlayerInventory(FString(), Page, Limit, MoveTemp(OnComplete));
	}

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

	/**
	 * Folds a server-returned wallet into the cached player row. No-op when the wallet is absent (the
	 * purchase moved no currency, so there is nothing to update) or the player provider is gone.
	 * `ApplyServerPlayerData` itself ignores a player whose rows are not cached.
	 */
	void ApplyWalletToPlayerCache(const FFlockPlayerData& Wallet) const;

	TSharedRef<FFlockAuthSession> Session;
	TWeakPtr<FFlockAnalyticsProvider> Analytics;
	TWeakPtr<FFlockPlayerProvider> PlayerProvider;
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
