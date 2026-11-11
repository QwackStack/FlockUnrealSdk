# Player data & templates

A **player template** defines a kind of per-player record — a currency wallet, an achievement set — with
its schema and default values. A **player data** row is one player's values for one template.

Reading is covered here. Changing a row is a separate surface with its own rules — see
[Game commands](game-commands.md).

## Blueprint

The nodes live under *Flock | Player*:

| Node | Reads |
|---|---|
| `Flock Get My Data By Template` | the signed-in player's row for one template id |
| `Flock Get My Data By Tag` | the same, found by the template's tag (`currency`, `achievement`) |
| `Flock Get Player Data By Id` | one row by its own id |
| `Flock Get All Player Data` | a page of rows (empty Player Id = every player) |
| `Flock Get Player Templates` | every template this game version declares |
| `Flock Get Player Template By Id / By Name / By Tag` | one template |
| `Flock Get Template Player Data` | every player's rows for one template |
| `Flock Get Player Ban` | a player's ban record |
| `Flock Get Wallet` | the signed-in player's balances by currency code (see [The wallet](#the-wallet)) |

Each is an async node with `On Success` and `On Failure` execution pins. Read values off the returned
row's **Data** pin with the *Get Data Int / Float / String / Bool* nodes, which take a dotted path and a fallback value
returned when the path cannot be read, and *Get Data String Array*, which takes the path alone and returns an empty array
when it cannot be read.

**You do not pass a player.** The "my data" nodes and *Flock Get Wallet* read the signed-in player. Two nodes keep a
`Player Id` pin in their advanced section (the little arrow), for admin-shaped reads:

- `Flock Get Player Ban`: empty means the signed-in player.
- `Flock Get All Player Data`: empty means **every player**, not the signed-in one.

To page through only the signed-in player's rows, give *Flock Get All Player Data* their id (**Flock Get Player Id**), or in
C++ call `GetMyData(Page, Limit, ...)`, which fails (`Validation`) rather than reading every player when nobody is signed
in. For one template's row, `Flock Get My Data By Template` or `By Tag` is simpler.

If you have run a schema sync, prefer the generated one-node reads instead — `Get Currencies` fetches,
converts, and hands back a typed struct with real pins. See [Code generation](codegen.md).

## C++

Everything lives on the player provider:

```cpp
#include "FlockSubsystem.h"
#include "Providers/FlockPlayerProvider.h"

// From any actor, widget or the game instance.
UFlockSubsystem* Sdk = UFlockSubsystem::Get(this);

// The signed-in player's achievements, found by the template's tag:
Sdk->GetPlayerProvider()->GetMyDataByTag(TEXT("achievement"),
    [](TFlockResult<FFlockPlayerData> Result)
    {
        if (Result.bSuccess)
        {
            int32 Wins = 0;
            Result.Value.Data.TryGetInt(TEXT("wins"), Wins);
        }
    });
```

## The wallet

The player's wallet is the row of the template tagged `currency`. `GetWallet` reads it into an `FFlockWallet`: the
balances of the game's own currencies (`GameCurrencies`) and of real currencies (`RealCurrencies`), each code exactly
as the game's currency config spells it.

```cpp
// Sdk: UFlockSubsystem::Get(this), as above.
Sdk->GetPlayerProvider()->GetWallet([](TFlockResult<FFlockWallet> Wallet)
{
    if (!Wallet.bSuccess) { return; }
    const int64 Gold = Wallet.Value.BalanceOf(TEXT("Gold"));   // 0 when the wallet holds no Gold
    const bool bHasGems = Wallet.Value.Holds(TEXT("Gems"));     // a zero balance is still held
    // Wallet.Value.CanAfford(Item): a game-currency item whose price the balance covers; false for a real-money item
    // or one with no currency.
});
```

- **Codes match with their letter case**, as the server matches them: `Holds("gold")` is false for a wallet of `Gold`.
- **It reads through the player-data cache**, which every purchase, store redemption, consume and fund grant keeps
  current with the wallet it answers; `ClearCache()` reads it afresh. A money call whose outcome is unknown drops the
  cached rows, so the next read asks the server.
- **No wallet is a failure** (`Validation`): the server makes a player's wallet when the player registers, from the Game
  Version's currency template, so a player who registered before that template existed, or under another Game Version,
  has none.
- In Blueprint, ask the wallet with *Wallet Balance Of*, *Wallet Holds* and *Can Afford* (under *Flock | Shop*).

## Things worth knowing

- **"My data" needs a signed-in player.** `GetMyDataByTemplate` / `ByTag` resolve the current player and
  fail with a Validation error when signed out. The first call paginates *all* of that player's rows and
  caches them, so asking for a second template afterwards costs nothing.
- **No row is not an error.** A player who has never written to a template comes back as a success with
  an empty record (empty `Id`), not a failure — check `Id` rather than the result flag.
- **A ban is always fetched fresh** and is never cached, because it can change server-side at any moment.
  An unbanned player is likewise a normal success with an empty record; `IsBanned()` tells them apart.
  Per-feature bans are keyed by feature name in the record's `Data` map.
- **Signing out drops the cached rows** so the next player never reads the previous one's data. Templates
  and their offline snapshot survive — they belong to the game version, not the player.
- **A read already on its way keeps nothing it might have missed.** When the cached rows are dropped, or a purchase,
  consume, fund grant or game command answers with a newer row, a read sent before that still answers its caller but
  is not kept, and the next read asks the server. Two such answers arriving in the opposite order to the one the server
  wrote them in never put the older row over the newer: the row's `UpdatedAt` decides.
- **A player's own data and ban act for the player signed in when they were called.** Each attempt goes out only while
  that sign-in lasts, so a retry after a sign-out or another player's sign-in is never sent as the next player (the call
  fails `Cancelled`); template reads are game-wide and keep retrying. Every call reads the token on each attempt, so the
  attempt after an expired token is renewed carries the new one.
- **Reading structured data** — a row's and a template's `data` both come back as an `FFlockStructuredData`
  handle, the same type game config uses. Read it by dotted path (`TryGetInt("stats.level")`, exact match
  first, then the flattened PascalCase name), or bind the whole thing to your own `USTRUCT` with
  `GetDataAs<T>()`.

---

[← Back to the README](../README.md)
