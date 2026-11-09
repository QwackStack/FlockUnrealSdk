# Real-money purchases

A real-money item (`FFlockShopItem::IsRealMoney`) is paid for through the platform's own way to pay: the store on a phone
(Google Play on Android, the App Store on iOS), and a web checkout with the game's payment gateway on Windows, Mac and Linux
(see [Web checkout](#web-checkout-windows-mac-and-linux)). Connect `UFlockStore` once at start, and the shop provider's ordinary
`Purchase` buys real-money items through it. On a phone the store charges the player, Flock checks the purchase with the store
and delivers the item, and only then is the store's purchase finished. Items priced in a game currency are still bought from
the wallet by the same call.

## Setting it up

1. **Nothing to install.** The store is reached by Flock's own `FlockPlatformStore` module, through the engine's online
   subsystem: Flock turns the engine's Online Subsystem plugin on with it, and the platform's store subsystem (Google Play,
   iOS) is on by default on its platform.
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
this platform's own way to pay: on Windows, Mac and Linux that is a web checkout (below). It is refused:

- for a payment gateway named on a phone: a phone store's rules forbid paying for digital goods on a web page;
- when Flock's store module is not loaded (no store library, for example with the engine's Online Subsystem plugin turned
  off): a phone store's rules forbid paying for digital goods elsewhere, so Flock never opens a web page instead;
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
a server that accepts it is reported as a problem. On a phone's store one restore runs at a time; a second is refused until the
first completes, and `Disconnect` ends the one under way.

## Web checkout (Windows, Mac and Linux)

On Windows, Mac and Linux, `Connect` connects a web checkout through the game's payment gateway (Stripe, Tap or Moyasar).
Leave `PaymentProvider` empty for the game's one active gateway, or name `FlockPaymentProviders::Stripe`, `Tap` or `Moyasar`
when it has several. `IsWebCheckout()` is true. A console is refused, as its rules forbid paying for digital goods on a web
page, and so is a gateway named on a phone.

**Setting it up** needs only an item priced in a currency the game's currency config lists as real money, and a payment
gateway connected with its keys under **Monetization > Payments** in the Flock dashboard. No store product id, and nothing in
the project settings.

```cpp
FFlockStoreOptions Options;
Options.ReturnUrl = TEXT("https://your-game.example/thanks");    // where the browser lands after paying; empty: the server's own page
Options.CancelUrl = TEXT("https://your-game.example/cancelled"); // after giving up; empty: ReturnUrl
UFlockStore::Connect(Sdk, Options, [](TFlockResult<UFlockStore*> Connected) { /* as above */ });
```

- **Buying:** the same `Purchase` call reads the item (so one added or repriced in the dashboard since the connect is sold as it
  is now), opens a checkout, opens the gateway's payment page in the player's browser, and reads the purchase about every 3
  seconds (`CheckEverySeconds`; each wait varies by a quarter either way) until Flock's server reports it. Once paid it
  completes with the purchase's `PurchaseId`, `ItemType`, `Status` and `Granted` (and `OnPurchaseCompleted` once); it carries
  no `Inventory` or `Wallet`, so read those afterwards. A payment the gateway declines fails `Store` with
  `FlockStoreFailureReasons::PaymentDeclined`. A page the player closes without paying keeps the call waiting until the
  checkout expires after its hour, which fails `Store` with `Expired`; show a way to stop waiting in the game if you need
  one. Flock's server delivers the item itself, so `OnPurchaseNotDelivered` is never raised here.
- **When the reads stop on a failure** (the player's sign-in refused, a proxy in the way, a checkout the server did not keep),
  the call fails with `bOutcomeUnknown` set, since the page may still be paid: read the open checkouts before offering it
  again.
- **Never opened twice.** A checkout is never sent again after an answer that did not come: that failure has
  `bOutcomeUnknown` set; read the player's open checkouts (`GetPurchases(FlockPurchaseStatuses::Pending, ...)`) before
  offering it again. One purchase of an item runs at a time.
- **Checkouts left open and still unpaid** (the game closed, another device, a call whose player signed out) are found at each
  sign-in and by `Restore`, read until they end, and reported through `OnPurchaseCompleted` once paid. One paid while nothing
  read it is delivered by Flock but not reported: refresh the wallet and inventory after each sign-in.
- **Another way to show the page:** set `FFlockStoreOptions::OpenPaymentPage` (C++) to show the address yourself, such as a
  QR code, instead of opening the default browser.
- `GetPrice` is the item's own price as the connect read it, which is what the gateway charges; `CheckSetup` cannot check a gateway without opening a
  checkout, so its `NotChecked` says how: buy an item with the gateway's test keys and its test card.
- To build a checkout screen of your own, `FFlockShopProvider::StartCheckout`, `GetPurchase` and `GetPurchases` are the same
  calls one at a time.
- Mac and Linux run the same code as Windows but have not been run yet.

## Things worth knowing

- **Use one way to buy.** The engine's own in-app purchase Blueprint nodes finish a purchase as it is made, before Flock has
  delivered it, so a purchase Flock then refuses is lost. The editor's Flock setup panel warns
  (`Flock.Store.EnginePurchaseNodes`) when a Blueprint uses them in a game whose shops sell real-money items. The panel's
  **Test Connection** reads the shops to find out, so the warning appears after a connection check. Keep the engine's nodes
  only for products no Flock shop item sells.
- **Steam, Epic and consoles** have no store provider in Flock. **A Steam build may take payments only through Steam's own
  wallet**, so do not ship a web checkout in it: connecting one in a build whose engine online service is Steam logs a
  warning. Web checkout is for the Epic Games Store, your own launcher or site, and other storefronts that allow it.
- **A game with its own store code** can hand `UFlockStore` its own `IFlockStoreConnection` with `UFlockStore::SetStoreLibrary`
  (or `ConnectThrough`), or redeem each purchase itself with `RedeemStoreReceipt`. Its answer to `Buy` sets
  `FFlockStoreOrder::bAnswersThePurchaseDialog`; a purchase handed back any other way leaves it false.
- **Shutting down** (`ShutdownSdk`) disconnects the store, ending a purchase that waits with `Cancelled`. Connect again after
  initializing again.
- Every call is on the game thread, as the engine's online subsystem is. The connection, prices, restore and the setup check
  are C++ only in this version.
