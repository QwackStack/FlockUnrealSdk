# Flock Unreal SDK

The Flock Unreal SDK provides access to Flock's game backend services from Unreal Engine games.

> **Early release.** The SDK's boot/init foundation, the network transport (HTTP client, automatic
> retry, typed errors), and automatic edit-time Game Version baking now ship. The feature providers
> (authentication, config, player data, shop, analytics, …) build on this next. See [Status](#status).

## Contents

- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Setup](#setup)
- [Initialization](#initialization)
- [Accessing the SDK](#accessing-the-sdk)
- [SDK events](#sdk-events)
- [Logging & debugging](#logging--debugging)
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

To watch the whole boot/init flow narrate itself without a backend, run the console command (development
builds):

```
Flock.SelfTest
```

## Testing

Automation tests live beside each feature and are grouped under the `Flock.` prefix. Run them from
**Tools → Session Frontend → Automation** (filter `Flock.`), or headless:

```
UnrealEditor-Cmd.exe <YourProject>.uproject -ExecCmds="Automation RunTests Flock." -unattended -nullrhi -log
```

## Status

The boot/init foundation, the HTTP transport, and automatic version baking now ship. Not yet wired:

- **Feature providers.** Authentication, config, player data, shop, and analytics build on the HTTP layer
  and land in later releases — the transport, retry, typed error model, and endpoint registry they need
  are in place.
- **Blueprint call nodes.** The generic HTTP client is C++-only (it's templated); typed Blueprint async
  nodes arrive per-operation with the providers. The error and model types are already Blueprint-ready.

See [CHANGELOG.md](CHANGELOG.md) for the version history.

## License

Copyright 2022, Qwacks. All Rights Reserved.

## Support

- Documentation: https://docs.qwacks.com/sdk/unreal
- Email: support@qwacks.com
