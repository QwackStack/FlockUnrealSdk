# Real-money purchases

A real-money item (`FFlockShopItem::IsRealMoney`) is paid for through the platform's own store: Google Play on Android, the
App Store on iOS. Connect `UFlockStore` once at start, and the shop provider's ordinary `Purchase` buys real-money items through
it: the store charges the player, Flock checks the purchase with the store and delivers the item, and only then is the store's
purchase finished. Items priced in a game currency are still bought from the wallet by the same call.

## Setting it up

1. **Install the Flock Platform Store plugin.** Releases carry `FlockPlatformStore-<version>.zip`. Extract it next to the Flock
   SDK, so that `Plugins/FlockPlatformStore/` sits beside `Plugins/FlockUnrealSdk/`, then enable it under **Edit → Plugins**. If
   you cloned the repository, copy or link `Plugins/FlockUnrealSdk/OptionalPlugins/FlockPlatformStore/` to
   `Plugins/FlockPlatformStore/`: Unreal does not load it where it sits. It reaches the store through the engine's online
   subsystem, which is on by default on Android and iOS.
2. **Set up the store in the project.**
   - **Android (UE 5.8 or later):** in *Project Settings > Android*, turn on **Enable Google Play Support**, enter the game's
     Google Play license key, and add the `com.android.vending.BILLING` permission. Keep `bSupportsInAppPurchasing=true`
     under `[OnlineSubsystemGooglePlay.Store]` (the engine's default). Google Play takes new builds only with a current
     Billing Library, and only UE 5.8 ships one (Billing 8); on UE 5.4 to 5.7 the store is refused at connect, and a game
     with billing code of its own redeems its purchases with `RedeemStoreReceipt` (see [Shop](shop.md#redeeming-a-store-purchase)).
     Google Play also requires a recent target SDK for new builds (36 at the time of writing).
   - **iOS:** in *Project Settings > iOS*, turn on **Enable StoreKit Support**, and add the In-App Purchase capability to
     the app.
3. **Set up the items in the Flock dashboard.** Price each item in a currency the game's currency config lists as real
   money, and enter its product id under **Store product IDs**, exactly as the store has it. In the store, make a
   `currency_pack` or `consumable` item a consumable product and a `standard` item a one-time product. Connect the store
   under **Monetization > Payments**, with the game's store keys, so Flock's server can check each purchase.

## Connecting

```cpp
#include "Store/FlockStore.h"

// Once Flock has initialized. The subsystem holds the connection (UFlockSubsystem::GetStore) until it is disconnected.
UFlockStore::Connect(Sdk, FFlockStoreOptions(), [](TFlockResult<UFlockStore*> Connected)
{
    if (!Connected.bSuccess)
    {
        UE_LOG(LogTemp, Warning, TEXT("%s"), *Connected.Error.ToDisplayText());
        return;
    }
    Connected.Value->OnPurchaseCompleted.AddLambda([](const FFlockShopItem& Item, const FFlockPurchaseResult& Result)
    {
        // Every real-money purchase Flock delivers, one paid at an earlier launch or while the game waited included:
        // refresh the wallet and the inventory here.
    });
    Connected.Value->OnPurchaseNotDelivered.AddLambda([](const FFlockUndeliveredPurchase& Purchase)
    {
        // A purchase the player paid for that Flock did not deliver. It stays unfinished in the store, which hands it
        // back at the next launch; Purchase.Error's code and Hint say why.
    });
});
```

`Connect` reads the game's shops, registers each item that has a product id for this store, and from then on sends Flock
every purchase the store hands over whenever a player is signed in. Leave `FFlockStoreOptions::PaymentProvider` empty for
this platform's own store. It is refused:

- on a platform with no store of its own (Windows, Mac, Linux): sell only items priced in a game currency there;
- when the Flock Platform Store plugin is not enabled (no store library): a phone store's rules forbid paying for digital
  goods elsewhere, so Flock never opens a web page instead;
- on Android with an engine older than UE 5.8, or with `bDisableLocalAcknowledgeAndConsume=true` under
  `[OnlineSubsystemGooglePlay.Store]` (the engine would then finish nothing Flock delivers);
- while another connection is connected: use the connected one, or `Disconnect` it first.

## Buying

```cpp
Sdk->GetShopProvider()->Purchase(ShopItemId, [](TFlockResult<FFlockPurchaseResult> Result)
{
    if (Result.bSuccess && Result.Value.Status == FlockPurchaseStatuses::Pending)
    {
        // The store waits for the payment (a slow payment method, a parent's approval). OnPurchaseCompleted follows once paid.
        return;
    }
    if (Result.bSuccess)
    {
        // Delivered: Result.Value.Granted, .Inventory and .Wallet say what was handed over.
        return;
    }
    if (Result.Error.Type == EFlockErrorType::Store)
    {
        // The store did not take the payment; nothing was charged. Result.Error.StoreFailureReason says why
        // (FlockStoreFailureReasons::Cancelled when the player closed the store's dialog).
        return;
    }
    UE_LOG(LogTemp, Warning, TEXT("%s"), *Result.Error.ToDisplayText());
});
```

- **Delivered before it is finished.** The store's purchase is finished only once Flock delivered it. A purchase Flock
  refused, or one whose answer never came, stays unfinished: the store hands it back at the next launch, where it is sent
  again once a player is signed in, and Flock answers a repeat with what it delivered the first time. Each one is sent once a
  run of the game, a connection made again included; `Restore` sends again one whose send failed earlier, and leaves one
  still being sent to its send. A purchase whose send ended with its sign-in, before it reached Flock, is sent at the next
  sign-in.
- **For the player who started it.** The purchase belongs to the sign-in it started under. When that player signs out or
  another signs in, the call ends `Cancelled`; a payment made after that is held, and delivered when the player who opened the
  store's dialog signs in again during this run of the game. After a restart the store hands it back to whoever is signed in
  then, or to `Restore`.
- **One store purchase at a time.** The store keeps one purchase dialog open at a time, so a second purchase, of any item, is
  refused (`Validation`) until the first has ended and its dialog has closed; a purchase started from inside
  `OnPurchaseCompleted` or `OnPurchaseNotDelivered` is not, since the first has ended by then. A dialog the store never
  answers keeps refusing them until the store is disconnected and connected again.
- **Checked just before the store's dialog.** An item switched off (Inactive) in the dashboard since the store connected is
  refused with `ShopItemNotForSale` before the store charges anything.
- **Refused before anything is sent:** a real-money item with no store connected, a real-money item bought for another
  player named by id, an item with no product id for this store, an item added in the dashboard since the store connected (connect again
  to sell it), and a second purchase of a product while the first is still under way.
- **What the store does not sell.** A purchase of a product no Flock item sells is left to the game, unfinished. A product
  two items sell is sent only from a purchase that names the item; one the store hands back is reported not delivered.
- **Items the shops no longer list.** The store remembers the products it sold (per game and store, between launches), so a
  paid purchase of an item since switched off or removed is still sent, and Flock delivers what was paid for or says why not.
  They are kept with **Enable Offline Cache** off too.
- **Google Play finishes by kind:** a one-time item is acknowledged (so the store account keeps it and it can be restored), a
  consumable is consumed (so it can be bought again).
- **Receipts are not tied to the player's account at the store.** The engine hashes an account id before Google sees it and
  never hands it to Apple, so Flock sends none. The server marks each such purchase unbound: a receipt taken from one device
  could be redeemed once by another Flock account.

## Prices, restore and the setup check

```cpp
UFlockStore* Store = Sdk->GetStore();

// The store's own price, in the player's currency and with their taxes: show this one rather than the item's Price.
const TFlockResult<FFlockStorePrice> Price = Store->GetPrice(ShopItemId);
if (Price.bSuccess && Price.Value.bFoundInTheStore)
{
    UE_LOG(LogTemp, Display, TEXT("%s"), *Price.Value.Text);
}

// A "Restore purchases" button: the one-time purchases this store account owns, delivered to the signed-in player.
Store->Restore([](TFlockResult<FFlockStoreRestore> Result)
{
    // Result.Value.Restored: items Flock holds for the player; NotDelivered: ones it refused, each with its reason.
});

// Before release: what is missing from the store's setup, without buying anything.
Store->CheckSetup([](TFlockResult<FFlockStoreSetupCheck> Result)
{
    // NotFoundInTheStore, MissingProductId, bServerCanCheckPurchases and ServerProblem; IsReady() when nothing needs fixing.
});
```

`CheckSetup` asks Flock's server about a made-up purchase: a store that answers it as unknown proves the game's keys work, and
a server that accepts it is reported as a problem. One restore runs at a time; a second is refused until the first completes,
and `Disconnect` ends the one under way.

## Things worth knowing

- **Use one way to buy.** The engine's own in-app purchase Blueprint nodes finish a purchase as it is made, before Flock has
  delivered it, so a purchase Flock then refuses is lost. The editor's Flock setup panel warns
  (`Flock.Store.EnginePurchaseNodes`) when a Blueprint uses them while the Flock Platform Store plugin is enabled. Keep them
  only for products no Flock shop item sells.
- **Steam, Epic and consoles** have no store provider in Flock. A Steam build may take payments only through Steam's own
  wallet.
- **A game with its own store code** can hand `UFlockStore` its own `IFlockStoreConnection` with `UFlockStore::SetStoreLibrary`
  (or `ConnectThrough`), or redeem each purchase itself with `RedeemStoreReceipt`. Its answer to `Buy` sets
  `FFlockStoreOrder::bAnswersThePurchaseDialog`; a purchase handed back any other way leaves it false.
- **Shutting down** (`ShutdownSdk`) disconnects the store, ending a purchase that waits with `Cancelled`. Connect again after
  initializing again.
- Every call is on the game thread, as the engine's online subsystem is. The connection, prices, restore and the setup check
  are C++ only in this version.
