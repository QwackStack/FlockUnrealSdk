# Flock Unreal SDK

The Flock Unreal SDK provides access to Flock's game backend services from Unreal Engine games.

> **Early release.** The SDK's boot/init foundation, the network transport (HTTP client, automatic
> retry, typed errors), automatic edit-time Game Version baking, **player authentication**,
> **analytics**, **game config** (with an offline snapshot cache), the **shop** (catalog, purchase,
> inventory), **player data & templates** (with bans), **game commands** (player-data mutations with an
> offline queue), and **typed Blueprint code generation** now ship. The remaining feature provider
> (assets) builds on this next. See [Status](#status).

## Contents

- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Setup](#setup)
- [Initialization](#initialization)
- [Accessing the SDK](#accessing-the-sdk)
- [SDK events](#sdk-events)
- [Logging & debugging](#logging--debugging)
- [Authentication](#authentication)
- [Analytics](#analytics)
- [Player data & templates](#player-data--templates)
- [Game commands](#game-commands)
- [Code generation](#code-generation)
- [Testing](#testing)
- [Status](#status)

## Features

- **Global accessor** — `UFlockSubsystem`, a `UGameInstanceSubsystem` ready when the game starts and the
  entry point to the SDK.
- **Hands-off startup** — the SDK auto-initializes from your project settings at launch, or you drive
  init yourself.
- **Offline, synchronous init** — the Game Version ID is resolved at edit time and baked into
  `DefaultGame.ini`; runtime init makes no network call.
- **Automatic version baking** — the Game Version ID resolves and bakes itself when you edit your
  settings (or on editor load if unresolved), with a Play-In-Editor setup guard and a packaging build
  guard. No manual step required; a manual **Resolve Game Version** action is still available.
- **HTTP transport** — an instance HTTP client over the engine HTTP module with automatic retry
  (exponential backoff + jitter, `Retry-After` aware) and a callback + result surface (no C++
  exceptions). JSON is (de)serialized to/from your `USTRUCT` models, unwrapping the backend envelope.
- **Typed errors** — every failure is an `FFlockError` with a typed `EFlockErrorType`, the server's
  `EFlockErrorCode`, and the server's human-readable message (`ServerMessage`) — Blueprint-ready,
  with `UFlockErrorLibrary` exposing display text and coded-error group checks to Blueprint.
- **SDK events** — a single hub (`GetEvents()`) for lifecycle, auth, session, and consent events,
  Blueprint-assignable, with auto-init-safe `CallOrRegister` entry points for the init events.
- **Authentication** — email/device/Google/Apple/Steam login and registration, encrypted token
  persistence, automatic session restore, silent token refresh, and the account flows.
- **Analytics** — a diagnostic log API, session tracking, automatic exception capture, an offline
  queue that survives crashes, consent gating, and next-launch crash reporting.
- **Game config** — fetch configs and patches (by id, by name, by tag, by version tag) and resolve a
  config's effective values (patch for this game version, else its base data). Read values in Blueprint
  with typed accessors by dotted path, or bind them to your `USTRUCT` in C++. Plus the game record and
  version info.
- **Shop** — browse shops and items (all shops, by id or name, a single item, a shop's items), buy an
  item for a player, and read a player's inventory. Catalog reads are cached and snapshot-backed; a
  purchase is money-safe (never retried on an ambiguous failure, never queued) and reports the
  transaction for revenue metrics; inventory is always fetched fresh.
- **Player data & templates** — read a player's saved data (by row id, a page of rows, or the signed-in
  player's row for a template by the template's id or tag), browse the player templates that define those
  records, and check a player's ban status. A player's rows are paginated once and cached; templates are
  cached and snapshot-backed.
- **Game commands** — the server-validated way to change a player's data: write a set of fields or a
  single field onto a data row, unlock an achievement, or add funds to a wallet. Every command returns
  the updated row and writes it back into the player cache. Data writes queue offline and replay
  automatically; funds never do — a grant fails rather than being queued, and is never re-sent after an
  ambiguous failure.
- **Code generation** — one menu click turns your backend's templates, configs, and shops into typed
  Blueprint structs, enums, and one-node reads, writes, and purchases. No C++, no toolchain, no compile
  step.
- **Offline snapshot cache** — successful config, game, shop-catalog, and player-template reads are cached
  to disk, scoped to the game version, and served when the network is down; toggleable in settings.
- **Pluggable logger** — route SDK breadcrumbs and errors into your own telemetry or on-screen debugger.
- **Blueprint-friendly** — every provider call is a self-contained async node, and the fire-and-forget
  calls plus auth/session state are one-node too (no "Get Flock Subsystem" needed). Events, error types,
  and structured data all read from Blueprint.

## Requirements

- **Unreal Engine 5.5.**
- A C++ project (the SDK is a code plugin). Blueprint-only projects need the C++ toolchain installed to
  compile the plugin.

## Installation

1. Copy the `FlockUnrealSdk` folder into your project's `Plugins/` directory
   (`YourProject/Plugins/FlockUnrealSdk/`).
2. Right-click your `.uproject` → **Generate Visual Studio project files**.
3. Rebuild the project (the plugin's `Flock` runtime and `FlockEditor` editor modules compile with it).
4. The plugin is enabled by default. Verify under **Edit → Plugins → Online Platform → Flock SDK**.

## Setup

Open **Project Settings → Plugins → Flock SDK Settings**. Required values:

- **API URL** — Flock API endpoint (default: `https://api-flock.qwacks.com`)
- **API Key** — your Flock API key
- **Game Name** — your game's name from the Flock dashboard
- **Game Version** — your game version name; the matching ID is resolved at edit time and baked in, so
  init makes no network call

Values are written to your project's `DefaultGame.ini`.

### Baking the Game Version

The Game Version ID bakes **automatically**: it resolves whenever you fill in or change a resolve input
(**API URL** / **API Key** / **Game Version**), and once on editor startup if the project has a version
name but no baked ID. It writes the ID into `DefaultGame.ini` (the read-only **Game Version ID** field on
the settings panel), so runtime init never contacts the server. You can also force a resolve any time with
**Tools → Flock → Resolve Game Version**.

## Initialization

### Automatic (default)

With the settings above filled in, **Auto-Initialize On Load** is on by default, so `UFlockSubsystem`
initializes itself when the game starts. Nothing to call — just use the subsystem when you need it.

```cpp
// React to init (optional). Auto-init completes before BeginPlay, so use CallOrRegister —
// it fires immediately if the SDK is already initialized, otherwise when init succeeds:
if (UFlockSubsystem* Flock = UFlockSubsystem::Get(this))
{
    FFlockInitializedCallback OnReady;
    OnReady.BindDynamic(this, &AMyActor::HandleFlockReady); // HandleFlockReady must be a UFUNCTION()
    Flock->GetEvents()->CallOrRegister_OnInitialized(OnReady);
}
```

To drive init yourself — e.g. to defer past a splash screen or EULA — turn **Auto-Initialize On Load**
off (Flock SDK Settings → Initialization) and call one of the manual entry points below.

> **Init is fail-safe.** A bad config or an unresolved Game Version leaves the SDK uninitialized and
> logs the reason (it never crashes startup). Guard with `IsInitialized()`, read
> `GetInitializationError()`, or handle `GetEvents()->OnInitializationFailed`.

### Manual

```cpp
UFlockSubsystem* Flock = GetGameInstance()->GetSubsystem<UFlockSubsystem>();

// From project settings:
Flock->InitializeFromSettings();

// ...or from an explicit config (e.g. tests / custom startup):
FFlockInitConfig Config;
Config.ApiUrl = TEXT("https://api-flock.qwacks.com");
Config.ApiKey = TEXT("...");
Config.GameId = TEXT("my-game");
Config.GameVersion = TEXT("1.0.0");
Config.GameVersionId = TEXT("...");   // baked at edit time
Flock->InitializeWithConfig(Config);
```

## Accessing the SDK

**C++:**

```cpp
#include "FlockSubsystem.h"

if (UFlockSubsystem* Flock = UFlockSubsystem::Get(WorldContextObject))
{
    if (Flock->IsInitialized())
    {
        const FString VersionedUrl = Flock->GetVersionedApiUrl(); // e.g. https://.../v1
    }
}
```

`UFlockSubsystem::Get(WorldContext)` is a convenience wrapper over
`GetGameInstance()->GetSubsystem<UFlockSubsystem>()`; either works.

**Blueprint: you usually don't need to get the subsystem at all.** Every node resolves the SDK from the
graph it's called in, so a call is one node with no Target pin to wire:

- **Provider calls** are async nodes with success/failure pins — `Flock Login With Email`,
  `Flock Get Config By Name`, `Flock Get Shop Items`, `Flock Get My Data By Tag`, `Flock Purchase`, … .
  Each fires exactly one pin, and fails with a Validation error if the SDK isn't initialized.
- **Fire-and-forget calls and state reads** are plain nodes — `Flock Log Event`, `Flock Record Screen
  View`, `Flock Set Analytics Consent`, `Flock Is Authenticated`, `Flock Get Player Id`, `Flock Logout`,
  `Flock Is Initialized`, `Flock Get Events`, … . All are safe no-ops (or return defaults) before init,
  so they never need an "is ready" guard.

Search the node menu for **Flock** to see the full set. The subsystem is still there via **Get Flock
Subsystem** if you prefer a Target pin, and you *do* need it for one-time setup — `Initialize From
Settings`, `Initialize With Config`, and `Shutdown Sdk`, which are deliberately not duplicated as
free-floating nodes.

## SDK events

`GetEvents()` returns the SDK event hub (`UFlockEvents`): lifecycle (`OnInitialized`,
`OnInitializationFailed`, `OnShutdown`), auth (`OnAuthenticated`, `OnTokenRefreshed`, `OnAuthExpired`,
`OnLoggedOut`, `OnSessionRestored`), session (`OnSessionStarted`, `OnSessionEnded`, `OnSessionPaused`,
`OnSessionResumed`), and consent (`OnConsentChanged`). All are Blueprint-assignable and raised on the
game thread; auth/session/consent events are declared now and raised by their features as they land.

Two things to know:

- **Auto-init fires before you can bind.** Initialization completes during GameInstance startup, so a
  plain `OnInitialized` binding made in `BeginPlay` misses it. Use `CallOrRegister_OnInitialized` (or
  `..._OnInitializationFailed`) — it fires immediately when init already happened, otherwise on the
  next init. One-shot.
- **Subscriptions survive `ShutdownSdk()`.** They stay bound across re-initialization and are released
  with the GameInstance (dynamic delegates hold weak references, so destroyed subscribers are skipped).

## Logging & debugging

The SDK routes every breadcrumb and error through a logger. Turn on **Enable Debug Logs** in the
settings for verbose output under the `LogFlock` category; warnings and errors always surface.

Turning it on also raises the `LogFlock` category to `Verbose` so the breadcrumbs actually reach the
console — it only ever raises, so `Log LogFlock Verbose` typed at the console still works with the
setting off.

**Every network call is traced**, and each line names the caller — `[C++]`, or the graph it came from —
the same way provider lines do:

```
[Flock SDK] -> GET http://localhost:8001/v1/player_data?player_id=01KY... [Blueprint 'bpTest']
[Flock SDK] <- 200 GET http://localhost:8001/v1/player_data?player_id=01KY... (34 ms, 2060 bytes) [Blueprint 'bpTest']
```

The origin is captured when the request goes out, not when the response lands, so a slow call is still
attributed to the graph that made it rather than to whatever started in the meantime.

A call that fails is logged as a **warning**, with the server's own reason, and does not need debug logs
turned on — you are usually chasing a failure precisely because you couldn't see it:

```
[Flock SDK] <- 422 POST .../game_command/update_player_data (28 ms) [Blueprint 'bpTest']: {"detail":{"code":"game_command.template_validation_failed",...}}
[Flock SDK] <- POST .../analytics/sessions [C++] failed to reach the server after 5001 ms: connection refused
```

Request and response **bodies are not logged**. The sign-in body carries a password and every other
request carries a bearer token, and a log is the thing people paste into bug reports. Failure responses
are the exception — the first 512 characters are included, because that is the server's error document,
not user data.

Inject your own logger (e.g. to feed an on-screen debugger or telemetry) by implementing `IFlockLogger`:

```cpp
class FMyFlockLogger : public IFlockLogger
{
public:
    virtual void LogDebug(const FString& Message) override { /* ... */ }
    virtual void LogInfo(const FString& Message) override { /* ... */ }
    virtual void LogWarning(const FString& Message) override { /* ... */ }
    virtual void LogError(const FString& Message) override { /* ... */ }
};

Flock->SetLogger(MakeShared<FMyFlockLogger>());
```

Every SDK log line for a provider call names its caller, so a log shows where a request came from:

```
[Flock SDK] Email login [Blueprint 'bpTest'] starting...
[Flock SDK] Email login [C++] successful for player: 01KY1QNRFNZO4E2KHNGV9F175N
```

To watch the whole flow narrate itself — init, auth state, the local guards, then a chained
register → login → email-verification run — use the console command (development builds):

```
Flock.SelfTest
```

It initializes from your Project Settings and calls your real backend, so the first run registers a
demo player (`pUE@x.com`). Point the settings at a dev environment before running it.

## Authentication

Sign players in with email, device id, Google, Apple, Steam, Facebook, or Discord. Tokens are
stored encrypted between launches and the session is restored automatically on startup; expired
access tokens refresh silently, including a one-shot retry for authenticated calls that raced the
expiry.

**Blueprint** — use the async nodes in *Flock | Auth* (each has success/failure pins):
`Flock Login With Email`, `Flock Register With Device`, `Flock Restore Session`,
`Flock Forgot Password`, `Flock Reset Password`, `Flock Send Email Verification`,
`Flock Verify Email`, `Flock Revoke Token`, `Flock Refresh Token`, `Flock Is Name Available` —
and read `Is Authenticated` / `Get Player Id` / call `Logout` on the Flock subsystem. Bind
`On Authenticated`, `On Logged Out`, `On Session Restored`, and `On Auth Expired` on the event
hub (`Get Events`) to react anywhere.

**C++** — everything lives on the auth provider:

```cpp
UFlockSubsystem* Sdk = UFlockSubsystem::Get(this);
Sdk->GetAuthProvider()->LoginWithEmail(Email, Password,
	[](TFlockResult<FFlockPlayerLoginResponse> Result)
	{
		if (Result.bSuccess)
		{
			UE_LOG(LogTemp, Log, TEXT("Signed in as %s"), *Result.Value.PlayerId);
		}
	});
```

Registration notes: the display name is optional and server-enforced unique; registering an
identity that already has an account completes successfully with `bAlreadyRegistered` set (log in
instead). `RevokeToken` invalidates the refresh token server-side; call `Logout` afterwards for a
full sign-out. Password reset requires the active session to have been signed in with email.

Token storage: an AES-encrypted file under the project's Saved directory, keyed to the machine,
user, and game — it defeats casual copying/inspection, not code running as the same user.
Implement `IFlockTokenStore` to plug in platform keychain storage.

## Analytics

Reach it with `UFlockSubsystem::GetAnalyticsProvider()` in C++, or the *Flock | Analytics* nodes in
Blueprint — `Flock Log Event`, `Flock Log Error`, `Flock Log Exception`, `Flock Record Screen View`,
`Flock Set Analytics Consent`, `Flock Flush Analytics`, and the session nodes, none of which need the
subsystem wired in. Everything below is off when **Analytics Enabled** is unticked in
*Project Settings → Plugins → Flock SDK*.

```cpp
UFlockSubsystem* Sdk = UFlockSubsystem::Get(this);

Sdk->LogAnalyticsEvent(TEXT("level_complete"),
    FFlockMetadata().Add(TEXT("level"), 3).Add(TEXT("deaths"), 0).Add(TEXT("flawless"), true));

FFlockLogDetails Details;
Details.LogicalExpression = TEXT("ItemCount >= 0");
Details.ErrorCode = TEXT("INV_DESYNC");
Sdk->LogAnalyticsError(TEXT("Inventory desynced"), Details);

Sdk->LogAnalyticsException(TEXT("Save failed"));   // callstack captured for you
Sdk->RecordAnalyticsScreenView(TEXT("MainMenu"));
```

**Logging.** `LogAnalyticsEvent` records a diagnostic, `LogAnalyticsError` a recoverable logic fault,
and `LogAnalyticsException` an exception. All three return immediately: the entry is written to disk
and delivered later, so a call is cheap and nothing is lost to a crash or a dead network. Keys in
your metadata reach the backend exactly as you write them.

`FFlockMetadata` builds the string map the wire wants without an `FString::FromInt` at every call
site — it takes ints, floats and bools directly and converts implicitly, so it drops into any call
taking metadata.

**In Blueprint**, drag off an *Extra Data* pin and the same builders appear as chainable nodes —
*Flock Metadata (Integer)*, *(Float)*, *(Boolean)*, *(String)*. Add one node per field and chain them:

```
Flock Metadata (Integer) "level" 3 → Flock Metadata (Boolean) "flawless" true → Extra Data
```

**The first node needs nothing wired into its own Metadata pin** — leaving it empty starts a fresh map,
so a single field is a single node. (*Make Flock Metadata* exists to produce an explicitly empty map;
you don't need it to begin a chain.) Each node copies the map coming in and adds its one key, so the
chain reads left to right, mixes types freely, and writes values identically to the C++ builder. Keys
are a map, so order doesn't matter — but if two nodes use the same key, the **last one wins**. Leaving
an *Extra Data* pin unconnected is fine — you get empty metadata.

On *Flock Log Error*, right-click the **Details** pin and choose **Split Struct Pin** to get Logical
Expression, Error Code, Error Data and Extra Data as separate pins.

> If a logged event never reaches the backend, check consent first: logging is **silently dropped**
> without it, and entries are spooled to disk rather than sent immediately. Call `Flock Set Analytics
> Consent (true)` once, and use `Flock Flush Analytics` to drain the spool now instead of waiting for
> the next interval.

`FFlockLogDetails` carries the optional detail on an error or exception as one named argument:
`LogicalExpression` (the invariant that failed), `ErrorCode` (yours), `ErrorData` (structured facts
about *what* was wrong) and `ExtraData` (context about *where* the player was). Leave it default when
you have nothing to add.

**You do not need a stack trace to report an exception.** Leave the trace argument off and the SDK
walks the callstack itself. Pass one only when you genuinely have something better — a script VM's
stack, say.

**Automatic exception capture.** Engine `Error` and `Fatal` log lines are reported as exceptions with
no wiring, along with hard crashes that never reach the log. Each carries the callstack from the point
of capture, with frames as `Module+0xOffset` — measured from the module base rather than the raw
address, so a frame reads the same on every run and stays symbolicatable from your build's symbols
after the fact. Function names and source lines are included when symbols are available locally.
The SDK's own categories are excluded so a failed upload cannot report itself in a loop.
Once the queue is full, further entries are dropped *before* their callstack is walked, so an error
storm stays cheap.

**Sessions** open when a player signs in and close on logout or quit, tracking duration, screen
views, pauses, and FPS. Backgrounding pauses the session; returning after **Analytics Session
Timeout** starts a fresh one. Starting a session while one is open replaces it, closing the old one
first. Bind `OnSessionStarted` / `OnSessionEnded` / `OnSessionPaused` / `OnSessionResumed` on
`GetEvents()`, or read `GetAnalyticsSnapshot()` for live metrics.

**A session end is never lost.** Every close is written to disk before it is sent, so quitting,
signing out, losing the network, or crashing outright all cost delivery time rather than the record —
whatever did not go out drains on the next flush or the next launch. Queued ends wait for a
signed-in player rather than retrying against a closed door, so a game sitting on its title screen
makes no analytics traffic at all. A run that dies with a session
open is picked up on the following launch and closed at the last moment it was known to be alive, so
a crashed session does not sit open on the backend forever. A session that could not be registered
when it started (offline at sign-in, say) registers itself when its end is finally delivered.

**Offline queue.** Entries are stored under the project's Saved directory and sent in batches on an
interval, when the app is backgrounded, or when you call Flush. A failed send keeps the batch queued
for the next attempt; the queue is capped by **Analytics Max Cached Events**, dropping oldest first.

**Crash reporting.** A run that ends without a clean quit is detected on the next launch and reported
once, classified `background_kill` (OS eviction or swipe-close) or `abnormal` (died in the
foreground), with the lost session's id, an approximate time of death, and how many unhandled
exceptions preceded it. Disabled in the editor, where stopping Play-In-Editor is not an app death.

**Consent** is a gate, not a filter. With consent withheld there is no session and nothing is
collected, not even on disk. The decision persists between runs; withdrawing it discards the session
outright — it is not reported, not queued, and no `OnSessionEnded` is raised — along with anything
already queued. Granting it opens the session that sign-in could not — so in an opt-in flow, a player
who agrees after signing in still gets a session.

```cpp
Sdk->SetAnalyticsConsent(true);            // persists; raises OnConsentChanged
const bool bOptedIn = Sdk->HasAnalyticsConsent();
Sdk->EraseLocalAnalyticsData();            // drops the queue, the decision, and any crash marker
```

Leave **Analytics Require Explicit Consent** off to collect by default (a withdrawal still applies),
or tick it for an opt-in flow where nothing is collected until you call `SetAnalyticsConsent(true)`.

> Analytics timestamps come from the device clock, so they are wrong if the player's clock is wrong.
> Session durations are unaffected — they are measured from frame deltas, not clock readings.

## Player data & templates

A **player template** defines a kind of per-player record — a currency wallet, an achievement set — with
its schema and default values. A **player data** row is one player's values for one template.

**Blueprint** — the *Flock | Player* nodes: `Flock Get My Data By Template`, `Flock Get My Data By Tag`,
`Flock Get Player Data By Id`, `Flock Get All Player Data`, `Flock Get Player Templates`, `Flock Get
Player Template By Id / By Name / By Tag`, `Flock Get Template Player Data`, and `Flock Get Player Ban`.
Read values off a returned row's **Data** pin with the *Get Data Int / Float / String / Bool / String
Array* nodes (dotted path + fallback).

**C++** — everything lives on the player provider:

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

Things worth knowing:

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

## Game commands

Commands are the only way to change a player's data: the server validates the write against the
template and answers with the whole updated row, which the SDK writes back into the player cache — so a
read straight after a write sees the new values.

**Blueprint** — the *Flock | Commands* nodes: `Flock Update Player Data`, `Flock Update Player Data
Field`, `Flock Unlock Achievement`, `Flock Add Game Funds`, and `Flock Flush Pending Commands`. Build the
values to write by dragging off the **Data** pin and chaining *Set Command Int / Float / String / Bool /
String Array* (there is also *Set Command Json* for a nested shape, and *Command Value (…)* for the
single-field node). `Flock Get Pending Command Count` drives a "syncing…" indicator.

**C++** — everything lives on the command provider:

```cpp
UFlockSubsystem* Sdk = UFlockSubsystem::Get(this);

Sdk->GetCommandProvider()->UpdatePlayerData(RowId,
    FFlockCommandData().Set(TEXT("coins"), 250).Set(TEXT("prestige"), true),
    [](TFlockResult<FFlockPlayerData> Result) { /* Result.Value is the updated row */ });

// The wallet row is resolved from the "currency"-tagged template — no id to look up first.
Sdk->GetCommandProvider()->AddGameFunds(TEXT("coins"), 250, OnComplete);
```

Things worth knowing:

- **Field names go out verbatim — use the template's names, not the ones you read back.** The SDK never
  case-transforms a write key, because only you know what the backend stores. Note that a *read* is not
  symmetric with a write here: a row's flattened data exposes `game_currencies` as `GameCurrencies` (so a
  struct can bind to it), and the dotted getters accept either spelling. A write has no such tolerance —
  the server matches the template exactly, so writing the name `Get Data Field Names` handed you is
  rejected with `game_command.template_validation_failed`. Write the name as the template declares it.
  (`FFlockPlayerTemplateSchema::SchemaJson` carries the declared names verbatim if you need to look them
  up at runtime; `Flock.SelfTest`'s commands sweep does exactly that.)
- **Values keep their type.** An int stays an int and a bool stays a bool on the wire — the builder is
  not a string map.
- **Data writes queue offline.** An update or achievement unlock made with no connectivity is saved to
  disk, applied optimistically to the cached row, and replayed in order when the connection returns —
  automatically on sign-in, on returning to the foreground, and on reconnect. The queue belongs to the
  player who made the writes and survives quitting the game.
- **Money does not.** `AddGameFunds` fails with a Connection error when the server is unreachable rather
  than queueing, and is never re-sent after an ambiguous failure (a timeout may mean the credit already
  landed). No offline grants, no double credits.
- **A rejected write is dropped, not retried forever.** If the server permanently rejects a queued write
  (a 4xx), it is discarded and the optimistic value rolled back, so it can't block everything behind it.
  A temporary failure — or an expired session — keeps the whole queue for the next attempt.

## Code generation

**Tools > Flock > Sync Schemas** fetches this game version's player templates, game configs, and shops and
generates typed Blueprint assets from them, into `Content/Flock/Generated`. There is no C++ involved: no
toolchain, no compile, no editor restart. (`Flock.SyncSchemas` does the same from the console.)

What you get:

- **A struct per template and config** with real typed pins — ints, floats, strings, bools, arrays,
  string-keyed maps, and nested objects as their own structs.
- **`FlockShopItemId`, `FlockCurrencyId`, `FlockAchievementId`** — pick an item or an achievement from a
  dropdown instead of typing an id.
- **One node per config and template** — `Get Gameplay`, `Get Currencies`, `Save Currencies`. Each carries
  its own fetch and id, and answers with `Completed` / `Failed` exec pins, the typed struct, and an
  `Error`. (These are macros, so they live in event graphs; the library is parented to Actor, covering
  Actors, ActorComponents and the Level Blueprint.)
- **One node per command** — `Purchase`, `Unlock Achievement`, `Add Funds`, each taking the matching
  generated enum. `Add Funds` also bakes your `currency`-tagged template id, so it skips the lookup the
  SDK would otherwise do at runtime.
- **A function library**: each template's and config's id as a constant, `Read …` / `Make … Update`
  conversions between a fetched row and its typed struct, and lookups turning a picked enum member into
  the id the SDK sends. Grouped under `Flock > Generated > Ids / Structs / Lookups`.
- **A content catalog asset** listing everything the backend declares — select it in the Content Browser
  to browse your content model with no code and no dashboard login.

Reading a game config is **one node** — the fetch, its id, and the conversion are all inside it:

```
Get Gameplay  ─exec─►  Completed ──►  Struct (typed)
                       Failed ──►     Error
```

Break that struct for typed pins.

### Changing a player's data

A read-modify-write is `Get <Template>` → the engine's **Set members in struct** → `Save <Template>`:

1. Place `Get Currencies` and `Save Currencies`.
2. Drag off `Get`'s **Struct** pin and add **Set members in Currencies Template**, between the two.
3. Select that node and, in the **Details** panel, tick the members you want to write. Each ticked member
   gets an input pin; type or wire the new value in.
4. Run execution through all three, wire the modified struct into `Save`'s **Struct**, and wire `Get`'s
   **Row Id** into `Save`'s **Row Id**.

`Get` hands out the row id because `Save` needs it and nothing else can supply it — it identifies this
player's row. Pass it straight through.

Two things about that middle node:

- **Unticked members are left untouched**, keeping whatever `Get` fetched. Ticking is how you say "write
  this one", so the members you never think about stay as they are.
- **`Save` sends the whole struct**, not just the members you ticked. That is why the `Get` is not
  optional: build a struct from scratch with *Make* instead and every field you didn't fill goes to the
  server as `0` or `""`, overwriting the row.

Unreal shows Set-members pins unticked by default and gives no way to change that, which is worth knowing
rather than working around — the tick is the difference between "don't change this" and "set this to
nothing".

Every generated node is built from pieces you can also use directly, if you want to assemble a chain by
hand:

```
Flock Resolve Config Data (Config Id ← Gameplay Config Id)
  → Read Gameplay Config  →  Break Gameplay Config
```

**Tools > Flock > Clean Generated** removes everything a sync wrote. Any Blueprint still referencing a
generated struct or enum is listed before anything is deleted.

For CI, a commandlet gives you an exit code:

```bash
UnrealEditor-Cmd.exe <YourProject>.uproject "-run=FlockEditor.FlockCodegen" -mode=verify -unattended -nullrhi
```

`verify` is read-only and answers `0` when the committed assets match the backend, `2` when the schema has
drifted, and `1` when it could not run at all — an unreachable backend or bad settings. Those last two are
kept apart on purpose: a network blip should not send anyone off to regenerate perfectly good output.
`-mode=sync` (the default) regenerates instead, and answers `0` or `1`.

Things worth knowing:

- **Generated members carry the names your template declares.** That is what makes an update built from a
  generated struct acceptable to the server — a field declared `game_currencies` reads back as
  `GameCurrencies`, and writing that spelling is rejected. The generated conversions hide the difference.
- **Regenerating replaces what it owns.** Treat `Content/Flock/Generated` as Flock's; a sync rewrites it.
  **Commit the folder** — your Blueprints hold hard references to the generated structs, so a teammate
  cloning a repo without them gets broken graphs until they sync.
- **Game configs are read-only.** They get a `Get …` and a `Read …`, but no `Save` and no update builder:
  configs are game-wide and changed from the dashboard, not by a client.
- **A field name that cannot be a Blueprint member** (a space, a dot, a leading digit) is skipped with a
  warning rather than renamed, because renaming it would silently produce updates the server rejects.
  Rename it on the dashboard to use it from Blueprint.
- **The editor warns when your generated assets are stale** — that is, generated for a different game
  version than the one baked into project settings. Re-sync.
- **Nothing generated is referenced at runtime except what you use**, and the catalog never is, so it
  stays out of packaged builds.

## Testing

Automation tests live beside each feature and are grouped under the `Flock.` prefix. Run them from
**Tools → Session Frontend → Automation** (filter `Flock.`), or headless:

```
UnrealEditor-Cmd.exe <YourProject>.uproject -ExecCmds="Automation RunTests Flock." -unattended -nullrhi -log
```

## Status

Shipping: the boot/init foundation, the HTTP transport, automatic version baking, the SDK event hub,
player authentication, analytics, game config (with the offline snapshot cache), the shop
(catalog, purchase, inventory), player data & templates (with bans), game commands (with the
offline queue), and typed Blueprint code generation. Not yet wired:

- **Remaining feature provider.** Assets build on the same HTTP layer and land in a later release — the
  transport, retry, typed error model, and endpoint registry they need are already in place.
- **Analytics gameplay events.** This release ships the diagnostic log API (event/error/exception) and
  purchase/transaction reporting (with the shop); custom gameplay event tracking arrives later.
- **Typed codegen for C++.** Code generation produces Blueprint assets. C++ still reads a row by dotted
  path or binds it to a `USTRUCT` you write yourself (`GetDataAs<T>`); generated C++ types would need a
  compile step and are not currently emitted.
- **A few Blueprint gaps.** Config *patch* reads (all patches, by id, by config) and configs by version
  tag are C++-only for now — in Blueprint, `Flock Resolve Config Data` already returns the patched-or-base
  values, which is what most graphs want. Provider cache clearing is also C++-only.

See [CHANGELOG.md](CHANGELOG.md) for the version history.

## License

Copyright 2022, Qwacks. All Rights Reserved.

## Support

- Documentation: https://docs.qwacks.com/sdk/unreal
- Email: support@qwacks.com
