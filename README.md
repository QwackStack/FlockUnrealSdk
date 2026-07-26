# Flock Unreal SDK

The Flock Unreal SDK provides access to Flock's game backend services from Unreal Engine games.

> **Early release.** The SDK's boot/init foundation, the network transport (HTTP client, automatic
> retry, typed errors), automatic edit-time Game Version baking, **player authentication**,
> **analytics**, and **game config** (with an offline snapshot cache) now ship. The remaining feature
> providers (player data, commands, shop, assets) build on this next. See [Status](#status).

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
- **Offline snapshot cache** — successful config and game reads are cached to disk, scoped to the game
  version, and served when the network is down; toggleable in settings.
- **Pluggable logger** — route SDK breadcrumbs and errors into your own telemetry or on-screen debugger.
- **Blueprint-friendly** — the accessor, init, state, events, and error types are exposed to Blueprint.

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

**Blueprint:** use **Get Flock Subsystem** (or the built-in **Get Game Instance Subsystem** node), then
call `Is Initialized`, `Get Game Version Id`, etc. For events, call **Get Events** and bind there.

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

Reach it with `UFlockSubsystem::GetAnalyticsProvider()` in C++, or the *Flock | Analytics* nodes and
subsystem functions in Blueprint. Everything below is off when **Analytics Enabled** is unticked in
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
*Flock Metadata (Integer)*, *(Float)*, *(Boolean)*, *(String)*, plus *Make Flock Metadata* to start a
chain:

```
Make Flock Metadata → Flock Metadata (Integer) "level" 3 → Flock Metadata (Boolean) "flawless" true → Extra Data
```

Each takes a metadata map and returns one, so they chain and branch freely, and they write values
identically to the C++ builder. Leaving an *Extra Data* pin unconnected is fine — you get empty
metadata. On *Log Analytics Error*, right-click the **Details** pin and choose **Split Struct Pin** to
get Logical Expression, Error Code, Error Data and Extra Data as separate pins.

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

## Testing

Automation tests live beside each feature and are grouped under the `Flock.` prefix. Run them from
**Tools → Session Frontend → Automation** (filter `Flock.`), or headless:

```
UnrealEditor-Cmd.exe <YourProject>.uproject -ExecCmds="Automation RunTests Flock." -unattended -nullrhi -log
```

## Status

Shipping: the boot/init foundation, the HTTP transport, automatic version baking, the SDK event hub,
player authentication, and analytics. Not yet wired:

- **Remaining feature providers.** Config, player data, commands, shop, and assets build on the same
  HTTP layer and land in later releases — the transport, retry, typed error model, and endpoint
  registry they need are already in place.
- **Analytics gameplay events.** This release ships the diagnostic log API (event/error/exception);
  custom gameplay event tracking and purchase/transaction reporting arrive with the shop provider.
- **Blueprint call nodes.** The generic HTTP client is C++-only (it's templated); typed Blueprint
  async nodes arrive per-operation with each provider, as they have for auth and analytics.

See [CHANGELOG.md](CHANGELOG.md) for the version history.

## License

Copyright 2022, Qwacks. All Rights Reserved.

## Support

- Documentation: https://docs.qwacks.com/sdk/unreal
- Email: support@qwacks.com
