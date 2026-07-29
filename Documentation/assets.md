# Assets

An **asset** is a file you upload from the dashboard — an image, an audio file, a JSON table, anything —
attached to your game and listed per game version. The SDK gives you the list, resolves one by name, and
downloads its bytes into a size-budgeted cache on disk.

This is for *loose runtime content* you want to change without shipping a build. It is not a replacement
for cooked content: meshes, materials and levels belong in your packaged game or in a pak-based patching
system, not here.

The asset list is cached and served from the offline snapshot. Downloaded bytes get their own binary
cache — see [Caching](#caching), which is the part worth reading even if you skip the rest.

## Blueprint

The nodes live under *Flock | Assets*:

| Node | Returns |
|---|---|
| `Flock Get Assets` | every asset record for this game version |
| `Flock Get Asset` | one record, by name or id |
| `Flock Download Asset Texture` | a `Texture 2D` |
| `Flock Download Asset Text` | the file as text — JSON, CSV, dialogue tables |
| `Flock Download Asset Bytes` | the raw bytes |
| `Flock Download Asset File` | the local file path |
| `Flock Preload Assets` | warms the cache for records you pass in |
| `Flock Preload All Assets` | warms the cache for everything |

**The download nodes take one string pin that accepts either a name or an id.** Names are what you see on
the dashboard, so wire a name and forget ids exist. If a name and an id ever collide, the id wins.

Each download node has three pins rather than the usual two: **On Success**, **On Failure**, and
**On Progress**. On Progress fires repeatedly while bytes arrive and carries a 0–1 `Progress` value — wire
it straight into a progress bar. Success and Failure behave as everywhere else in the SDK: exactly one of
them, exactly once, at the end.

`Progress` stays at 0 for the first tick or two, until the server reports how large the file is. Show an
indeterminate bar until it moves rather than treating it as "stuck".

These pure nodes answer immediately, with no network call:

| Node | Answers |
|---|---|
| `Flock Is Asset Cached` | whether this exact version is already on disk |
| `Flock Get Uncached Assets` | which of these a preload would actually fetch |
| `Flock Get Cached Asset Path` | where a cached asset lives, or empty |
| `Flock Get Asset Cache Directory` | the cache root on this device |
| `Flock Clear Asset Cache` | *(callable)* drops every cached file |

A loading screen is `Flock Get Assets` → `Flock Get Uncached Assets` → `Flock Preload Assets`, with the
preload node's On Progress driving the bar. Preload is best-effort: one unreachable asset does not fail
the batch, and `Succeeded` reports how many of `Requested` actually landed.

## C++

Everything lives on the asset provider:

```cpp
UFlockSubsystem* Sdk = UFlockSubsystem::Get(this);
FFlockAssetProvider* Assets = Sdk->GetAssetProvider();

// The list, and a lookup by name or id.
Assets->GetAll([](TFlockResult<TArray<FFlockAsset>> Result) { /* ... */ });
Assets->GetByName(TEXT("title_logo"), [](TFlockResult<FFlockAsset> Result) { /* ... */ });

// Download by name or id, with progress.
FFlockAssetProgress Progress;
Progress.BindLambda([](int64 Received, int64 Total)
{
    UE_LOG(LogTemp, Log, TEXT("%lld / %lld"), Received, Total);
});

Assets->DownloadTexture(TEXT("title_logo"), Progress, [](TFlockResult<UTexture2D*> Result)
{
    if (Result.bSuccess)
    {
        // Keep a reference before the next GC — assign it to a UPROPERTY.
    }
});
```

`DownloadText`, `DownloadBytes` and `DownloadFile` have the same shape. Every flavour also takes an
already-resolved `FFlockAsset` instead of a string, which skips the lookup when you already hold the
record.

`PreloadWhere` takes a predicate for the C++ side of a loading screen:

```cpp
Assets->PreloadWhere(
    [](const FFlockAsset& Asset) { return Asset.ExtensionType == TEXT("png"); },
    [](float Fraction) { /* drive a bar */ },
    [](TFlockResult<int32> Result) { /* Result.Value landed */ });
```

`IsCached`, `GetUncached`, `GetCachedFilePath`, `GetCacheDirectory` and `ClearCache` are synchronous.

## Caching

**The asset list** is snapshot-cached like every other read, scoped to the game version, so it still
answers when the network is down.

**Downloaded bytes** have their own binary cache, separate from the snapshot store and living under the
project's persistent download directory — the location that stays writable on console and mobile. Entries
are keyed by asset *and* version, so:

- reading the same version twice never touches the network;
- re-uploading an asset supersedes the old copy, but only once the new one has fully landed;
- a failed or cancelled transfer leaves nothing behind, so a truncated file can never be mistaken for a
  complete one later.

The cache has a size budget (**Asset Cache Max Size MB**, default 100) with least-recently-used eviction,
where a *read* counts as a use, not just a write. An asset larger than the entire budget is still
downloaded but deliberately not cached — storing it would evict everything else and then itself.

**Downloads stream to disk.** The bytes are written as they arrive rather than assembled in memory, so a
large asset costs the disk write and a socket buffer instead of its full size in RAM.

**Signed links refresh themselves.** Download URLs are time-limited, so one held in the offline cache can
go stale. A download refused for that reason refetches that single record and retries once with the fresh
link — you do not need to handle it.

## Settings

Under *Project Settings > Flock SDK > Asset Cache*:

| Setting | Default | Does |
|---|---|---|
| Enable Asset Cache | on | Off, downloads go to a scratch file the caller owns |
| Asset Cache Directory | *(empty)* | Empty means the default under the persistent download path |
| Asset Cache Max Size MB | 100 | 0 means unbudgeted |
| Asset Download Timeout Seconds | 0 | 0 means no timeout, so a large file is never cut off mid-transfer |
| Asset Download Retry Count | 3 | Retries for a failed transfer, separate from the HTTP retry policy |
| Asset Max Concurrent Downloads | 4 | Transfers in flight at once; the rest queue |

## Things worth knowing

- **There is no sound-wave flavour.** The engine has no API that turns compressed audio (mp3, ogg) into a
  playable sound at runtime, and supporting only uncompressed WAV would fail silently on exactly the files
  a content server usually holds. Use `Flock Download Asset File` and hand the path to whatever imports
  audio in your project.
- **Texture decoding happens on the game thread**, so a very large image can cost a frame. Preload during
  a loading screen rather than mid-gameplay.
- **There is no by-name route on the backend.** `Flock Get Asset` resolves a name against the list, which
  is fetched once and cached — so the first lookup may fetch the list, and every later one is free.
- **Two callers downloading the same asset share one transfer.** The second does not start a duplicate; it
  waits for the first and receives the same result.
- **Assets are scoped to the game version, not the player**, so signing out does not clear them.
