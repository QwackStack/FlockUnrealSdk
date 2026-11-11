# Real-money purchases

A real-money item (`FFlockShopItem::IsRealMoney`) is paid for through the platform's own way to pay: the store on a phone
(Google Play on Android, the App Store on iOS), and a web checkout with the game's payment gateway on Windows, Mac and Linux
(see [Web checkout](#web-checkout-windows-mac-and-linux)). Connect `UFlockStore` once at start, and the shop provider's ordinary
`Purchase` buys real-money items through it. On a phone the store charges the player, Flock checks the purchase with the store
and delivers the item, and only then is the store's purchase finished. Items priced in a game currency are still bought from
the wallet by the same call.

**Working in Blueprint?** Do [Setting it up](#setting-it-up) first, then go straight to [Blueprint](#blueprint): every step
there is a node, with no C++. [Trying a purchase from the editor](#trying-a-purchase-from-the-editor) shows how to buy
without a phone.

## Setting it up

1. **Nothing to install.** The store is reached by Flock's own `FlockPlatformStore` module, through the engine's online
   subsystem: Flock turns the engine's Online Subsystem plugin on with it, and the platform's store subsystem (Google Play,
   iOS) is on by default on its platform.
2. **Set up the store in the project.**
   - **Android (UE 5.8 or later):** in *Project Settings → Platforms → Android*, turn on **Enable Google Play Support**, enter the game's
     Google Play license key, and add the `com.android.vending.BILLING` permission. Keep `bSupportsInAppPurchasing=true`
     under `[OnlineSubsystemGooglePlay.Store]` (the engine's default). Google Play takes new builds only with a current
     Billing Library, and only UE 5.8 ships one (Billing 8); on UE 5.4 to 5.7 the store is refused at connect, and a game
     with billing code of its own redeems its purchases with `RedeemStoreReceipt` (see [Shop](shop.md#redeeming-a-store-purchase)).
     Google Play also requires a recent target SDK for new builds (36 at the time of writing).
   - **iOS:** in *Project Settings → Platforms → iOS*, turn on **Enable StoreKit Support**, and add the In-App Purchase capability to
     the app. Flock's App Store support is written against the engine's own StoreKit code and has not yet run on an iPhone: buy in
     the App Store's sandbox before you ship.
3. **Set up the items in the Flock dashboard.** Price each item in a currency the game's currency config lists as real
   money, and enter its product id under **Store product IDs**, exactly as the store has it. In the store, make a
   `currency_pack` or `consumable` item a consumable product and a `standard` item a one-time product. Connect the store
   under **Monetization → Payments**, with the game's store keys, so Flock's server can check each purchase.

## Connecting

Connect once, after Flock has initialized. `Sdk` is the Flock subsystem, `UFlockSubsystem::Get(this)` from any actor,
widget or the game instance; it holds the connection (`Sdk->GetStore()`) until it is disconnected.

```cpp
#include "FlockSubsystem.h"
#include "Store/FlockStore.h"

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
        // A purchase the player paid for that Flock refused to deliver, or whose delivery got no answer
        // (Purchase.Error.bOutcomeUnknown: Flock may have delivered it after all). It stays unfinished in the store, which
        // hands it back at the next launch; Purchase.Error's code and Hint say why. Do not sell it to the player again meanwhile.
    });
});
```

`Connect` reads the game's shops, registers each item that has a product id for this store, and from then on sends Flock
every purchase the store hands over whenever a player is signed in. On failure, `Connected.Error` says why and how to fix
it. Leave `FFlockStoreOptions::PaymentProvider` empty for this platform's own way to pay: on Windows, Mac and Linux that is
a web checkout (below). It is refused:

- for a payment gateway named on a phone: a phone store's rules forbid paying for digital goods on a web page;
- when Flock's store module is not loaded (no store library, for example with the engine's Online Subsystem plugin turned
  off): a phone store's rules forbid paying for digital goods elsewhere, so Flock never opens a web page instead;
- on Android with an engine older than UE 5.8, or with `bDisableLocalAcknowledgeAndConsume=true` under
  `[OnlineSubsystemGooglePlay.Store]` (the engine would then finish nothing Flock delivers);
- while another connection is connected: use the connected one, or `Disconnect` it first.

## Buying

`ShopItemId` is the shop item's `Id`, read from its shop (see [Shop](shop.md#c)); `Sdk` is the Flock subsystem, as above:

```cpp
const TWeakObjectPtr<UFlockSubsystem> WeakSdk(Sdk);
Sdk->GetShopProvider()->Purchase(ShopItemId, [WeakSdk](TFlockResult<FFlockPurchaseResult> Result)
{
    if (Result.bSuccess && !Result.Value.IsPaid())
    {
        // The store waits for the payment (a slow payment method, a parent's approval). OnPurchaseCompleted follows once paid.
        return;
    }
    if (Result.bSuccess)
    {
        // Delivered: Result.Value.Granted says what was handed over. Inventory is filled only when HasInventoryRow(),
        // Wallet only when HasWallet() (currency moved). A web checkout's result carries neither: read the wallet afresh.
        UFlockSubsystem* Flock = WeakSdk.Get();
        if (!Result.Value.HasWallet() && Flock && Flock->IsInitialized())
        {
            Flock->GetPlayerProvider()->GetWallet([](TFlockResult<FFlockWallet> Wallet) { /* show the balances */ });
        }
        return;
    }
    if (Result.Error.bOutcomeUnknown)
    {
        // No answer said whether it was done. A phone store has already taken the payment: tell the player it will arrive,
        // and never offer it again on this answer. A web checkout's page may still be paid: read the open checkouts
        // (UFlockStore::Restore) before offering it again.
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

### How the call ends

| Result | Means | What to do |
|---|---|---|
| Success, `IsPaid()` false | The store waits for the payment (a slow payment method, a parent's approval) | Nothing yet: `OnPurchaseCompleted` follows once it is paid |
| Success, `IsPaid()` true | Delivered | Show `Granted`. `Inventory` is the row the player now owns only when `HasInventoryRow()` (an item that hands its contents over outright makes none); `Wallet` holds the balances only when `HasWallet()` (currency moved). A web checkout's result carries neither, and its payment dropped the cached wallet: read the wallet (`GetWallet`) and the inventory afterwards |
| Failure, `Error.Type` is `Store` | The store did not take the payment, or the gateway declined it or let it expire; nothing was charged | Compare `Error.StoreFailureReason` with `FlockStoreFailureReasons` (`Cancelled` when the player closed the dialog). Offer it again if the player wants it |
| Failure, `Error.bOutcomeUnknown` set, on a phone store | The store took the payment; whether Flock delivered it is unknown | Tell the player it will arrive, then call `Restore` or wait for `OnPurchaseCompleted`. **Never offer it again** on this answer: the store hands the purchase back, and Flock answers a repeat with what it delivered the first time |
| Failure, `Error.bOutcomeUnknown` set, on a web checkout | The payment page may still be paid | Read the open checkouts (`Restore`, or `GetPurchases(FlockPurchaseStatuses::Pending, ...)`) before offering it again; one paid meanwhile is delivered by Flock |
| Failure, `Cancelled`, or `Auth` with a `StatusCode` other than 403 | The sign-in the purchase was for ended (the player signed out, another signed in, or the sign-in expired), or nobody was signed in | Nothing was charged if it ended before the store's dialog or the payment page opened. If it ended later, a payment made is still delivered to that player: on a phone once they are signed in again (or through `Restore`); on a web checkout by Flock's server, unreported, so refresh the wallet and inventory after each sign-in |
| Any other failure | Refused before the store was asked (below), so nothing was charged; or, on a phone store, paid and refused by Flock's server (below) | Show `Error.ToDisplayText()`: its fix says what to change. If `OnPurchaseNotDelivered` reported it, the player paid: do not sell it again, as the store hands it back at the next launch |

**Nothing was charged** when the purchase was refused before the store was asked:

- another store purchase is under way (`Validation`, below; on a web checkout, another purchase of the same item);
- the item was switched off in the dashboard since the store connected (`ShopItemNotForSale`, checked just before the dialog);
- nobody is signed in (`Auth`), or the player signed out or changed while the item was read (`Cancelled`);
- no store is connected, the item has no product id for this store, it was added in the dashboard since the store
  connected (connect again to sell it), or it is bought for another player named by id (all `Validation`);
- on a web checkout, the server refused to open the checkout (its code says why).

**Paid, and not delivered.** Flock's server refused the store's purchase when it checked it (the error carries the server's
code, such as `PaymentProviderReceiptBelongsToAnotherPlayer`). `OnPurchaseNotDelivered` is raised with the same error just
before the call's failure. The purchase stays unfinished in the store, which hands it back at the next launch: do not sell
it to the player again, and fix what the error's `Hint` names. `OnPurchaseNotDelivered` is raised too when Flock's answer
never came (`bOutcomeUnknown`), though Flock may have delivered it: at the next launch Flock answers the repeat with what it
delivered, so nothing is delivered twice.

**Delivered before it is finished.** The store's purchase is finished only once Flock delivered it.

- A purchase Flock refused, or one whose answer never came, stays unfinished: the store hands it back at the next launch,
  where it is sent again once a player is signed in, and Flock answers a repeat with what it delivered the first time.
- Each one is sent once a run of the game, a connection made again included. `Restore` sends again one whose send failed
  earlier, and leaves one still being sent to its send.
- A purchase whose send ended with its sign-in, or that the server refused for the sign-in (`Auth`, not a 403), never
  reached Flock: it raises no `OnPurchaseNotDelivered`, stays unfinished, and is sent at the next sign-in.

**For the player who started it.** The purchase belongs to the sign-in it started under.

- When that player signs out or another signs in, the call ends `Cancelled`.
- A payment made after that is held, and delivered when the player who opened the store's dialog signs in again during this
  run of the game.
- After a restart the store hands it back to whoever is signed in then, or to `Restore`.

**One store purchase at a time.** The store keeps one purchase dialog open at a time.

- A second purchase, of any item, is refused (`Validation`) until the first has ended and its dialog has closed.
- A purchase started from inside `OnPurchaseCompleted` or `OnPurchaseNotDelivered` is not refused: the first has ended by
  then.
- A dialog the store never answers keeps refusing them until the store is disconnected and connected again.

**Checked just before the store's dialog.** An item switched off (Inactive) in the dashboard since the store connected is
refused with `ShopItemNotForSale` before the store charges anything.

**More to know:**

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
if (Store == nullptr)
{
    return;   // no store connected: connect one first
}

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
gateway connected with its keys under **Monetization → Payments** in the Flock dashboard. No store product id, and nothing in
the project settings.

```cpp
// Sdk: UFlockSubsystem::Get(this), as above.
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
- **A payment page the browser could not open** fails `Validation` saying why; the checkout is still read, so if the player
  pays it anyway, `OnPurchaseCompleted` reports it.
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
  checkout, so its `NotChecked` says how: buy an item with the gateway's test keys and its test card (see
  [Trying a purchase from the editor](#trying-a-purchase-from-the-editor)). `IsReady()` is true when nothing else was found to fix.
- To build a checkout screen of your own, `FFlockShopProvider::StartCheckout`, `GetPurchase` and `GetPurchases` are the same
  calls one at a time.
- Mac and Linux run the same code as Windows but have not been run yet.

### Trying a purchase from the editor

On Windows, Play In Editor pays through a web checkout, so a real-money purchase can be tried without a phone and without
real money:

1. In the Flock dashboard, under **Monetization → Payments**, connect the payment gateway (Stripe, Tap or Moyasar) with its
   **test** keys, which each gateway gives beside its live ones.
2. Price an item in a currency the game's currency config lists as real money.
3. Press Play, sign a player in, connect the store and buy the item (C++ above, or the Blueprint flow below). The gateway's
   payment page opens in your browser.
4. Pay with the gateway's test card, listed in the gateway's own documentation. No money moves.

**What success looks like:** within a few seconds of paying, the purchase completes successfully with `IsPaid()` true
(Blueprint: **Flock Purchase**'s *On Success*, with **Is Purchase Paid** true), its `Granted` lists what was handed over,
and `OnPurchaseCompleted` is raised (Blueprint: **On Store Purchase Completed** on Flock's events). A read of the wallet and
inventory afterwards shows the item.

## Blueprint

The store's nodes live under *Flock | Store*; buying is the ordinary **Flock Purchase** node (*Flock | Shop*). The usual
flow, once at start and then for each purchase:

1. On **Event BeginPlay** of an actor that lives as long as your shop, or **Event Init** of the game instance, drag off
   **Flock Get Events** and call **Call or Register on Initialized**, with a custom event such as *Flock Ready*. It runs at
   once when Flock has already initialized, and later otherwise.
2. In *Flock Ready*, call **Flock Connect Store**. Leave its *Options* unwired for every default.
3. From its *On Success*, buy with **Flock Purchase**, giving it the item's *Shop Item Id*: the item's **Id**. To find it,
   run **Flock Get Shop By Name** with the shop's dashboard name, then **Flock Get Shop Items** with that shop's **Id**, and
   break the item you want for its **Id** (see [Shop](shop.md#blueprint)).
4. From Flock Purchase's *On Success*, check **Is Purchase Paid** on the result before handing anything over: a payment the
   store still waits for is not paid yet. From *On Failure*, read the *Error* (below).

![Event BeginPlay runs into Call or Register on Initialized, whose Target is Flock Get Events, with the custom event Flock Ready; Flock Ready runs Flock Connect Store, its On Success runs Flock Purchase with a Shop Item Id, and Flock Purchase's On Success runs a Branch on Is Purchase Paid](images/store-connect-purchase.png)

The picture is built on an actor, and chains Flock Connect Store straight into Flock Purchase only to fit on one page. In a
game, connect once at start (steps 1 and 2), and run **Flock Purchase** from your shop's Buy button, such as the button's
*On Clicked* in your shop widget; a connect for each purchase is refused while a store is connected.

**Is Purchase Paid** reads the result of **Flock Purchase**. **Is Player Purchase Paid** reads a purchase you looked up with
**Flock Get Player Purchase** or **Flock Get Player Purchases**, such as an open web checkout.

| Node | What it does |
|---|---|
| `Flock Connect Store` | connects the way to pay, once at start: the success pin carries the store Flock holds. Its *Options* pin is `FFlockStoreOptions` without `OpenPaymentPage`, which is C++ only, so from Blueprint a web checkout's page opens in the default browser; leave it unwired for every default |
| `Flock Get Store Price` | a real-money item's price (*Text* to show); false, with an *Error* saying why, when no store is connected or the item has no price here |
| `Flock Restore Purchases` | has Flock deliver what the store account owns (*Restored*, *Not Delivered*); on a web checkout, reads the open checkouts again |
| `Flock Check Store Setup` | what the store did not find, and whether Flock's server can ask it; *Is Store Ready* says whether anything needs fixing (on a web checkout, true when nothing was found: *Not Checked* says how to check the gateway) |
| `Flock Get Store` | the store Flock holds, or none |
| `Flock Disconnect Store` | disconnects it; does nothing when none is connected |
| `Flock Start Checkout` | opens a web checkout and hands back its *Payment Url* and *Purchase Id*, for a game that shows the page itself |
| `Flock Get Player Purchase`, `Flock Get Player Purchases` | one of the player's purchases, or a page of them by status (*Flock Purchase Status Pending* for the open checkouts); *Is Player Purchase Paid* and *Is Player Purchase Pending* read one |
| `Flock Redeem Store Receipt` | has Flock redeem a purchase the game's own store code made, from *Make App Store Receipt* or *Make Google Play Receipt*; safe to send again |

Connect from *Call or Register on Initialized* on Flock's events (*Get Events*), which runs at once when Flock has already
initialized. Bind **On Store Purchase Completed** on Flock's events once, too: it is raised for every delivery the connected
store reports, whichever connection made it, right after the store's own `OnPurchaseCompleted` (a web checkout paid while
nothing read it is delivered but not reported: refresh the wallet and inventory after each sign-in). **On Store Purchase Not Delivered** is
raised for a store purchase the player paid for that Flock refused to deliver, and for one whose delivery got no answer
(its *Error*'s *Outcome Unknown* is true: Flock may have delivered it after all); its *Error* says why and how to fix it. Do
not sell that item to the player again meanwhile: the store hands the purchase back at the next launch.

**Flock Purchase**'s success pin can carry a purchase the store is still waiting to be paid for (a pending payment or a
parent's approval): check *Is Purchase Paid* before handing anything over; On Store Purchase Completed follows once it is
paid. After a paid purchase, the result's *Inventory* is filled only when **Has Inventory Row** is true and its *Wallet* only
when **Has Wallet** is true; a web checkout's result carries neither, so read them again (**Flock Get Wallet**, **Flock Get
Owned Item Counts**).

Its failure pin carries the *Error*. Compare its *Store Failure Reason* with the **Flock Store Failure Reason** nodes
(*Cancelled*, *Payment Declined*, *Already Owned*, *Already Pending*, *Product Unavailable*, *Store Unavailable*, *Not Signed
In To The Store*, *Expired*, *Unknown*): any of those means nothing was charged. Then check *Outcome Unknown*, true when no
answer said whether it was done. **Flock Get Store** → **Is Web Checkout** tells the two cases apart:

- **A phone store** (Is Web Checkout false): the store already took the payment; only Flock's delivery is unknown. Tell the
  player it will arrive, run **Flock Restore Purchases** or wait for On Store Purchase Completed, and never offer the item
  again on this answer.
- **A web checkout** (Is Web Checkout true): the payment page may still be paid. Run **Flock Restore Purchases**, which reads
  the open checkouts, or **Flock Get Player Purchases** with **Flock Purchase Status Pending**, before offering it again.

The other failures, with *Outcome Unknown* false, are listed under [How the call ends](#how-the-call-ends).

With no store connected, *Flock Get Store Price*, *Flock Restore Purchases* and *Flock Check Store Setup* fail with
"No store is connected" and how to connect one; with no Flock in the World Context's game, the async nodes and *Flock Get Store Price* say how to initialize it
(*Flock Get Store* is none, and *Flock Disconnect Store* does nothing).

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
- Every call is on the game thread, as the engine's online subsystem is. Blueprint has a node for each call above but
  `OpenPaymentPage`, `SetStoreLibrary` and `ConnectThrough` (a game's own store code in C++); see [Blueprint](#blueprint).
