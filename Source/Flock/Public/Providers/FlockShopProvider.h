// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockShopModels.h"
#include "UObject/WeakObjectPtrTemplates.h"

class FFlockAnalyticsProvider;
class FFlockPlayerProvider;
class FJsonObject;
class UFlockStore;

/**
 * Shop catalog + purchase + player inventory.
 *
 * Catalog reads (all shops, shop by id/name, item by id, items by shop) are memoized in-process and
 * backed by the offline snapshot, so a second ask is free and a fetch survives an outage. Like the game
 * provider — and unlike the config provider — there is no in-flight coalescing: a plain memoize is
 * enough, because catalog reads are not the ones several widgets race for on the same frame.
 *
 * A wallet purchase carries one key per call, so an answer that never came is sent again under that key and the server
 * makes one purchase of it; redeeming a store's purchase is re-sent too, since the server makes one purchase per store
 * transaction. A consume is never re-sent after an ambiguous failure, and nothing is queued offline. Any of them that
 * failed with no answer saying whether it was done has bOutcomeUnknown set. Player inventory changes on every
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

	/**
	 * The store a real-money purchase goes through (UFlockStore sets itself once connected, and clears itself when it
	 * disconnects); null when none is connected, and a real-money item is then refused before anything is sent.
	 */
	void SetConnectedStore(UFlockStore* Store);

	/** The store set by SetConnectedStore, or null. */
	UFlockStore* GetConnectedStore() const;

	/**
	 * Whether ShopItemId is still sold in shop ShopId, read from the server now rather than from what the provider kept: the
	 * store asks this just before its purchase dialog. An item or shop switched off reads false, and drops the shops kept.
	 */
	void IsForSale(const FString& ShopId, const FString& ShopItemId, TFunction<void(TFlockResult<bool>)> OnComplete);

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
	 * Buys an item for a player (empty PlayerId = the signed-in player). An item priced in real money (IsRealMoney) is bought
	 * for the signed-in player through the connected UFlockStore (see UFlockStore::Purchase), and refused before anything is
	 * sent when no store is connected or another player is named. Every other item is bought from the wallet, as follows.
	 * Every attempt of one call carries
	 * the same `idempotency_key`, so an answer that never came (a timeout, a dropped connection, a 5xx) is sent again under
	 * the retry policy and the server answers it with the first purchase: never bought twice. Needs a backend that reads the
	 * key (from 2026-10-28 on); an older one ignores it, and a purchase sent again there is charged twice.
	 *
	 * Once any attempt failed with no answer saying whether it was bought, the failure the call ends with has
	 * bOutcomeUnknown set and a fix saying to read the inventory and wallet first (a new Purchase call is a new purchase),
	 * unless a later refusal under the same key proves nothing was bought (not enough funds, a currency not held, the item
	 * no longer for sale). A cancel after such an attempt ends with that attempt's failure, marked unknown. A 2xx that names
	 * no purchase is unknown too. An unknown outcome drops the cached player data, so the wallet read goes to the server.
	 *
	 * Fires Started (before the post) and Purchased/Failed (after) analytics transactions fire-and-forget: they are
	 * dispatched alongside the purchase, never awaited, so a slow, failed, or absent analytics endpoint never delays or
	 * fails it. A purchase whose outcome is unknown records neither.
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
	 * Redeems a purchase a store made for a shop item, as the signed-in player: Flock asks the store and hands the item
	 * over. Completes with the purchase, as Purchase does, and writes the wallet it reports into the cached player data.
	 *
	 * **Safe to send again**, unlike a wallet purchase: one store purchase makes one Flock purchase, and a repeat answers
	 * with what the first delivered. So a dropped connection, a timeout or a 5xx is re-sent under the retry policy, the
	 * store being unavailable included, except a 5xx naming a setting someone has to fix (the game's payment keys
	 * unreadable). Every 4xx is final but 408 and 429, which say the request was not processed. An expired token is
	 * renewed once and the redemption sent again. Leave the store's purchase unfinished until this succeeds. A failure after
	 * an attempt that got no answer has bOutcomeUnknown set: send the same receipt again later.
	 *
	 * Needs a signed-in player (the body names none: the server redeems for the player the sign-in names), and a retry is
	 * never sent once that player signs out or another signs in. The ids are sent exactly as given; an empty one, or one
	 * with spaces around it, is refused before anything is sent. A 2xx that names no purchase fails the call.
	 */
	void RedeemStoreReceipt(const FString& ShopItemId, const FFlockStoreReceipt& Receipt,
		TFunction<void(TFlockResult<FFlockPurchaseResult>)> OnComplete);

	/**
	 * Consumes an owned inventory entry, granting whatever it carries. Completes with the updated row,
	 * what was granted, and the wallet afterwards.
	 *
	 * **Money-moving, so it takes the purchase's path and not the ordinary one.** Consuming credits
	 * currency, which means a re-send after an ambiguous failure double-grants — the mirror image of a
	 * double-charge and just as unrecoverable from the client. It posts with bIdempotent=false, so only a
	 * failure that proves the request was never processed (408/429) is retried and everything else
	 * surfaces for the caller to decide about. Never queued offline, for the same reason. A failure with no answer saying
	 * whether it was used has bOutcomeUnknown set, as does a 2xx that names no entry: read the inventory before using it again.
	 *
	 * The route takes **no request body**; an empty JSON object goes on the wire to keep the POST
	 * well-formed. It acts for the player whose token it carries, so it is sent only while the sign-in current at the call
	 * lasts (never as the player who signed in next); a call made signed out is sent without a token, as the route allows.
	 */
	void Consume(const FString& InventoryId, TFunction<void(TFlockResult<FFlockConsumeResult>)> OnComplete);

	/**
	 * Uses up one unused entry of a shop item the signed-in player holds, granting what it holds (a consumable's rewards; a
	 * standard item just becomes used). Reads the inventory to find one, then consumes the first; an entry used meanwhile by
	 * another call or device (PlayerInventoryAlreadyUsed) is passed over for the next, and any other failure stops, so one
	 * call never uses two. Refused (Validation) when the player holds none, and (Auth) when nobody is signed in. The sign-in
	 * is taken before the read, so nothing is used as a player who signed in meanwhile.
	 */
	void ConsumeOne(const FString& ShopItemId, TFunction<void(TFlockResult<FFlockConsumeResult>)> OnComplete);

	/**
	 * How many unused entries of each shop item the signed-in player holds, from every page of the inventory; used and
	 * refunded entries do not count. The inventory is never cached, so each call reads it afresh: for a screen of items,
	 * read this once.
	 */
	void GetOwnedItemCounts(TFunction<void(TFlockResult<FFlockOwnedItemCounts>)> OnComplete);

	/** Whether the signed-in player holds an unused entry of this shop item, such as a standard item bought and not used up. Reads the whole inventory. */
	void Owns(const FString& ShopItemId, TFunction<void(TFlockResult<bool>)> OnComplete);

	/**
	 * A page of a player's owned items (empty PlayerId = the signed-in player). Never cached. Sent only while the sign-in
	 * current at the call lasts.
	 */
	void GetPlayerInventory(const FString& PlayerId, int32 Page, int32 Limit,
		TFunction<void(TFlockResult<FFlockPlayerInventoryPage>)> OnComplete);

	/** The signed-in player's inventory. See the Purchase overload for why this is not a default argument. */
	void GetPlayerInventory(int32 Page, int32 Limit,
		TFunction<void(TFlockResult<FFlockPlayerInventoryPage>)> OnComplete)
	{
		GetPlayerInventory(FString(), Page, Limit, MoveTemp(OnComplete));
	}

	/**
	 * Drops every in-process cache and the shop snapshot category, so the next fetch hits the backend. A read already on
	 * its way hands its answer to its caller but keeps nothing in memory: it may have been answered before what made the
	 * clear. A purchase refused because its item is switched off (ShopItemNotForSale) calls this before its caller is told.
	 */
	void ClearCache();

private:
	/**
	 * The one owner of "a catalog read keeps its answer only when no clear came while it was out": true when none
	 * did; otherwise the read's snapshot entry, written before its completion runs, is deleted and nothing is kept.
	 */
	bool KeepsWhatItRead(int32 DropsWhenSent, const FString& SnapshotKey);

	FString MakeUrl(const FString& Path) const { return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Path); }

	/** This provider's money-call rules: the cached player data to drop when an outcome is unsure, and the call's fix then. */
	FMoneyCall MoneyCall(const FString& FixWhenUnknown, TFunction<bool(const FFlockError&)> ProvesNoAttemptDidIt = nullptr) const;

	/** A purchase refusal the server makes only after finding no earlier purchase under the same key. */
	static bool IsARefusalAfterTheKey(const FFlockError& Refused);

	/** Drops the cached player data, so the next wallet read asks the server. No-op without a player provider. */
	void ForgetCachedPlayerData() const;

	/**
	 * Consume, for the sign-in taken by the caller (ConsumeOne takes it before reading the inventory). bUsedMeanwhileIsExpected
	 * logs a refusal for an entry already used at debug level: ConsumeOne passes over it.
	 */
	void ConsumeEntry(const FString& InventoryId, int32 SignInNumber, TFunction<void(TFlockResult<FFlockConsumeResult>)> OnComplete,
		bool bUsedMeanwhileIsExpected);

	/** Consumes the first of InventoryIds from Index on, passing over one already used, stopping on any other failure. */
	static void ConsumeFirstOf(const TWeakPtr<FFlockShopProvider>& WeakSelf, const TSharedRef<TArray<FString>>& InventoryIds,
		int32 Index, int32 SignInNumber, TFunction<void(TFlockResult<FFlockConsumeResult>)> OnComplete);

	/** One page of a player's inventory, sent only while SignInNumber lasts. */
	void ReadInventoryPage(const FString& PlayerId, int32 SignInNumber, int32 Page, int32 Limit,
		TFunction<void(TFlockResult<FFlockPlayerInventoryPage>)> OnComplete);

	/** Ids match letter for letter, as the server's do. */
	template <typename ValueType>
	struct FExactIdKeyFuncs : TDefaultMapKeyFuncs<FString, ValueType, /*bInAllowDuplicateKeys*/ false>
	{
		static bool Matches(const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); }
		static uint32 GetKeyHash(const FString& Key) { return FCrc::StrCrc32(*Key); }
	};

	/** The unused entries read so far, and every entry id read, so an entry handed back on two pages counts once. */
	struct FUnusedEntries
	{
		TArray<FFlockPlayerInventory> Entries;
		TMap<FString, bool, FDefaultSetAllocator, FExactIdKeyFuncs<bool>> Ids;
	};

	/** The player's unused entries from every page of the inventory, in the server's order. */
	void ReadUnusedEntries(const FString& PlayerId, int32 SignInNumber,
		TFunction<void(TFlockResult<TArray<FFlockPlayerInventory>>)> OnComplete);
	static void ReadUnusedEntriesFrom(const TWeakPtr<FFlockShopProvider>& WeakSelf, const FString& PlayerId, int32 SignInNumber,
		int32 Page, const TSharedRef<FUnusedEntries>& Unused,
		TFunction<void(TFlockResult<TArray<FFlockPlayerInventory>>)> OnComplete);

	/** The inventory page size the owned counts and ConsumeOne read with: the route's largest. */
	static constexpr int32 InventoryPageSize = 100;

	/** The receipt's ids as the route takes them, each exactly as the store issued it; false (and OnComplete told) when one is not. */
	static bool ReceiptIds(const FFlockStoreReceipt& Receipt, TSharedRef<FJsonObject>& OutIds,
		const TFunction<void(TFlockResult<FFlockPurchaseResult>)>& OnComplete);

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
	TWeakObjectPtr<UFlockStore> ConnectedStore;
	FString VersionedApiUrl;

	// Full paginated pages cache separately (page metadata isn't part of a Shop). Every shop/item lives
	// once in ShopsById/ItemsById, keyed by id; the index maps hold ids per query.
	TMap<FString, FFlockShopPage> ShopPages;
	TMap<FString, FFlockShop> ShopsById;
	TMap<FString, FFlockShopItem> ItemsById;
	TMap<FString, FString> ShopIdByName;
	TMap<FString, TArray<FString>> ItemIdsByShop;

	/** Counts the clears; a read keeps its answer in memory only when no clear came between its sending and its answer. */
	int32 CatalogDrops = 0;

	static const TCHAR* const SnapshotCategory;
};
