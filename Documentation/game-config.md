# Game config & metadata

A **game config** is a named bag of tuning values you edit on the dashboard and read at runtime — damage
numbers, feature switches, drop tables. A **patch** overrides a config's values for one game version, so
you can change 1.2 without touching 1.1.

Configs are game-wide and read-only from a client. Changing one is a dashboard action, which is why there
is no write call here — see [Game commands](game-commands.md) for the per-player data you *can* change.

## Resolve, don't fetch

Nearly every graph wants **the effective values for this game version**, not the raw config and not the
patch. That is one call:

`ResolveConfigData` returns the patch's data when a patch applies to the running game version, and the
config's own data when none does. The caller never learns which happened, which is the point — the
resolution policy stays out of your gameplay code.

Reach for the individual config and patch reads only when you specifically need to know what is
overriding what.

## Blueprint

The nodes live under *Flock | Config* and *Flock | Game*:

| Node | Returns |
|---|---|
| `Flock Resolve Config Data` | the effective values — **start here** |
| `Flock Get Config By Name` | one config, found by its dashboard name |
| `Flock Get Config By Id` | one config |
| `Flock Get Configs By Tag` | every config carrying a tag |
| `Flock Get Player Features` | the feature-flag config resolved for one player |
| `Flock Get Game` | the game record |
| `Flock Get Game Version` | the running version's record |
| `Flock Get Game Version By Name` | another version, by name |

The **Data** pin is an `FFlockStructuredData` handle. Read it with the *Get Data Int / Float / String /
Bool / String Array* nodes — each takes a dotted path (`stats.damage`) and a fallback used when the field
is absent, so a missing value never breaks a graph.

After a schema sync this collapses to one node: `Get Gameplay` fetches, resolves, and hands back a typed
struct with real pins. See [Code generation](codegen.md).

**Not in Blueprint yet:** patch reads (all patches, by id, by config) and configs-by-version-tag are C++
only. `Flock Resolve Config Data` already returns patched-or-base values, which is what those reads are
usually wanted for.

## C++

Configs and patches live on the config provider; the game record lives on the game provider:

```cpp
UFlockSubsystem* Sdk = UFlockSubsystem::Get(this);

// The effective values for this game version.
Sdk->GetConfigProvider()->ResolveConfigData(ConfigId,
    [](TFlockResult<FFlockStructuredData> Data)
    {
        if (!Data.bSuccess) { return; }

        int32 Damage = 0;
        Data.Value.TryGetInt(TEXT("weapons.sword.damage"), Damage);

        // Or bind the whole thing to a struct you wrote:
        FMyGameplayTuning Tuning;
        Data.Value.GetDataAs<FMyGameplayTuning>(Tuning);
    });

// Metadata.
Sdk->GetGameProvider()->GetGame(OnGame);
Sdk->GetGameProvider()->GetGameVersion(OnVersion);

// Feature flags for one player (empty id = the signed-in player).
Sdk->GetConfigProvider()->GetPlayerFeatures(FString(), OnFeatures);
```

## Things worth knowing

- **Reads are cached and snapshot-backed.** A config fetched once is memoized, written to the offline
  snapshot, and served without a network call next launch. Concurrent fetches of the same key are
  coalesced into one request.
- **Player features are never cached.** They gate access, and stale access is worse than a round trip.
- **Dotted paths match exactly first, then the Pascal-cased name.** A field declared `max_health` reads
  back as `MaxHealth` in the flattened data, and both spellings resolve — so a path copied from the
  dashboard works, and so does one copied from a Break node.
- **Dictionary keys are never transformed.** Object and list field names are Pascal-cased on read so a
  struct can bind to them; a dict's keys are author data and stay verbatim.
- **The schema is kept verbatim** on every config, which is what codegen reads to emit typed structs.
- **`ClearCache()`** drops the in-process caches and the config snapshot category. C++ only.

---

[← Back to the README](../README.md)
