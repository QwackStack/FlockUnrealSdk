# Flock Unreal SDK

The Flock Unreal SDK provides access to Flock's game backend services from Unreal Engine games.

> **1.48.0.** Everything documented below ships today. Requires Unreal Engine 5.4 to 5.8 and a C++ compiler: the plugin
> ships as source, so a Blueprint-only project is converted once into a C++ project (see
> [Blueprint-only projects](#blueprint-only-projects)). Your game itself needs no C++: every feature has Blueprint nodes,
> apart from the few calls listed under [Status](#status).

## Contents

- [Features](#features)
- [Installation](#installation)
  - [Blueprint-only projects](#blueprint-only-projects)
- [Requirements](#requirements)
- [Setup](#setup)
  - [Baking the Game Version](#baking-the-game-version)
  - [Checking your credentials](#checking-your-credentials)
  - [While playing](#while-playing)
  - [Push notifications: dashboard and project setup](#push-notifications-dashboard-and-project-setup)
- [Initialization](#initialization)
  - [Automatic](#automatic-default)
  - [Manual](#manual)
- [Quick Start](#quick-start)
- [Accessing the SDK](#accessing-the-sdk)
- [Feature guides](#feature-guides)
- [Error handling](#error-handling)
- [Offline caching](#offline-caching)
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
- **A setup panel that tells you what's wrong** — a dockable Flock tab listing every setup problem with
  the button that fixes it, mirrored as a banner on the settings page. It opens itself only when
  something needs doing, never blocks the editor, and stays silent on a correctly configured project.
  **Test Connection** names the specific credential the backend rejected. During Play In Editor the same
  tab shows live SDK state — signed-in player, analytics session, offline queue, connectivity.
- **HTTP transport** — an instance HTTP client over the engine HTTP module with automatic retry
  (exponential backoff + jitter, `Retry-After` aware) and a callback + result surface (no C++
  exceptions). JSON is (de)serialized to/from your `USTRUCT` models, unwrapping the backend envelope.
- **Typed errors** — every failure is an `FFlockError` with a typed `EFlockErrorType`, the server's
  `EFlockErrorCode`, and the server's human-readable message (`ServerMessage`) — Blueprint-ready,
  with `UFlockErrorLibrary` exposing display text and coded-error group checks to Blueprint.
- **SDK events** — a single hub (`GetEvents()`) for lifecycle, auth, session, notification, consent and
  real-money purchase events, Blueprint-assignable, with auto-init-safe `CallOrRegister` entry points for
  the init events.
- **Authentication** — email/device/Google/Apple/Steam login and registration, a device id the SDK makes once
  and keeps, encrypted token persistence, automatic session restore, silent token refresh, and the account flows.
- **Analytics** — a diagnostic log API, session tracking, automatic exception capture, an offline
  queue that survives crashes, consent gating, and next-launch crash reporting.
- **Game config** — fetch configs and patches (by id, by name, by tag, by version tag) and resolve a
  config's effective values (patch for this game version, else its base data; with a player signed in, a gameplay or
  feature config's values as the server resolves them for that player). Read values in Blueprint
  with typed accessors by dotted path, or bind them to your `USTRUCT` in C++. Plus the game record and
  version info.
- **Shop** — browse shops and items (all shops, by id or name, a single item, a shop's items), buy an
  item for a player, read a player's inventory, count what they own, check one item, and use one up. Each item says
  whether it costs real money and gives its product id in each store, and a purchase the player made in the App Store or
  Google Play is redeemed for the item it bought. Catalog reads are cached and snapshot-backed; a purchase is money-safe
  (sent again only under its one key, so never bought twice; never queued; an answer that never came reads as "may have
  been bought") and reports the transaction for revenue metrics (a store purchase's revenue is recorded by the server);
  inventory is always fetched fresh.
- **Player data & templates** — read a player's saved data (by row id, a page of rows, or the signed-in
  player's row for a template by the template's id or tag), browse the player templates that define those
  records, and check a player's ban status. A player's rows are paginated once and cached; templates are
  cached and snapshot-backed.
- **Game commands** — the server-validated way to change a player's data: replace a data row (every field
  its template declares, nullable ones included) or write a single field onto it, unlock an achievement, or add funds to a wallet. Every command returns
  the updated row and writes it back into the player cache. Data writes queue offline and replay
  automatically; funds never do — a grant fails rather than being queued, and is never re-sent after an
  ambiguous failure.
- **Assets** — list the assets for your game version, look one up by name or id, and download it as a
  texture, as text, as raw bytes, or as a file. Transfers stream to disk rather than into memory, land in
  a size-budgeted on-disk cache keyed by asset and version, and recover on their own when a signed
  download link expires. Preload warms the cache for a loading screen.
- **Leaderboards** — read a board's configuration, its ranked standings, the signed-in player's own
  placement, and the entries either side of them. Boards are addressed by name, and reads are cached and
  snapshot-backed so a leaderboard screen survives an outage. Read-only by design: a board ranks a
  player-data field, so a score is submitted by writing that field with a game command.
- **Notifications** — a per-player inbox with unread counts and read/mark-all-read, reminders your game
  schedules and cancels server-side, the readable template catalog, and push device-token registration.
  Scheduling is addressed by template **name**. Inbox reads are snapshot-backed; read receipts are never
  queued offline, because a receipt replayed later would mark messages the player never saw. The SDK
  **registers** a push token rather than fetching one — see the dashboard setup below.
- **Account linking** — attach more than one credential to the same player, so a guest who started on a
  device can add an email or a social login and keep their progress on a new phone. Read the list, attach
  email, device, Google, Apple, Steam, Facebook or Discord, and detach again. Every call answers with the
  player's full updated credential list, so a link doubles as a refresh; the server refuses to remove the
  last credential, and a credential already owned by another player surfaces as an error rather than
  silently merging accounts.
- **Parties** — players group up by invite code before they search for a match together: create, join, get the
  player's own party back after a restart, and, as its leader, remove players, hand over the lead, change its size and
  settings, or disband it. The SDK keeps a held party current on its own, and raises an event when its members, its
  leader or its end change; an older reading never undoes a newer one.
- **Sessions** — one player hosts a game the others join by code: host, join, read a session or the one the player is
  seated in, and, as its host, publish how to reach it, hand hosting over or end it; join tokens let the host check who
  connects. The SDK keeps the seat with heartbeats whose answers keep the session current, raises an event when its
  players, its host, its connection or its end change, and gives the seat up when the game quits.
- **Code generation** — one menu click turns your backend's templates, configs, and shops into typed
  structs, enums, and one-node reads, writes, and purchases. Blueprint by default, with no toolchain and
  no compile step; switch the target to emit a generated C++ module instead.
- **Offline snapshot cache** — successful config, game, shop-catalog, player-template, leaderboard,
  notification, and asset-list reads are cached to disk, scoped to the game version, and served when the
  network is down; toggleable in settings.
- **Pluggable logger** — route SDK breadcrumbs and errors into your own telemetry or on-screen debugger.
- **Blueprint-friendly** — nearly every provider call is a self-contained async node (the few C++-only
  ones are listed under [Status](#status)), and the fire-and-forget calls plus auth/session state are
  one-node too (no "Get Flock Subsystem" needed). Events, error types, and structured data all read from
  Blueprint.

## Installation

**Download: [github.com/QwackStack/FlockUnrealSdk/releases](https://github.com/QwackStack/FlockUnrealSdk/releases)**

Get the plugin into your project one of two ways:

- **Extract the release zip** into your project's `Plugins/` folder. The archive nests everything under
  `FlockUnrealSdk/`, so you end up with `YourProject/Plugins/FlockUnrealSdk/` and nothing to rename.
- **Clone the repo** into the same place, if you want to track it with git:
  `git clone https://github.com/QwackStack/FlockUnrealSdk.git YourProject/Plugins/FlockUnrealSdk`

Then, either way:

1. Right-click your `.uproject` → **Generate Visual Studio project files**.
2. Rebuild the project. The plugin's three modules compile with it: `Flock` (runtime, the whole SDK),
   `FlockPlatformStore` (runtime, real-money purchases through the platform's store) and `FlockEditor`
   (editor only, the setup panel and code generation).
3. The plugin is enabled by default — verify under **Edit → Plugins → Online Platform → Flock SDK**. It
   turns on two engine plugins it needs: **OnlineSubsystem** (for the platform's store) and
   **DataValidation** (for its editor checks).

Each release is built from the tagged source, so it contains no compiled binaries: you build it once with
your project, against your engine version and toolchain.

**Calling Flock from C++.** Add `"Flock"` to your game module's `Build.cs`; `PrivateDependencyModuleNames` is
enough. A game written only in Blueprint needs no `Build.cs` line, though a Blueprint-only project still has to be
converted once (below). That covers every call a game makes, the typed reads
`GetDataAs<T>` and `GetAs<T>` included. Only a module that builds its own requests on Flock's HTTP layer (a
subclass of `FFlockProviderBase`, or `FFlockHttpClient`'s request templates) also adds `"Json"` and
`"JsonUtilities"`.

**Optional: Protokite Playtest (beta).** Releases also carry `ProtokitePlaytest-<version>.zip`, a separate plugin for
Protokite playtests. Extract it next to the Flock SDK, so that `Plugins/ProtokitePlaytest/` sits beside
`Plugins/FlockUnrealSdk/`, then enable it under **Edit → Plugins**. It stays off until **Enable Playtesting** is
turned on in **Project Settings → Plugins → Protokite Playtest Settings**. If you cloned the repository, copy or link
`Plugins/FlockUnrealSdk/OptionalPlugins/ProtokitePlaytest/` to `Plugins/ProtokitePlaytest/`: Unreal does not load it
where it sits. Leave it out of projects that are not running playtests. [Playtesting with Protokite](Documentation/playtesting.md)
walks through the rest, from pointing the build at a playtest to a session on Protokite's Sessions page.

Real-money purchases through Google Play (UE 5.8 or later) and the App Store, and through a web checkout on Windows, Mac and
Linux, need nothing more to install: they are part of Flock. [Real-money purchases](Documentation/real-money-purchases.md) covers the project settings and `UFlockStore`.

To see what a playtest recording looks like before any playtest exists, tick **Record Video In Play In Editor** in
**Project Settings → Plugins → Protokite Playtest Local Settings** and press Play, or type `ProtokitePlaytest.RecordTestVideo 30`
in the console of a Development build. The video is saved under `Saved/ProtokitePlaytest/Recordings/TestVideos/`, and the
log names the file, which is a WebM video a browser plays. Video is recorded on 64-bit Windows, with the **Video Recording** settings in **Protokite
Playtest Settings**. Recordings are kept inside **Recordings Disk Budget**, and the oldest test videos make room first.

### Blueprint-only projects

**Yes, a Blueprint-only project must be converted once, and no, you never write C++.** Flock is a **code plugin**: it
ships C++ source and no binaries, so something has to compile it. A Blueprint-only project has no `Source/` folder and
nothing to compile it *with*, so adding Flock makes your project a C++ project.

You never have to write any C++ — your game stays entirely in Blueprint, and the conversion itself is a
couple of clicks. What it does change is that the project needs a compiler from then on, which is worth
understanding before you start rather than after.

**1. Install the C++ toolchain.** Unreal does not bundle a compiler. On Windows you need
**Visual Studio 2022** with the **Game development with C++** workload, which pulls in the MSVC compiler
and the Windows SDK. In the Visual Studio Installer, that workload also offers an **Unreal Engine
installer** optional component — tick it; it wires up the pieces Unreal expects. Community edition is
free and sufficient. (Verified against Visual Studio Community 2022 17.14.) On macOS this is Xcode with
its command-line tools; on Linux, the engine's bundled clang toolchain.

**Worth knowing before you commit to this:** the toolchain is needed by *everyone on the team who builds
the project*, not just whoever installs the plugin. Compiled output lives in `Binaries/`, which is
normally excluded from source control, so each machine builds its own copy. On a team with more designers
than programmers, that is the real cost of a code plugin — not the one-time conversion below.

**2. Give the project a `Source/` folder.** With Flock in `Plugins/`, open the project and use
**Tools → New C++ Class**, pick `None` as the parent, and accept the defaults. This is the standard way
to convert a Blueprint project: it creates `Source/` with the module and target files Unreal needs, then
compiles and reopens. The class it adds is an empty placeholder you can ignore — you are doing this for
the `Source/` folder, not for the class.

If you skip this step, the editor will notice the missing module on the next open and offer to rebuild.
That prompt works, but it fails with a generic message when no toolchain is installed, which is why the
order above is toolchain first.

**3. Build once**, either from the editor's rebuild prompt or by right-clicking the `.uproject` →
**Generate Visual Studio project files** and building from your IDE. After that, Flock behaves like any
other plugin; you only rebuild again when you change engine version or update the plugin.

**Is a no-toolchain option coming?** It's planned, not available. A plugin can be consumed without any
compiler when it ships binaries already built against your exact engine version, and that means producing
and verifying a separate build per supported engine. Flock is deliberately not claiming support it hasn't
compiled, so until that pipeline exists, the toolchain is required. No date — when it lands it will be
announced on the releases page.

> **Codegen note.** Flock's schema codegen has two targets, and the default — **Blueprint** — is the one
> to keep. It emits Blueprint assets with no compile step and works in any project. The **C++** target
> writes a generated module and needs `Source/` to already contain a build target, so on a project that
> has never been converted it refuses with: *"This is a Blueprint-only project, so a C++ module cannot be
> added to it."* That is deliberate — it declines rather than silently restructuring your project.

## Requirements

- **Unreal Engine 5.4 to 5.8.** Every version in the range is built *and* run against the full test
  suite before a release — the range is what has been verified, not what is expected to work. Older engines are refused with a single clear error. Newer ones
  compile and emit a warning saying they are past what has been verified: a new engine usually breaks
  nothing, so the SDK does not block you on the day one ships. See [CHANGELOG.md](CHANGELOG.md) for how
  the range moves.
- **Known engine issue on 5.4 and 5.5: a Development package ends with an access violation (`0xC0000005`)
  about 60 seconds after it quits.** The game has already closed by then, and its log is complete. This is
  an engine bug, not this plugin's: as a packaged game shuts down, the engine releases Nanite's resources
  after a lock they use has been destroyed, and its crash handler then waits 60 seconds before the process
  ends. It happens whether or not any Flock feature ran. Epic fixed it in 5.6. It was measured on 5.5
  Development packages; 5.4 has the same engine code. Shipping packages leave out the part involved, so
  they should not be affected, though that has not been measured. If a script judges a packaged run on
  these engines, read the game's own exit lines in its log rather than the process's exit code.
- **Platforms: verified on Windows (Win64), and on Android with UE 5.4.** The SDK is written against portable
  engine APIs and uses no platform-specific code, so other targets are expected to work — but expected is not
  verified, and this project states only what it has actually run. Windows is where the test suite has been
  executed; an Android phone built with UE 5.4 has played multiplayer through Flock's relay and a direct path
  (1.38.1). macOS, iOS, Linux, console, and Android with UE 5.5 or later have not been exercised. Nothing restricts them: the plugin declares
  no platform allow-list, so it builds and packages for any target your project supports. If you ship on
  one of them, treat the first run as something to check rather than assume, and please report what you
  find — that is how this line gets shorter.
- **A C++ toolchain on every machine that builds the project** — Visual Studio 2022 with the
  *Game development with C++* workload on Windows, Xcode on macOS. The plugin ships source rather than
  binaries, and build output isn't usually committed, so each developer compiles their own copy.
- **A C++ project.** Blueprint-only projects work fine, but need converting once — see
  [Blueprint-only projects](#blueprint-only-projects) above. No C++ authoring is required afterwards.

## Setup

The first time you open a project with the plugin installed, the **Flock** panel opens itself. It lists
whatever is missing and gives you the fields and buttons to fix it, so you can configure the SDK without
knowing where anything lives. Reopen it any time from **Tools → Flock → Flock Panel**.

It only opens on its own when there is something to act on — a broken setup, or your first time in the
project. A correctly configured project never sees it unprompted, and it is never a modal dialog.

Everything below can be done from that panel, or directly in
**Project Settings → Plugins → Flock SDK Settings**. Required values:

- **API URL** — Flock API endpoint (default: `https://api-flock.qwacks.com`)
- **API Key** — your game's API key, from the Flock dashboard
- **Game Name** — your game's name, as the Flock dashboard shows it
- **Game Version** — the name of one of your game's versions in the Flock dashboard; the matching ID is resolved at edit
  time and baked in, so init makes no network call

All three come from the Flock dashboard, where you create the game: open the game there and copy its API Key, its name and
a Game Version's name. The Unreal page of the Flock documentation, <https://docs.qwacks.com/sdk/unreal>, links to the
dashboard.

Values are written to your project's `DefaultGame.ini`.

### Baking the Game Version

The Game Version ID bakes **automatically**: it resolves whenever you fill in or change a resolve input
(**API URL** / **API Key** / **Game Version**), and once on editor startup if the project has a version
name but no baked ID. It writes the ID into `DefaultGame.ini` (the read-only **Game Version ID** field on
the settings panel), so runtime init never contacts the server. You can also force a resolve any time with
**Tools → Flock → Resolve Game Version**.

### Checking your credentials

**Test Connection** in the Flock panel makes one authenticated round-trip and reports which credential is
at fault rather than a generic failure: an unreachable API URL, a rejected API key, or a Game Version name
that doesn't exist on this game. It also reads back the game your API key belongs to and warns if your
**Game Name** disagrees with it — that name is never sent to the server, but it keys the saved-login
store, so correcting it later signs existing players out.

### While playing

The Flock panel switches to a live view during Play In Editor: whether the SDK initialized, the signed-in
player, the analytics session and consent state, how many offline commands are queued, connectivity, and a
running list of SDK events. If Flock isn't set up when you press Play, the warning names every blocking
problem and links straight to the panel.

### Push notifications: dashboard and project setup

The notification **inbox** and **scheduling** need nothing beyond the setup above. **Push delivery** needs
four things, and three of them live outside this plugin.

**1. Provider credentials, on the dashboard.** Under **Settings → Push Notifications**, configure the
providers you ship on. Credentials are per game, stored encrypted, and write-only — replaceable, never
readable back.

| Provider | Delivers to | What to supply |
| --- | --- | --- |
| Firebase Cloud Messaging (FCM) | Android and web | The full contents of the Firebase service-account JSON file |
| Apple Push Notification service (APNs) | iOS | The `.p8` signing key, its **Key ID**, your **Team ID**, the app's **Bundle ID**, and **Use APNs sandbox** for development builds |

Until a provider reads **Configured**, tokens register successfully and nothing is ever delivered on that
platform. That is the usual cause of "push is broken": the client reports success because registration
genuinely succeeded.

**2. A notification template, on the dashboard.** Under **Marketing → Templates** — a title and body, its
locale, the channels it may go out on, and any `{placeholders}` you fill at send time. Scheduling takes the
template's **name**, so a template is required before `ScheduleByTemplateName` can do anything.

**3. A way to mint the token.** The SDK registers a device token; it does not fetch one — yet. This differs
by platform:

- **iOS: nothing extra.** Unreal surfaces the APNs token itself and the backend speaks APNs directly, so no
  Firebase is involved. Bind `FCoreDelegates::ApplicationRegisteredForRemoteNotificationsDelegate`,
  hex-encode the bytes, pass the string in — about ten lines, worked example in the setup guide.
- **Android: a Firebase-based plugin, for now.** Bind its token callback and pass the string to
  `RegisterDeviceToken`. It must be a real **FCM registration token** — a vendor SDK that hands you its own
  subscriber ID will not resolve.

**Built-in acquisition is planned for both**: a one-call iOS wrapper and a first-party Android UPL shipping
the modern `firebase-messaging` binding, so push does not depend on a paid marketplace plugin. The
`RegisterDeviceToken` call you write does not change when they land — only where the string comes from.

**4. `google-services.json` in your project**, wherever your push plugin expects it — Firebase Features
wants `<YourProject>/Services/`. Leave the **Google Cloud Messaging Sender ID** field under
Project Settings → Platforms → Android **blank**: it gates Unreal's own `GoogleCloudMessaging` plugin,
which is built on APIs Google decommissioned in 2019, and blanking it also stops two registration paths
competing for the same engine delegates.

Full walkthrough with the exact download-and-place steps:
**[Testing push notifications on Android](Documentation/push-setup-android.md)**.

Push cannot be exercised in the editor, in Play In Editor, or in a Windows build — Unreal's desktop
implementation is a stub that reports "not registered" and does nothing. Branch on
`Flock Get Current Device Platform` to hide an "enable notifications" toggle where push cannot work, rather
than letting the register call fail. `Flock.SelfTest` still covers the registration call on any platform by
registering a synthetic token under `web` and unregistering it — that proves the request, response and
parse, but only a device build shows a banner arriving.

## Initialization

### Automatic (default)

With the settings above filled in, **Auto Initialize on Load** is on by default, so `UFlockSubsystem`
initializes itself when the game starts. Nothing to call — just use the subsystem when you need it.

To react to initialization (optional), in an actor whose header declares
`UFUNCTION() void HandleFlockReady();`:

```cpp
#include "FlockEvents.h"
#include "FlockSubsystem.h"

void AMyActor::BeginPlay()
{
    Super::BeginPlay();

    // Auto-init completes before BeginPlay, so use CallOrRegister: it runs the callback at once when the
    // SDK is already initialized, otherwise when initialization succeeds.
    if (UFlockSubsystem* Flock = UFlockSubsystem::Get(this))
    {
        FFlockInitializedCallback OnReady;
        OnReady.BindDynamic(this, &AMyActor::HandleFlockReady);
        Flock->GetEvents()->CallOrRegister_OnInitialized(OnReady);
    }
}
```

To drive init yourself — to start it after a splash screen or a license agreement, for example — turn **Auto Initialize
on Load** off (**Project Settings → Plugins → Flock SDK Settings**, its **Initialization** section) and call one of
the manual entry points below.

> **Init is fail-safe.** A bad config or an unresolved Game Version leaves the SDK uninitialized and
> logs the reason (it never crashes startup). Guard with `IsInitialized()`, read
> `GetInitializationError()`, or handle `GetEvents()->OnInitializationFailed`.

### Manual

From any actor (or from your game instance, with `GetSubsystem<UFlockSubsystem>()` directly):

```cpp
UFlockSubsystem* Flock = GetGameInstance()->GetSubsystem<UFlockSubsystem>();

// From project settings:
Flock->InitializeFromSettings();

// ...or from a config you fill in (for tests, or a startup of your own):
FFlockInitConfig Config;
Config.ApiUrl = TEXT("https://api-flock.qwacks.com");
Config.ApiKey = TEXT("...");
Config.GameId = TEXT("my-game");
Config.GameVersion = TEXT("1.0.0");
Config.GameVersionId = TEXT("...");   // baked at edit time
Flock->InitializeWithConfig(Config);
```

## Quick Start

The shortest path from zero to reading data — the SDK initializes itself, so sign a player in and read
something back. Every feature follows the same shape; see the [Feature guides](#feature-guides).

A device needs an account before it can log in: on the first launch, logging in alone fails with
`player.invalid_login_credentials`. So the quick start registers the device first. Registering a new
device signs the player in; registering one that already has an account is a success that says so
(`bAlreadyRegistered`), and then the player logs in.

Sign in only when nobody is signed in. A saved sign-in is restored while the game starts, before any `BeginPlay`, so a
returning player often is signed in already, and a second sign-in ends the session the first one started.

The device id is left empty, so the SDK uses its own: a GUID it makes once and keeps in `Saved/Flock/device_id.txt`
(`ThisDeviceId`; see [Device ids](Documentation/authentication.md#device-ids) for when to pass an id of your own).

**C++** — add `"Flock"` to your module's `Build.cs` (see [Installation](#installation)), then, in an actor placed in your
level whose header declares `virtual void BeginPlay() override;`:

```cpp
#include "FlockSubsystem.h"

// What this quick start does once a player is signed in: read the game's record and print its name.
void PrintFlockGameName(UFlockSubsystem* Flock)
{
    Flock->GetGameProvider()->GetGame([](TFlockResult<FFlockGameSchema> Game)
    {
        if (Game.bSuccess)
        {
            UE_LOG(LogTemp, Log, TEXT("Playing %s"), *Game.Value.Name);
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("%s"), *Game.Error.ToDisplayText());
        }
    });
}

void AMyActor::BeginPlay()
{
    Super::BeginPlay();

    // 1. With Auto Initialize on Load on (the default), the SDK is already running by BeginPlay.
    UFlockSubsystem* Sdk = UFlockSubsystem::Get(this);
    if (!Sdk || !Sdk->IsInitialized())
    {
        return;
    }

    // 2. Signed in already (a saved sign-in, restored while the game started)? Then sign in no more.
    if (Sdk->IsAuthenticated())
    {
        PrintFlockGameName(Sdk);
        return;
    }

    // 3. Register this device (an empty id: the SDK's own). On the first launch that creates the player and signs them in.
    const TWeakObjectPtr<UFlockSubsystem> WeakSdk(Sdk);
    Sdk->GetAuthProvider()->RegisterWithDevice(FString(), FString(),
        [WeakSdk](TFlockResult<FFlockRegisterResult> Registered)
        {
            UFlockSubsystem* Flock = WeakSdk.Get();
            if (!Flock || !Flock->IsInitialized())
            {
                return;
            }
            if (!Registered.bSuccess)
            {
                UE_LOG(LogTemp, Warning, TEXT("%s"), *Registered.Error.ToDisplayText());
                return;
            }
            if (!Registered.Value.bAlreadyRegistered)
            {
                PrintFlockGameName(Flock);   // 5. a new player, signed in by registering
                return;
            }
            // 4. A later launch finds this device registered (a success, with bAlreadyRegistered set): log in to it.
            Flock->GetAuthProvider()->LoginWithDevice(FString(),
                [WeakSdk](TFlockResult<FFlockPlayerLoginResponse> Login)
                {
                    UFlockSubsystem* SignedIn = WeakSdk.Get();
                    if (!Login.bSuccess)
                    {
                        UE_LOG(LogTemp, Warning, TEXT("%s"), *Login.Error.ToDisplayText());
                    }
                    else if (SignedIn && SignedIn->IsInitialized())
                    {
                        PrintFlockGameName(SignedIn);   // 5. read something back
                    }
                });
        });
}
```

The callbacks hold the subsystem weakly (`TWeakObjectPtr`), never `this` or a raw pointer: an answer can
arrive after the level or the SDK is gone.

**What success looks like:** press Play, and the Output Log (**Window → Output Log**) shows `LogTemp: Playing` followed by
your game's name. A failure shows the error and its `Fix:` line as a `LogTemp` warning.

**Blueprint** — no C++ to write. Put the nodes in your level's Level Blueprint (the **Blueprints** button in the level
editor's toolbar → **Open Level Blueprint**), or in any actor placed in the level. In the order they run:

1. On **Event BeginPlay**, call a custom event, *Sign In With This Device*.
2. *Sign In With This Device* starts with a Branch on **Flock Is Authenticated**. On True a player is signed in already:
   there is nothing to do.
3. On False, run **Flock Register With Device** with its *Device Id* left empty: the SDK uses this install's own id, made
   once and kept ([Device ids](Documentation/authentication.md#device-ids)). A new device is registered **and signed in** in
   one step.
4. A device that already has an account also arrives on **On Success**, with **Already Registered** set and nobody signed
   in yet: break the result, branch on it, and run **Flock Login With Device**, its *Device Id* empty too, only on True.
   Wiring On Success straight into Login signs the player in twice.
5. From each **On Failure**, print the *Error* through **To String (Flock Error)**.

No node needs a Target pin; each resolves the SDK from the graph it is in. Once signed in, any other node works, for
example `Flock Get Game`.

![A custom event Sign In With This Device, with a Device Id input, runs a Branch on Flock Is Authenticated: True prints Already signed in; False runs Flock Register With Device, whose On Success breaks the result and branches on Already Registered: True runs Flock Login With Device and prints Signed in, False prints Registered and signed in; both On Failure pins print the error through To String (Flock Error)](Documentation/images/auth-register-then-login.png)

**What success looks like:** press Play, and the Print String nodes write *Registered and signed in* (the first launch),
*Signed in* (a later launch) or *Already signed in* (a saved sign-in) on the screen, and on a `LogBlueprintUserMessages`
line in the Output Log. A failure prints the error and its `Fix:` line.

[Authentication](Documentation/authentication.md#blueprint) walks through the same flow step by step.

## Accessing the SDK

**C++** (with `"Flock"` in your module's `Build.cs`):

```cpp
#include "FlockSubsystem.h"

if (UFlockSubsystem* Flock = UFlockSubsystem::Get(WorldContextObject))
{
    if (Flock->IsInitialized())
    {
        const FString VersionedUrl = Flock->GetVersionedApiUrl(); // such as https://.../v1
    }
}
```

`WorldContextObject` is any object in a game world: an actor, a component, a widget. `UFlockSubsystem::Get`
is a convenience wrapper over `GetGameInstance()->GetSubsystem<UFlockSubsystem>()`; either works.

**Blueprint: you usually don't need to get the subsystem at all.** Every node resolves the SDK from the
graph it's called in, so a call is one node with no Target pin to wire:

- **Provider calls** are async nodes with success/failure pins — `Flock Login With Email`,
  `Flock Get Config By Name`, `Flock Get Shop Items`, `Flock Get My Data By Tag`, `Flock Purchase`, … .
  Each fires exactly one pin, and fails with a Validation error if the SDK isn't initialized.
- **Fire-and-forget calls and state reads** are plain nodes — `Flock Log Diagnostic Event`, `Flock Record Screen
  View`, `Flock Set Analytics Consent`, `Flock Is Authenticated`, `Flock Get Player Id`, `Flock Logout`,
  `Flock Is Initialized`, `Flock Get Events`, … . All are safe no-ops (or return defaults) before init,
  so they never need an "is ready" guard.

Search the node menu for **Flock** to see the full set. The subsystem is still there via **Get Flock
Subsystem** if you prefer a Target pin, and you *do* need it for one-time setup — `Initialize From
Settings`, `Initialize With Config`, and `Shutdown Sdk`, which are deliberately not duplicated as
free-floating nodes.

## Feature guides

Per-feature usage lives in its own guide. Each separates the **Blueprint** surface from the **C++** one,
so you can read only the half you work in.

| Guide | Covers |
|-------|--------|
| [Authentication](Documentation/authentication.md) | Login/register for every provider, logout, token refresh and revocation, password reset, email verification, account linking |
| [Game config & metadata](Documentation/game-config.md) | Resolving effective values, configs and patches, feature flags, game/version records |
| [Player data & templates](Documentation/player-data.md) | Reads by id/template/tag, the wallet by currency code, templates, pagination, bans |
| [Game commands](Documentation/game-commands.md) | Updating a row, achievements, funds, the offline queue and its money-safety rules |
| [Shop](Documentation/shop.md) | Shops and items, real-money items and their store product ids, redeeming a store purchase, purchase and its money-safety contract, player inventory, owned counts and consume one |
| [Assets](Documentation/assets.md) | Listing and resolving assets, the four download flavours, preloading, the binary cache |
| [Leaderboards](Documentation/leaderboards.md) | Boards by name, standings, a player's own rank, windows and score formatting, why there is no submit call |
| [Notifications](Documentation/notifications.md) | The inbox, scheduled reminders, the template catalog, push device tokens and what push delivery needs |
| [Android push setup](Documentation/push-setup-android.md) | Step-by-step: Firebase project, the two JSON files and where each goes, the plugin, the token hookup, dashboard credentials, and what to check when nothing arrives |
| [Multiplayer](Documentation/multiplayer.md) | Parties, sessions, matchmaking and connecting players: create or host, join by code, the party and session objects and their events, heartbeats, join tokens, searching for a match alone or as a party, Travel To Session, direct connections and Flock's relay, direct paths for relayed players, letting in only the session's players, how each ends |
| [Code generation](Documentation/codegen.md) | Sync Schemas, generated structs/enums/one-node macros, the C++ target, Clean |
| [Analytics](Documentation/analytics.md) | Sessions, screen views, gameplay events, transactions, consent — what players did (Dashboards → Game Metrics) |
| [Diagnostics](Documentation/diagnostics.md) | Log entries, automatic exception and Blueprint exception capture, crash reporting — what went wrong (Diagnostics → Errors / Events) |
| [Real-money purchases](Documentation/real-money-purchases.md) | Google Play and App Store setup, `UFlockStore` (connect, buy, pending payments, restore, prices, the setup check), what is delivered when, and the limits |
| [Playtesting with Protokite](Documentation/playtesting.md) | The optional Protokite Playtest plugin: setup, the session per launch, video, heavy analytics, exceptions, the feedback form, its logos and its Blueprint nodes, what to tell players |
| [SDK events](Documentation/events.md) | The event hub — lifecycle, auth, session, notification, consent and real-money purchase events, and when to bind each |
| [Errors](Documentation/errors.md) | What an `FFlockError` carries, branching on codes, the hint that names the fix, field-error validation failures |
| [Logging & debugging](Documentation/logging.md) | The SDK's own log output, the network call trace, the self-test |
| [Architecture](Documentation/architecture.md) | How the SDK is layered and why, the module split, the folder map, and the rules that hold across every feature |

## Error handling

Every call answers with a `TFlockResult<T>` rather than throwing — Unreal builds with exceptions off.
Check `bSuccess`, then read `Value` or `Error`. Here `gem_pack_small` is an example item id, read from its shop (an id is
not the item's name):

```cpp
UFlockSubsystem* Sdk = UFlockSubsystem::Get(this);
Sdk->GetShopProvider()->Purchase(TEXT("gem_pack_small"), [](TFlockResult<FFlockPurchaseResult> Result)
{
    if (!Result.bSuccess && Result.Error.ErrorCode == EFlockErrorCode::ShopInsufficientFunds)
    {
        // The server declined: not enough funds.
    }
});
```

`FFlockError` carries a typed `EFlockErrorType` (Auth, Validation, Network, Connection, Timeout,
Serialization, Cancelled, Store), the server's `EFlockErrorCode`, and the server's own human-readable
`ServerMessage`. `UFlockErrorLibrary` exposes display text and coded-error group checks to Blueprint, so
a graph can branch on a failure without matching strings.

Printing one is meant to be enough to act on — `To String (Flock Error)`, or
`FFlockError::ToDisplayText()`, names the call, the server's reason, the code and the next step:

```
Device login failed: Invalid credentials [player.invalid_login_credentials, HTTP 401]
Fix: This device is not registered yet. Call Flock Register With Device once to create the account, then Flock Login With Device on later launches.
```

Full details in the [errors guide](Documentation/errors.md).

## Offline caching

Successful config, game, shop-catalog, player-template, leaderboard, notification, and asset-list reads are
snapshotted to disk, scoped to the game version, and served when the server is unreachable. There are no
TTLs — an entry lives until a fresh success replaces it. Toggle it with **Enable Offline Cache**
(**Project Settings → Plugins → Flock SDK Settings**, its **Offline Cache** section).

The server is tried first, with one exception: once a request has failed because the server could not be
reached at all, the SDK serves cached reads directly for a short window instead of waiting out calls that
cannot succeed. Any answer from the server — including an error — ends that window immediately, and it
lapses on its own regardless, so the next read always re-tests the network. A slow or failing server is
not treated as being offline; only a request that never reached it is.

Bans, player inventory, and purchases are never cached: they are security or money state that can change
server-side at any moment. Player data rows are cached per player and dropped on sign-out.

## Testing

Automation tests live beside each feature: the SDK's under the `Flock.` prefix (the store module's under
`Flock.PlatformStore.`), and the optional playtest plugin's under `Protokite.`. Run them from **Tools → Session Frontend → Automation** (filter `Flock.`, or `Protokite.` for the
playtest plugin), or headless:

```
UnrealEditor-Cmd.exe <YourProject>.uproject -ExecCmds="Automation RunTests Flock.+Protokite." -unattended -nullrhi -log
```

## Status

Everything below ships today. What each version added, oldest first (the [CHANGELOG](CHANGELOG.md) has the
detail and the fixes):

| Version | What it added |
|---|---|
| 1.0.0 | **The full feature set**: the boot/init foundation, the HTTP transport, automatic version baking, the SDK event hub, player authentication, analytics, game config (with the offline snapshot cache), the shop (catalog, purchase, inventory), player data & templates (with bans), game commands (with the offline queue), assets (metadata, streamed downloads, and a binary cache), typed code generation for Blueprint or C++, and the editor setup panel with its live Play-In-Editor view |
| 1.2.0 | **Leaderboards**: boards by name, standings, and a player's own placement |
| 1.3.0 | **Notifications**: the per-player inbox, server-side scheduled reminders, the template catalog, and push device-token registration |
| 1.4.0 | **Account linking**: attaching and detaching credentials on a signed-in player |
| 1.6.0 | **The rest of notifications**: an unread-count and a notification-received event on the event hub, and the pending-schedule list (`Get Pending Schedules`, `Cancel All Scheduled`) that lets a game see and clear the reminders it created |
| 1.6.1 | **A correctness release**: leaderboard standings, rank and around-me address the board by name, which is the only form the API serves; and two data-durability fixes (see the CHANGELOG) |
| 1.7.0 | **Shop rewards**: items that grant currency or other rewards when bought, a call to consume an owned item, and reading a player's scheduled notifications back from the server. A purchase completes with the rewards granted and the updated wallet alongside the inventory row, which changed its result type |
| 1.8.0 | **Failures name the fix**: every error carries the call that failed and a next step, and a request the server rejects for a bad field names that field |
| 1.9.0 | **Queued offline writes kept** when a build ships with a new Game Version |
| 1.10.0 | **Gameplay events** (`Flock Track Event`), and Blueprint script errors such as Accessed None captured automatically, counting repeats of the same error instead of sending each one |
| 1.11.0 | **Protokite Playtest**, an optional plugin for Protokite playtests, in beta and shipped as its own download: its settings, and a status that says whether playtest work may run |
| 1.12.0 | The playtest plugin **fetches the build's playtest** from Protokite, with the playtest's feature switches and feedback form, and says why when it cannot |
| 1.13.0 | **One Protokite session per launch**, named after the launch's first Flock session and sent with the player's Steam id or a device id, ended when the game shuts down; also `OnSessionRegistered` and the **Session Platform** setting in the Flock SDK |
| 1.14.0 | **Heavy analytics** when a playtest turns it on: a performance window for every ten seconds of play, each level load, and the game's own playtest events, all through the Flock SDK's analytics |
| 1.15.0 | **Screen recording** when a playtest turns video recording on, saving VP9 video on disk on 64-bit Windows; a developer tries it in Play In Editor or with the `ProtokitePlaytest.RecordTestVideo` console command |
| 1.16.0 | **Recordings kept**: one that was not uploaded waits for a later launch, with the session it belongs to; one cut off when its game ended is finished; recordings stay inside a disk budget |
| 1.17.0 | **Recording upload** to the session each recording belongs to, including what earlier launches left |
| 1.18.0 | **The playtest's feedback form**, built from the questions the playtest publishes, with a button to upload the recording from it |
| 1.19.0 | **Blueprint nodes for the whole playtest**, including a form of your own; setup findings when Play starts; a warning for a Shipping build carrying a playtest; a crash reported once rather than three times |
| 1.26.0 | **Parties**, the first multiplayer feature: create, join by invite code, get the player's party back after a restart, and the leader's calls, with events when the party changes |
| 1.27.0 | **Sessions**: host, join by code, my session after a restart, the host's calls, join tokens, and heartbeats that keep the seat and keep the session current |
| 1.29.0 | **Matchmaking**: search a queue by its name alone or as a party, every member's game told when the party's search starts and ends, and a match handing over the session it seats the player in |
| 1.30.0 | **Connecting the players**: Travel To Session opens the host's level listening and publishes how to reach it, and takes every other player there, to the host's LAN address when both share a public address; a connection of the game's own (Steam, EOS) is published and read back the same way |
| 1.31.0 | **Checking who joins**: the host's game mode calls Flock's check from `PreLoginAsync`, which lets in only players whose Flock join token checks out, one connection a player, and says which Flock player each connection is; a player turned away learns why |
| 1.32.0 | **Multiplayer in Blueprint**: a node for every party, session and matchmaking call and for Travel To Session, a node for every value a result carries, and ready-made game modes (`AFlockGameModeBase`, `AFlockGameMode`) that let in only the session's players, with the game's own check overridable in Blueprint |
| 1.33.0 | **Flock's relay client** built into the SDK |
| 1.34.0 | **Players connected through the relay**: Travel To Session takes a player who cannot reach the host directly through Flock's relay (`Relay`: When Needed by default, Always or Never), the engine's own networking carrying it, and keeps a fresh join token on hand for a player a host's level change brings back |
| 1.35.0 | **A host change is one call**: on On Host Changed (or On Relay Lost, raised when this device's relay stops working) the game calls Travel To Session again, which does this device's part from wherever the game is, a newer travel taking over one under way |
| 1.36.0 | **The multiplayer sample**, made of Blueprints only (`Content/Samples/Multiplayer`): sign in, host, join by code, quick match and leave, following host changes |
| 1.37.0 | **From playing through Cloudflare's relay**: a relay that stops answering this device is told lost within a minute, so On Relay Lost brings the game back rather than a silent wait |
| 1.38.0 | **Direct paths for relayed players** (When Needed) once they and the host reach each other, and back to the relay when it goes quiet, the game's connection carrying on through both; a player connecting again through its relay address is back in within seconds rather than the engine's minute |
| 1.38.1 | **Android**: builds and runs there, where no name was looked up before, so the relay could not be reached; a phone on mobile data plays through Flock's relay and moves to a direct path with a host on another network |
| 1.38.2 | **Codegen deletes only what it made**: a Clean or a switch of Codegen Target keeps the game's own assets and files in the generated folders, and a Generated Content Path of a whole content folder such as `/Game` is refused |
| 1.38.3 | **The relay reached another way when a network blocks UDP to it**: UDP over IPv4, then UDP over IPv6, then TCP and TLS, each way Flock lists, so a player on a phone hotspot or an office network that drops UDP, or on a network with IPv6 alone, still hosts and joins through the relay; when no way opens, `relay_unreachable` names each way and why it failed |
| 1.39.0 | **A playtest session carries the player's sign-in** when it starts while a player is signed in |
| 1.40.0 | **The feedback form's logos**: the game's own logos, and Powered by Qwacks at its bottom left |
| 1.41.0 to 1.44.0 | **The shop's payment calls**: every payment failure's own code and fix, real-money items with their store product ids, redeeming a store purchase, the wallet read by currency code and a purchase sent again under one key |
| 1.45.0 | **Real-money items through Google Play and the App Store**: connect `UFlockStore`, and the same Purchase call charges through the store, has Flock deliver the item, and only then finishes the store's purchase |
| 1.46.0 | **Real-money items on Windows, Mac and Linux through a web checkout**: the same connect opens the payment gateway's page (Stripe, Tap or Moyasar) in the player's browser and waits for Flock's server to report the payment |
| 1.47.0 | **All of it in Blueprint**: nodes to connect the store, show prices, restore, check the setup and open a checkout, and the store's two events on Flock's events hub |
| 1.48.0 | **The shop sample**, made of Blueprints only (`Content/Samples/Shop`): every item with its price and what the player owns, buying, using, restoring and the setup check |

Deliberate omissions and known gaps:

- **No sound-wave asset download.** Unreal has no engine API for turning mp3/ogg bytes into a
  `USoundWave`, and a WAV-only helper would fail silently on most of what a CDN actually holds — so
  `Flock Download Asset File` is the documented path for audio.
- **A few surfaces are C++-only.** Config *patch* reads (all patches, by id, by config) and configs by
  version tag — in Blueprint, `Flock Resolve Config Data` already returns the patched-or-base values,
  which is what most graphs want. Recording an analytics transaction yourself (`RecordTransaction`), a custom
  logger (`SetLogger`), provider cache clearing, a web checkout's own way to show the payment page
  (`FFlockStoreOptions::OpenPaymentPage`) and a game's own store code (`UFlockStore::SetStoreLibrary`,
  `ConnectThrough`) are also C++-only.
- **Asset uploads are not included.** The SDK reads and downloads assets; publishing them is a dashboard
  operation.
- **Leave `framegrabber.framelatency` at 0 while Protokite Playtest records video.** Changing that engine console
  variable while a frame is on its way to be captured stops a Development build: the engine's frame grabber then
  flushes rendering commands from the render thread. The plugin asks for no frame while it is not 0, but cannot
  stop the change itself.

See [CHANGELOG.md](CHANGELOG.md) for the version history.

## License

MIT — see [LICENSE.md](LICENSE.md).

## Support

- Documentation: https://docs.qwacks.com/sdk/unreal
- Email: support@qwacks.com
