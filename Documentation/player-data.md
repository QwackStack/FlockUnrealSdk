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

Each is an async node with `On Success` and `On Failure` execution pins. Read values off the returned
row's **Data** pin with the *Get Data Int / Float / String / Bool / String Array* nodes, which take a
dotted path and a fallback value.

**You do not pass a player.** The signed-in one is used, and the `Player Id` pin lives in each node's
advanced section for the admin-shaped reads. Note the exception: on `Flock Get All Player Data` an empty
id means *every* player, not the signed-in one — in C++ use `GetMyData` when you want just theirs.

If you have run a schema sync, prefer the generated one-node reads instead — `Get Currencies` fetches,
converts, and hands back a typed struct with real pins. See [Code generation](codegen.md).

## C++

Everything lives on the player provider:

```cpp
UFlockSubsystem* Sdk = UFlockSubsystem::Get(this);

// The signed-in player's wallet, found by the template's tag:
Sdk->GetPlayerProvider()->GetMyDataByTag(TEXT("currency"),
    [](TFlockResult<FFlockPlayerData> Result)
    {
        if (Result.bSuccess)
        {
            int32 Coins = 0;
            Result.Value.Data.TryGetInt(TEXT("coins"), Coins);
        }
    });
```

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
- **Reading structured data** — a row's and a template's `data` both come back as an `FFlockStructuredData`
  handle, the same type game config uses. Read it by dotted path (`TryGetInt("stats.level")`, exact match
  first, then the flattened PascalCase name), or bind the whole thing to your own `USTRUCT` with
  `GetDataAs<T>()`.

---

[← Back to the README](../README.md)
