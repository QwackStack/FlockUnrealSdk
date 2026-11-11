# Shop

A **shop** is a named collection of **shop items**. An item has a price, a currency, and free-form `data` the game reads
however it likes; the shop's own `data` holds its web shop addresses and free-form `stats`. Buying an item credits it to a
player's **inventory**.

Catalog reads are cached and served from the offline snapshot; purchases and inventory never are — see
[Caching and money-safety](#caching-and-money-safety), which is the part of this guide worth reading even
if you skip the rest.

## Blueprint

The nodes live under *Flock | Shop*:

| Node | Returns |
|---|---|
| `Flock Get All Shops` | a page of shops (`Page`, `Limit`) |
| `Flock Get Shop By Id` | one shop |
| `Flock Get Shop By Name` | one shop, found by its dashboard name |
| `Flock Get Shop Item` | one item by its id |
| `Flock Get Shop Items` | every item in a shop (optional `Patch Id`) |
| `Flock Purchase` | the purchase result: what was bought, what it granted, and the wallet after |
| `Flock Consume Inventory Item` | the consumed entry, what it granted, and the wallet after |
| `Flock Consume One` | uses one unused entry of a shop item the player holds: the consumed entry, what it granted, the wallet after |
| `Flock Get Player Inventory` | a page of what a player owns |
| `Flock Get Owned Item Counts` | how many unused entries of each shop item the player holds, from every page |
| `Flock Owns Shop Item` | whether the player holds an unused entry of a shop item |

`Flock Purchase` also buys a real-money item, through the store connected with `Flock Connect Store`; the store's own nodes
are under *Flock | Store* (see [Real-money purchases](real-money-purchases.md#blueprint)).

For a shop screen, read the wallet once (*Flock Get Wallet*, under *Flock | Player*, see [Player data](player-data.md#the-wallet))
and the owned counts once, then ask per item: *Can Afford* (wallet, item), *Owned Count Of* (counts, item id), *Wallet
Balance Of* and *Wallet Holds* (wallet, currency code). *Purchase Balances* and *Consume Balances* read a result's wallet the
same way.

![A graph showing Flock Get Shop By Name feeding Flock Get Shop Items, with the item list driving a ForEach loop](images/shop-browse.png)

**You do not pass a player.** These act for the signed-in player. `Flock Purchase` and `Flock Get Player Inventory` keep
a `Player Id` pin in their advanced section (the little arrow) for the rarer case of naming someone else; leave it empty.

An item's free-form **Data**, and its shop's **Data → Stats**, are `FFlockJsonData` handles, not strings. Read them with
the *Get Json Int / Float / String / Bool* nodes, which take a dotted path and a fallback, and *Get Json String Array*,
which takes the path alone and returns an empty array when it cannot be read — no JSON parsing node needed.

![A Flock Purchase node with its Shop Item Id wired from a picked item, both exec pins handled, and the Purchase Result output feeding a UI update](images/shop-purchase.png)

**Break the Purchase Result to see what actually happened.** It carries:

| Member | Holds |
|---|---|
| `Inventory` | the row the player now owns (see below: it can be empty) |
| `Granted` | what the purchase handed over immediately |
| `Wallet` | the balances afterwards; a purchase or consume that reports one also writes it into the cached player data, so the next read of that template shows the new balance |
| `Status` | one of the server's purchase statuses (`FlockPurchaseStatuses`): compare it against the *Flock Purchase Status* nodes, never a typed-out string |

How an item is paid for:

- **Priced in a game currency:** bought from the player's wallet, so the purchase is paid at once and *Is Purchase Paid* is
  true.
- **Priced in real money** (`IsRealMoney`): bought through the platform's store, or a web checkout on Windows, Mac and Linux,
  once `UFlockStore` is connected (see [Real-money purchases](real-money-purchases.md)). On a phone it can then also answer
  pending, while the store waits for the payment. With no store connected, or bought for another player named by id, it is
  refused before anything is sent; a server too old to say which items cost real money refuses one with
  `ShopRealCurrencyNeedsCheckout` instead.

When a purchase is refused:

| Code | Means | What else happens |
|---|---|---|
| `ShopItemNotForSale` | The item, or its shop, is switched off (Inactive) in the dashboard; nothing is bought | The shop provider clears the shops it kept, so reading the shop again shows the item gone from its items. *Flock Get Shop Item*, which reads one item by its id, still returns it, its `Status` inactive |
| `ShopInsufficientFunds`, `ShopCurrencyNotHeld` | The wallet cannot pay | The cached player data is dropped, so the next wallet read asks the server |
| `ShopPlayerMismatch` | While a player is signed in, the `Player Id` named anyone else (the server buys only for the player signed in) | Leave `Player Id` empty |

`Inventory` can be **empty**, and that is not an error: an item that grants its contents outright — a
currency pack — creates nothing to own and reports the grant instead. Use *Has Inventory Row* before
reading it rather than checking the Id yourself.

For rewards, branch with *Is Currency Reward* rather than comparing `Type` to a typed-out `"currency"`.
The server owns that set and can add kinds; a literal that does not match reads as "not a currency
reward" and silently skips the grant.

After a schema sync you can replace the id wiring entirely: the generated `Purchase` macro takes a typed
`FlockShopItemId` dropdown instead. See [Code generation](codegen.md).

## C++

Everything lives on the shop provider. `Sdk` is the Flock subsystem, `UFlockSubsystem::Get(this)` from any actor, widget
or the game instance.

```cpp
#include "FlockSubsystem.h"
#include "Providers/FlockShopProvider.h"

// Browse.
Sdk->GetShopProvider()->GetByName(TEXT("Starter"),
    [](TFlockResult<FFlockShop> Shop)
    {
        if (!Shop.bSuccess) { return; }

        for (const FFlockShopItem& Item : Shop.Value.ShopItems)
        {
            int32 Damage = 0;
            Item.Data.TryGetInt(TEXT("damage"), Damage);   // free-form, read by dotted path
        }
    });

// Buy. ShopItemId is the shop item's Id from its shop (Item.Id above): not the item's name, and not the
// shop's id. An empty player id resolves the signed-in player.
Sdk->GetShopProvider()->Purchase(ShopItemId, FString(),
    [](TFlockResult<FFlockPurchaseResult> Bought)
    {
        if (!Bought.bSuccess)
        {
            if (Bought.Error.ErrorCode == EFlockErrorCode::ShopInsufficientFunds)
            {
                // The server declined — show the player, don't retry.
            }
            // What went wrong, then a "Fix:" line saying how to fix it.
            UE_LOG(LogTemp, Warning, TEXT("%s"), *Bought.Error.ToDisplayText());
            return;
        }

        // The row can be absent: a currency pack grants its contents and creates nothing to own.
        if (Bought.Value.HasInventoryRow()) { /* the player now owns this */ }

        for (const FFlockShopItemReward& Reward : Bought.Value.Granted)
        {
            // IsCurrency compares Type with the constant, letter for letter: the server can add kinds.
            if (Reward.IsCurrency())
            {
                // Reward.Code, Reward.Amount
            }
        }
    });

// Use something up. Grants whatever the entry carries. InventoryId is a row of the player's inventory (below).
Sdk->GetShopProvider()->Consume(InventoryId,
    [](TFlockResult<FFlockConsumeResult> Consumed) { /* Consumed.Value.Granted; .Wallet when HasWallet() */ });

// What they own, a page at a time.
Sdk->GetShopProvider()->GetPlayerInventory(/*Page*/ 1, /*Limit*/ 100,
    [](TFlockResult<FFlockPlayerInventoryPage> Owned)
    {
        if (!Owned.bSuccess) { return; }
        for (const FFlockPlayerInventory& Row : Owned.Value.Items)
        {
            // Row.Id is what Consume takes.
        }
    });
```

### A shop screen

Read the wallet and what the player holds once, then decide per item. The wallet comes from the player-data cache,
which purchases keep current; the inventory is read from the server on every call.

```cpp
// Items: the TArray<FFlockShopItem> the screen shows, read from a shop as above.
const TWeakObjectPtr<UFlockSubsystem> WeakSdk = Sdk;
Sdk->GetPlayerProvider()->GetWallet([WeakSdk, Items](TFlockResult<FFlockWallet> Wallet)
{
    if (!Wallet.bSuccess || !WeakSdk.IsValid()) { return; }
    WeakSdk->GetShopProvider()->GetOwnedItemCounts([Wallet = Wallet.Value, Items](TFlockResult<FFlockOwnedItemCounts> Owned)
    {
        if (!Owned.bSuccess) { return; }
        for (const FFlockShopItem& Item : Items)
        {
            const int32 Held = Owned.Value.CountOf(Item.Id);   // unused entries; ids match with their case
            const bool bCanBuy = Wallet.CanAfford(Item);         // false for a real-money item or one with no currency
            // A standard item the wallet sells is sold again on every purchase: offer it only while Held is 0.
        }
    });
});

// Use one potion the player holds, without reading the inventory yourself. PotionItemId is the potion's shop item Id.
Sdk->GetShopProvider()->ConsumeOne(PotionItemId,
    [](TFlockResult<FFlockConsumeResult> Used) { /* Used.Value.Granted */ });
```

`ConsumeOne` reads the inventory, uses the first unused entry of that item, and passes over one used meanwhile on another
device; any other failure stops it, so one call never uses two. It fails (`Validation`) when the player holds none, and
(`Auth`) with nobody signed in. `Owns(ShopItemId, ...)` answers whether the player holds one, reading the whole inventory;
it and `GetOwnedItemCounts` fail before sending with nobody signed in.

## Real-money items

Each item says whether it costs real money and what it is called in each store. The server sends both with the catalog,
from the game's currency config and the product ids set in the Flock dashboard. Like the rest of the catalog, they are
kept for the session, and in the offline snapshot when **Enable Offline Cache** is on, so a change made in the dashboard
shows after the shop provider's `ClearCache()` (C++ only) or on the next launch with a connection. A snapshot written by
an SDK older than 1.42.0 holds neither: read offline, its items say `IsRealMoney` false and have no product ids until one
read reaches the server.

```cpp
// ShopItemId: the item's Id, from its shop.
Sdk->GetShopProvider()->GetItem(ShopItemId,
    [](TFlockResult<FFlockShopItem> Read)
    {
        if (!Read.bSuccess || !Read.Value.IsRealMoney) { return; }

        // Paid through the platform's store or a web checkout, never from the wallet: Purchase buys it through the
        // connected UFlockStore (see real-money-purchases.md).
        FString ProductId;
        if (Read.Value.FindStoreProductId(FlockPaymentProviders::GooglePlay, ProductId))
        {
            UE_LOG(LogTemp, Display, TEXT("Sold on Google Play as %s"), *ProductId);
        }
    });
```

In Blueprint, break the item for *Is Real Money* and *Store Product Ids*, and look an id up with *Find Store Product Id*,
giving it the *Flock Payment Provider Apple App Store* or *Flock Payment Provider Google Play* node as the store.

`IsRealMoney` is true when the item's currency is one the currency config lists as real money; a server that does not
send it reads false. For such an item `Price` is in the currency's smallest unit (199 is USD 1.99), but a phone shows the
player the store's own price for the product, in their currency and with their taxes, so show that one there.
`StoreProductIds` is keyed by store name (`FlockPaymentProviders::AppleAppStore`, `FlockPaymentProviders::GooglePlay`),
holds each id exactly as set in the dashboard, and is empty for an item with no product id. Use `FindStoreProductId`
rather than the map's own `Find`: a map of strings ignores letter case, and a store does not. `Type` is one of
`FlockShopItemTypes` (the *Flock Shop Item Type* nodes): in a store, set up a `FlockShopItemTypes::CurrencyPack` or
`FlockShopItemTypes::Consumable` item as a consumable product and a `FlockShopItemTypes::Standard` item as a one-time product. Store names and item kinds are strings rather than enums, for the same
reason as a reward's `Type`.

### Redeeming a store purchase

`UFlockStore` does this for every purchase it makes (see [Real-money purchases](real-money-purchases.md)). A game whose own
store code made the purchase hands it to `RedeemStoreReceipt` (in Blueprint, **Flock Redeem Store Receipt**, with *Make App
Store Receipt* or *Make Google Play Receipt*). Flock asks the store about it and hands the item over, completing with the purchase as `Purchase` does, and the
wallet it reports goes into the cached player data. Make the receipt with `FFlockStoreReceipt::FromAppleAppStore` (the
App Store's transaction id) or `FFlockStoreReceipt::FromGooglePlay` (Google Play's product id and purchase token), each id
exactly as the store gave it.

```cpp
// ProductId and PurchaseToken: exactly as your Google Play billing code received them.
// The store's purchase is left unfinished until Flock has handed the item over.
Sdk->GetShopProvider()->RedeemStoreReceipt(ShopItemId,
    FFlockStoreReceipt::FromGooglePlay(ProductId, PurchaseToken),
    [](TFlockResult<FFlockPurchaseResult> Redeemed)
    {
        if (Redeemed.bSuccess)
        {
            // Finish the purchase in the store now. Redeemed.Value.Granted says what it handed over; .Inventory is filled
            // only when HasInventoryRow(), .Wallet only when HasWallet() (currency moved).
            return;
        }
        // Leave the store's purchase unfinished, and send the same receipt again later.
        UE_LOG(LogTemp, Warning, TEXT("%s"), *Redeemed.Error.ToDisplayText());
    });
```

- **It needs a signed-in player.** The server redeems for the player the sign-in names, so with nobody signed in the call
  fails with an `Auth` error before anything is sent, and a retry is never sent once that player signs out or another
  signs in.
- **It is safe to send again.** One store purchase makes one Flock purchase, and a repeat answers with what the first
  delivered.
  - The call itself re-sends, under the retry policy, after a timeout, a dropped connection or a 5xx, the store being
    unavailable (`PaymentProviderStoreUnavailable`) included.
  - A 5xx naming a setting someone has to fix (`PaymentProviderCredentialsUnreadable`) is not re-sent: it stays the same
    until the setting is fixed.
  - An expired token is renewed once and the redemption sent again.
- **Every refusal says why** in its own code and fix; the common ones:

  | Code | Means |
  |---|---|
  | `PaymentProviderReceiptProductMismatch` | the receipt is for another product, or the item has no product id in that store |
  | `PaymentProviderReceiptPending` | Google is still waiting for the payment |
  | `PaymentProviderReceiptBelongsToAnotherPlayer` | the purchase is another player's: started for another Flock player, or already redeemed by another player |
  | `PaymentProviderProviderNotFound` | the store is not connected under Monetization → Payments in the Flock dashboard |
  | `PaymentProviderStoreCredentialsRejected` | the store refused the game's own keys |

  The rest are in [Errors](errors.md).
- **Keep the store's purchase until it succeeds.** A failure, a setup fault in the dashboard included, keeps nothing on the
  server, so the same receipt redeems once the cause is fixed. A 2xx that names no purchase, or whose body cannot be
  read, is a failure too, and drops the cached player data. A failure after an attempt that got no answer has
  `bOutcomeUnknown` set: the item may have been handed over, and sending the same receipt again is the way to find out.
- **Ids are sent exactly as given.** An empty id, or one with spaces around it, is refused (`Validation`) rather than
  trimmed: the spaces came from wherever it was copied. The SDK does not compare the receipt's product with the item's
  `StoreProductIds`; the server does.
- **No analytics transaction is recorded**: the server records the revenue of each store purchase it confirms.

## Caching and money-safety

- **A purchase is sent again under its key, never bought twice.** Every attempt of one `Purchase` call carries the same
  `idempotency_key`, and the server answers a repeat with the first purchase, so an answer that never came (a timeout, a
  dropped connection, a 5xx) is sent again under the retry policy. Two `Purchase` calls are two purchases. This needs a
  Flock server from 2026-10-28 on: an older one ignores the key, and a purchase sent again there is charged twice.
- **"Maybe bought" is its own answer.** When any attempt got no answer saying whether it went through, the failure the call
  ends with has `bOutcomeUnknown` set, even if a later attempt was refused or the player signed out before the next one,
  and its fix says to read the inventory and wallet before offering the item again: a new `Purchase` call is a new
  purchase. A later refusal for funds (`ShopInsufficientFunds`, `ShopCurrencyNotHeld`) or sale (`ShopItemNotForSale`) is
  certain: the server checks the key first, so nothing was bought (a refusal for funds drops the cached wallet, which was
  out of date). An unknown outcome drops the cached player data, so the wallet read asks the server; it records no
  Purchased or Failed analytics transaction, and neither does a purchase cancelled while it was out. A success that names no
  purchase is unknown too. The same flag is set on `RedeemStoreReceipt`, `Consume`, `ConsumeOne` and the game commands'
  `AddGameFunds`.
- **Consuming is never sent again after an ambiguous failure.** It credits currency and carries no key, so a re-send
  would grant twice. Only a failure that proves the server never processed the request is retried; everything else is
  reported, with `bOutcomeUnknown` when no answer said whether it was used. A 2xx that names no entry fails the same way.
- **A purchase is never queued offline**, and neither is a consume. With the server unreachable a purchase is sent on each
  attempt its retry policy allows, under its key (a purchase of an item not read yet first reads it, and fails there), a
  consume once; both fail with a `Connection` error, and neither is sent again when the network comes back. There is no
  offline shopping.
- **A purchase, a consume and an inventory read act for the player signed in when they were called.** Each attempt goes
  out only while that sign-in lasts, so a retry after a sign-out or another player's sign-in is never sent as the next
  player; the call fails `Cancelled` (or, after an attempt in doubt, with that attempt's failure, marked unknown). A
  cancelled purchase or consume may have gone out before the cancel, so the cached wallet is dropped then too. Every
  call reads the token on each attempt, so the attempt after an expired token is renewed carries the new one.
- **Rewards are advertised before they are bought.** An item's `Rewards` says what buying it will grant,
  so a shop tile can show "500 gold" without a transaction. It is empty for a plain item.
- **Inventory is always fetched fresh.** It is never cached and never snapshotted, because it is what a
  player owns — stale here is worse than a round trip.
- **Catalog reads are cached and snapshot-backed.** Shops and items are memoized in-process and written to
  the offline snapshot, so a returning player with no connection still sees the store. Prices come from
  that snapshot too, so treat a displayed price as advisory until the purchase succeeds — the server is
  the authority.
- **Purchases report themselves to analytics.** Started / Purchased / Failed transactions are dispatched
  alongside the call for revenue metrics. They are fire-and-forget: a failed analytics call never fails a
  purchase, and analytics being disabled simply skips them.
- **`ClearCache()`** drops the in-process caches and the shop snapshot category, so the next read hits the
  backend. C++ only.
- **The wallet sells a standard item again on every purchase.** Only a store sells it once, so check ownership
  (`Owns`, or `GetOwnedItemCounts` for a screen) before offering a standard item the wallet pays for.

## The shop sample

The plugin's **Content/Samples/Shop** folder (turn on **Show Plugin Content** in the Content Browser's settings) holds a shop
screen made of Blueprints only, with no C++ of its own. Every Flock call in it is a node:

| Asset | What it is |
|---|---|
| `WBP_FlockShopSample` | The screen. It logs a device in (registering a device that has no account yet), reads every page of the game's shops, finds or connects the store when one of the items is sold for real money (**Flock Get Store**, **Flock Connect Store**), reads the wallet (**Flock Get Wallet**) and what the player owns (**Flock Get Owned Item Counts**), and shows a row for each item. **Buy** is **Flock Purchase** for every item; **Use** is **Flock Consume One**; **Restore purchases** and **Check store setup** are the store's nodes. Each outcome is a line under the buttons, newest first, and in the log. |
| `WBP_FlockShopSampleItem` | One item's row: its name and price (**Flock Get Store Price** for a real-money item: the store's own on a phone, Flock's through a web checkout), how many the player owns, **Buy** or why it cannot be bought now, and **Use** for a consumable the player holds. |
| `FlockShopSample` | The level. Its Level Blueprint shows the screen and the mouse cursor. |

To try it:

1. Fill in **Project Settings → Plugins → Flock SDK Settings** (API URL, API Key, Game Name and Game Version).
2. Make a shop in the Flock dashboard, under Shops, and give the player template that holds the wallet the tag `currency`.
3. For real-money items on a phone, create each item's product in Google Play Console or App Store Connect and enter its product
   id on the shop item in the Flock dashboard (see [Real-money purchases](real-money-purchases.md)). On Windows, Mac and Linux,
   connect Stripe, Tap or Moyasar under Monetization → Payments.
4. Open the `FlockShopSample` level and play it. Press **Log in (device)**: with the device id field empty it signs in with this
   device's own id; type an id to be another player (two copies of the game started from one project folder share this
   device's id). Once a player is signed in, pressing it again only says so: a second sign-in would end the first. With more than one payment
   gateway connected, type the one to use in the *Payment provider* field (`stripe`, `tap` or `moyasar`) before opening the
   shop; leave it empty for the build's own way to pay. Then press **Open the shop**.

When an item is sold for real money, **Open the shop** uses the store Flock already holds, or connects one; the rows show at
once, and again once the store answers. A connect that fails is said, and the next **Open the shop** tries again. The screen
disconnects a store it connected when it is closed (its Destruct), and leaves alone one the game connected.

A row's **Buy** reads **Owned** for a standard item the player holds, **Not enough** and the currency when the wallet cannot pay,
**Store not connected** when no store is connected, and **Not in the store** for a real-money item the store does not sell here
(no product id for it, or the store did not find the product), and **Waiting for delivery** for a real-money item Flock gave
no answer for (below); those rows cannot be pressed. While a purchase, a use, a restore or the shops' read is under way, the
rows, **Restore purchases**, **Open the shop** and **Log in (device)** are off, so a second press cannot buy twice.

Each outcome is a line in plain words followed by the SDK's own cause and fix, then the wallet and what the player owns are read
again: bought (with what it granted), waiting for its payment, the store's common reasons (the rest show the SDK's words alone),
not enough currency, and no answer from Flock (*Outcome Unknown*). What the screen does on *Outcome Unknown* depends on how the
item was paid for:

- **From the wallet:** it may have been bought, and the reads of the wallet and the inventory show whether it was.
- **For real money** (a phone's store, which has already taken the payment, or a web checkout, whose page may still be paid):
  the line says the item arrives if it was paid for, with the SDK's fix; the screen keeps the item in its *Delivery Unknown*
  list, so its row reads **Waiting for delivery**, and runs **Flock Restore Purchases** (on a phone the store hands the
  purchase back and Flock delivers it; on a web checkout it reads the player's open checkouts and watches them until they end).
  The item leaves the list when **On Store Purchase Completed** reports it, or when Restore succeeds: the store and Flock's
  server then refuse a second purchase of one still unfinished. When Restore fails, the item stays waiting for this run.

After a purchase, a use or a restore the screen reads them once more a second later (its *Refresh After Change* event): Flock's
server can answer just before it has saved the change, so a read made at once can still show the item as not owned. A game that
shows what the player owns right after buying does the same, or counts the item it just bought.
Once a store is connected, **On Store Purchase Completed** from Flock's events hub reads them again too, for a purchase paid
elsewhere. To build on it, copy the folder into your project's Content and change the copies.

## Things worth knowing

- **`Get Shop Items` takes an optional patch id.** Leaving it empty reads the current items; supplying one
  reads the catalog as of that patch, which is how a limited-time or A/B store front is served.
- **Paged reads return a page struct**, not a bare array — `Items`, `Total`, `Page`, `Limit`. Ask for the
  next page by incrementing `Page`; there is no cursor to carry.
- **An item's `data` and a shop's `stats` are author data.** The SDK never transforms their keys, so they read back
  exactly as the dashboard declares them.

---

[← Back to the README](../README.md)
