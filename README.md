# Flock Unreal Engine SDK

Runtime + editor integration for **Flock** (Qwacks). The SDK provides player
authentication, analytics sessions/events/transactions, and structured log
events for Unreal Engine games.

- **Plugin name:** Flock (`Qwack_ue_Sdk` module)
- **Version:** 1.0
- **Unreal Engine:** 5.0+
- **Modules:** `Qwack_ue_Sdk` (Runtime), `Qwack_ue_SdkEditor` (Editor)

---

## Table of Contents

1. [How It Works](#how-it-works)
2. [Installation](#installation)
3. [Initialization & Configuration](#initialization--configuration)
4. [Settings Reference](#settings-reference)
5. [Quick Start](#quick-start)
6. [Features](#features)
   - [Configuration (runtime overrides)](#configuration-runtime-overrides)
   - [Authentication](#authentication)
   - [Sessions](#sessions)
   - [Analytics Events](#analytics-events)
   - [Transactions](#transactions)
   - [Log Events](#log-events)
   - [Context Defaults](#context-defaults)
   - [Offline Caching (write-ahead spool)](#offline-caching-write-ahead-spool)
7. [Blueprint Usage](#blueprint-usage)
8. [Troubleshooting](#troubleshooting)

---

## How It Works

There is **no manual SDK initialization**. The SDK is a set of
`UGameInstanceSubsystem`s that boot automatically with your `UGameInstance`.
You configure credentials in **Project Settings**, then the only runtime step
your game performs is **authentication** — once a player logs in, the access
token propagates automatically to every feature.

| Subsystem | Class | Responsibility |
|-----------|-------|----------------|
| Config | `UQwackConfigSubsystem` | Runtime façade over project settings; resolves the game-version UUID |
| Auth | `UQwackAuthSubsystem` | Register / login / refresh / auth-test; owns tokens |
| Analytics | `UQwackAnalyticsSubsystem` | Sessions, events, transactions |
| Log | `UQwackLogEventSubsystem` | Debug / logic-error / exception logging |
| Session | `UQwackSessionSubsystem` | Automatic session lifecycle |
| Context | `UQwackContextSubsystem` | Default fields injected into every event |
| HTTP plumbing | `UQwackFlockGameSubsystem` | Internal — shared request dispatch (not called directly) |

**Token flow:** `UQwackAuthSubsystem` broadcasts `OnAccessTokenChanged`
whenever the access token changes (login, register, refresh). The Analytics,
Log, and Session subsystems subscribe to it — a fresh token automatically
drains any cached offline events and lets the auto-session start.

> An all-in-one `UQwackFlockSubsystem` also exists exposing the same
> auth/analytics/log methods on a single object. Prefer the dedicated
> subsystems below: the automatic session lifecycle and offline-spool draining
> are wired to `UQwackAuthSubsystem`, so authenticating through the combined
> facade will not trigger them.

---

## Installation

1. Copy the plugin into your project:
   ```
   YourProject/Plugins/FlockUnrealSdk/
   ```
2. The plugin is `EnabledByDefault`. If needed, enable it via
   **Edit → Plugins → search "Flock" → Enabled**, then restart the editor.
3. Regenerate project files and build (the plugin ships C++ modules).

---

## Initialization & Configuration

All configuration lives in **Edit → Project Settings → Plugins → Flock**.

### Required settings

| Field | Sent as | Notes |
|-------|---------|-------|
| **Api Url** | base URL | Defaults to `https://api-flock.qwacks.com`. Endpoint paths are appended. |
| **Api Key** | `X-Flock-API-Key` header | From your Flock dashboard. Sent on every request. |
| **Game Id** | — | Your Flock game identifier. |
| **Game Version** | `X-Game-Version-ID` header | A human-readable name (e.g. `0.1.2`). The SDK resolves it to a UUID via `GET /v1/game_version/by-name/{name}` lazily on the first request that needs it. |

### Config file equivalent

Settings persist to `Config/DefaultGame.ini` under the
`UQwackSettings` section. You can also commit them directly:

```ini
[/Script/Qwack_ue_Sdk.QwackSettings]
ApiUrl=https://api-flock.qwacks.com
ApiKey=your_api_key_here
GameId=your_game_id
GameVersion=0.1.2
EnableDebugLogs=False
AnalyticsEnabled=True
AnalyticsAutoStartSession=True
AnalyticsAutoEndOnQuit=True
AnalyticsSessionTimeoutSeconds=30
AnalyticsHeartbeatIntervalSeconds=60
AnalyticsBounceThresholdSeconds=10
AnalyticsPersistSession=True
AnalyticsTrackFps=True
AnalyticsFpsSampleIntervalSeconds=1
AnalyticsCacheFailedEvents=True
AnalyticsMaxCachedEvents=1000
AnalyticsCacheFlushBatchSize=50
```

That's the entire setup. No code-side `Init()` call is required.

---

## Settings Reference

| Setting | Default | Description |
|---------|---------|-------------|
| `ApiUrl` | `https://api-flock.qwacks.com` | Flock API base URL |
| `ApiKey` | *(empty)* | `X-Flock-API-Key` header value |
| `GameId` | *(empty)* | Flock game id |
| `GameVersion` | *(empty)* | Version name resolved to `X-Game-Version-ID` |
| `EnableDebugLogs` | `false` | Verbose SDK logging |
| `AnalyticsEnabled` | `true` | Master analytics toggle |
| `AnalyticsAutoStartSession` | `true` | Auto-start a session once auth + version are ready |
| `AnalyticsAutoEndOnQuit` | `true` | End the session on application exit |
| `AnalyticsSessionTimeoutSeconds` | `30` | Inactivity before a backgrounded session is ended |
| `AnalyticsHeartbeatIntervalSeconds` | `60` | Seconds between session heartbeat pings |
| `AnalyticsBounceThresholdSeconds` | `10` | Sessions shorter than this are flagged `is_bounce` |
| `AnalyticsPersistSession` | `true` | Persist a session marker for crash recovery |
| `AnalyticsTrackFps` | `true` | Sample FPS during the session |
| `AnalyticsFpsSampleIntervalSeconds` | `1` | FPS sample cadence |
| `AnalyticsCacheFailedEvents` | `true` | Write-ahead spool failed events to disk |
| `AnalyticsMaxCachedEvents` | `1000` | Spool cap |
| `AnalyticsCacheFlushBatchSize` | `50` | Events per flush batch |

---

## Quick Start

Dynamic delegates bind to a `UFUNCTION`. Declare a callback on a `UObject`
(GameInstance, PlayerController, etc.), bind it with `BindDynamic`, then call
the subsystem.

```cpp
#include "Qwack_ue_Sdk/Auth/QwackAuthSubsystem.h"

void UMyGameInstance::LoginWithEmail()
{
    UQwackAuthSubsystem* Auth = GetSubsystem<UQwackAuthSubsystem>();

    FFlockPlayerLoginRequest Request;
    Request.login_type = EFlockLoginType::email;
    Request.email      = TEXT("player@example.com");
    Request.password   = TEXT("hunter2");

    UQwackAuthSubsystem::FFlockOnAuthResponse Cb;
    Cb.BindDynamic(this, &UMyGameInstance::OnLoggedIn);
    Auth->LoginPlayer(Request, Cb);
}

void UMyGameInstance::OnLoggedIn(const FFlockPlayerAuthResponse& Response)
{
    if (Response.Meta.bSuccess)
    {
        // Token is already stored & broadcast — analytics/log/session light up.
        UE_LOG(LogTemp, Log, TEXT("Logged in as %s"), *Response.Auth.player_id);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Login failed (%d): %s"),
            Response.Meta.StatusCode, *Response.Meta.ErrorMessage);
    }
}
```

`OnLoggedIn` must be declared `UFUNCTION()` in the header:

```cpp
UFUNCTION()
void OnLoggedIn(const FFlockPlayerAuthResponse& Response);
```

Every response carries an `FFlockOpResult Meta` with:

```cpp
bool    bSuccess;     // 2xx
int32   StatusCode;   // HTTP status
FString ErrorMessage; // populated on failure
FString ResultJson;   // raw result payload
```

---

## Features

### Configuration (runtime overrides)

`UQwackConfigSubsystem` reads project settings but lets you override the API
URL, key, game id, and version at runtime (e.g. to point at a staging
environment). Pass an empty string to clear an override and revert to the
project setting.

```cpp
UQwackConfigSubsystem* Config = GetGameInstance()
    ->GetSubsystem<UQwackConfigSubsystem>();

Config->SetApiUrlOverride(TEXT("https://staging-flock.qwacks.com"));
Config->SetGameVersionOverride(TEXT("0.2.0-rc1")); // invalidates cached UUID

FString Url       = Config->GetApiUrl();
FString GameId    = Config->GetGameId();
bool    bResolved = Config->IsGameVersionResolved();
FString VersionId = Config->GetGameVersionId(); // empty until resolved

Config->ClearOverrides(); // back to Project Settings
```

The configured **Game Version** is a name; the server wants its UUID on the
`X-Game-Version-ID` header. Resolution (`/v1/game_version/by-name/{name}`)
runs lazily and requests queue until it lands — no action needed from you.

---

### Authentication

`UQwackAuthSubsystem` — `POST /v1/player/{register,login,token/refresh}` and
`GET /v1/player/auth-test`. Successful register/login/refresh stores the
player id, access token, and refresh token, and broadcasts
`OnAccessTokenChanged`.

Supported login types (`EFlockLoginType`): `device_id`, `email`, `google`,
`apple`, `facebook`, `steam`, `discord`.

**Register**

```cpp
FFlockPlayerRegisterRequest Req;
Req.email    = TEXT("player@example.com");
Req.password = TEXT("hunter2");
Req.name     = TEXT("DuckLord");

UQwackAuthSubsystem::FFlockOnAuthResponse Cb;
Cb.BindDynamic(this, &UMyObject::OnAuth);
Auth->RegisterPlayer(Req, Cb);
```

**Login (device id — anonymous)**

```cpp
FFlockPlayerLoginRequest Req;
Req.login_type = EFlockLoginType::device_id;
Req.device_id  = FPlatformMisc::GetDeviceId();
Req.device_type = TEXT("desktop");

UQwackAuthSubsystem::FFlockOnAuthResponse Cb;
Cb.BindDynamic(this, &UMyObject::OnAuth);
Auth->LoginPlayer(Req, Cb);
```

**Login (Steam)**

```cpp
FFlockPlayerLoginRequest Req;
Req.login_type = EFlockLoginType::steam;
Req.steam_id   = MySteamId;
```

**Refresh the token**

```cpp
FFlockPlayerRefreshRequest Req;
Req.player_id     = Auth->GetPlayerId();
Req.refresh_token = Auth->GetRefreshToken();

UQwackAuthSubsystem::FFlockOnAuthResponse Cb;
Cb.BindDynamic(this, &UMyObject::OnAuth);
Auth->RefreshToken(Req, Cb);
```

**Verify the current token**

```cpp
UQwackAuthSubsystem::FFlockOnAuthTestResponse Cb;
Cb.BindDynamic(this, &UMyObject::OnAuthTest);
Auth->AuthTest(Cb); // Response.RequesterJson holds the raw requester object
```

**React to token changes** (multicast — bind a `UFUNCTION`):

```cpp
Auth->OnAccessTokenChanged.AddDynamic(this, &UMyObject::HandleTokenChanged);

UFUNCTION()
void HandleTokenChanged(const FString& NewToken);
```

Accessors: `GetAccessToken()`, `GetRefreshToken()`, `GetPlayerId()`. You can
also set a token you obtained elsewhere via `SetAccessToken(Token)` (this also
broadcasts `OnAccessTokenChanged`).

---

### Sessions

`UQwackSessionSubsystem` runs the session lifecycle **automatically** when
`AnalyticsAutoStartSession` is on:

- **Start:** once auth + the resolved game-version UUID are both ready.
- **Heartbeat:** every `AnalyticsHeartbeatIntervalSeconds`.
- **End:** on background timeout (`AnalyticsSessionTimeoutSeconds`), on a
  login/player swap, on quit (`AnalyticsAutoEndOnQuit`), or recovered on the
  next launch if the previous run crashed (`AnalyticsPersistSession`).
- **Bounce:** flagged when duration `< AnalyticsBounceThresholdSeconds` or
  `screens_viewed <= 1` (screens are counted from `PostLoadMapWithWorld`).

Read-only state:

```cpp
UQwackSessionSubsystem* S = GetGameInstance()
    ->GetSubsystem<UQwackSessionSubsystem>();

bool    bActive   = S->IsSessionActive();
FString SessionId = S->GetActiveSessionId();
int32   Screens   = S->GetScreensViewed();
```

**Manual sessions** — set `AnalyticsAutoStartSession=false` and drive them
through `UQwackAnalyticsSubsystem`:

```cpp
FFlockSessionStartRequest Req;
Req.player_id        = Auth->GetPlayerId();
Req.platform         = TEXT("Windows");
Req.device_type      = TEXT("desktop");
Req.game_version_id  = Config->GetGameVersionId();
Req.started_at       = FDateTime::UtcNow().ToIso8601();

UQwackAnalyticsSubsystem::FFlockOnSessionStart Cb;
Cb.BindDynamic(this, &UMyObject::OnSessionStarted);
Analytics->StartSession(Req, Cb); // Response.session_id on success

// ...later:
FFlockSessionEndRequest End;
End.duration_seconds = 240;
End.screens_viewed   = 5;
End.is_bounce        = false;
End.ended_at         = FDateTime::UtcNow().ToIso8601();

UQwackAnalyticsSubsystem::FFlockOnGenericResponse EndCb;
EndCb.BindDynamic(this, &UMyObject::OnGeneric);
Analytics->EndSession(SessionId, End, EndCb);
```

The active `session_id` is cached centrally, so analytics/log events pick it
up automatically — you don't need to thread it through every call.

---

### Analytics Events

`UQwackAnalyticsSubsystem` — `POST /v1/analytics/events/single` (one) and
`/v1/analytics/events` (batch).

`PropertiesJson` is a **free-form JSON string** (empty serializes to `{}`).
SDK context defaults are merged in automatically; your keys win on conflict.

**Single event**

```cpp
UQwackAnalyticsSubsystem* Analytics = GetGameInstance()
    ->GetSubsystem<UQwackAnalyticsSubsystem>();

FFlockAnalyticsEventRequest Evt;
Evt.event_name     = TEXT("level_complete");
Evt.event_category = TEXT("progression");
Evt.timestamp      = FDateTime::UtcNow().ToIso8601();
Evt.PropertiesJson = TEXT("{\"level\":3,\"score\":1500,\"deaths\":2}");
// player_id / session_id are filled from context if left empty

UQwackAnalyticsSubsystem::FFlockOnGenericResponse Cb;
Cb.BindDynamic(this, &UMyObject::OnGeneric);
Analytics->TrackEvent(Evt, Cb);
```

**Batch events**

```cpp
FFlockAnalyticsEventsRequest Batch;

FFlockAnalyticsEventRequest A;
A.event_name = TEXT("item_pickup");
A.PropertiesJson = TEXT("{\"item\":\"sword\"}");
Batch.events.Add(A);

FFlockAnalyticsEventRequest B;
B.event_name = TEXT("enemy_killed");
B.PropertiesJson = TEXT("{\"enemy\":\"goblin\",\"weapon\":\"sword\"}");
Batch.events.Add(B);

UQwackAnalyticsSubsystem::FFlockOnGenericResponse Cb;
Cb.BindDynamic(this, &UMyObject::OnGeneric);
Analytics->TrackEvents(Batch, Cb);
```

---

### Transactions

`RecordTransaction` — `POST /v1/analytics/transactions` for monetization /
economy events.

```cpp
FFlockTransactionRequest Tx;
Tx.player_id               = Auth->GetPlayerId();
Tx.amount                  = 4.99f;
Tx.currency_code           = TEXT("USD");
Tx.shop_item_id            = TEXT("starter_pack");
Tx.quantity                = 1;
Tx.transaction_type        = TEXT("purchase");
Tx.status                  = TEXT("completed");
Tx.payment_provider        = TEXT("steam");
Tx.external_transaction_id = MyStoreOrderId;
Tx.created_at              = FDateTime::UtcNow().ToIso8601();

UQwackAnalyticsSubsystem::FFlockOnGenericResponse Cb;
Cb.BindDynamic(this, &UMyObject::OnGeneric);
Analytics->RecordTransaction(Tx, Cb);
```

---

### Log Events

`UQwackLogEventSubsystem` — structured logging to `POST /v1/log_event/single`
and `/v1/log_event` (batch). Three convenience entry points plus a low-level
one. `game_version` is filled automatically; `*Json` params are free-form JSON
strings.

**Debug**

```cpp
UQwackLogEventSubsystem* Log = GetGameInstance()
    ->GetSubsystem<UQwackLogEventSubsystem>();

UQwackLogEventSubsystem::FFlockOnGenericResponse Cb;
Cb.BindDynamic(this, &UMyObject::OnGeneric);
Log->LogDebug(TEXT("Checkpoint reached"),
              TEXT("{\"checkpoint\":\"bridge\"}"), Cb);
```

**Logic error** (a failed invariant / assertion):

```cpp
Log->LogError(TEXT("Inventory desync"),
              TEXT("client.gold == server.gold"),  // logical_expression
              TEXT("{\"clientGold\":100,\"serverGold\":80}"), Cb);
```

**Exception**

```cpp
Log->LogException(
    TEXT("Save failed"),
    TEXT("std::runtime_error: disk full"), // error_message
    TEXT("E_DISK_FULL"),                   // error_code
    TEXT("at SaveGame()\nat Tick()"),      // traceback
    TEXT("{\"slot\":2}"),                  // error_data (JSON)
    TEXT("{\"build\":\"shipping\"}"),      // extra_data (JSON)
    Cb);
```

**Low-level / batch** — full control via `FFlockLogEventRequest`:

```cpp
FFlockLogEventRequest Req;
Req.message        = TEXT("custom");
Req.timestamp      = FDateTime::UtcNow().ToIso8601();
Req.data.type      = EFlockLogEventType::debug; // debug | logic_error | exception
Req.data.extra_data = TEXT("{\"k\":\"v\"}");
Log->LogEvent(Req, Cb);

FFlockLogEventsRequest Batch;
Batch.events.Add(Req);
Log->LogEvents(Batch, Cb);
```

---

### Context Defaults

`UQwackContextSubsystem` works **automatically** — it snapshots
platform/device info, persists an install GUID, tracks gameplay time
(excluding backgrounded time), and caches the live `session_id`. These are
merged into every analytics `properties` and log `extra_data` block
(your explicit keys always win).

You rarely call it directly, but two accessors are available:

```cpp
UQwackContextSubsystem* Ctx = GetGameInstance()
    ->GetSubsystem<UQwackContextSubsystem>();

FString InstallId = Ctx->GetInstallId();
double  PlaySecs  = Ctx->GetGameplayTimeSeconds();
```

---

### Offline Caching (write-ahead spool)

When `AnalyticsCacheFailedEvents` is on, analytics and log events are written
to a disk spool **before** being sent:

- On a `2xx` (accepted) or permanent `4xx` (won't succeed on retry, except
  `408`/`429`) the entry is removed.
- On transient failures (`0`, `5xx`, network down, `408`, `429`) the entry
  stays on disk.
- When connectivity returns (a successful send) or a fresh token arrives
  (`OnAccessTokenChanged`), the spool drains in batches of
  `AnalyticsCacheFlushBatchSize`, capped at `AnalyticsMaxCachedEvents`.

Spool/marker locations under `<ProjectSaved>/Flock/`:

| Path | Contents |
|------|----------|
| `analytics_events/` | cached analytics events |
| `log_events/` | cached log events |
| `sessions/` | session markers (crash recovery) |

This is fully automatic — no API calls required.

---

## Blueprint Usage

Every subsystem method is `BlueprintCallable` and every request/response
struct is `BlueprintType`. From a Blueprint:

1. **Get Game Instance → Get Subsystem** (pick e.g. `Qwack Auth Subsystem`).
2. Build the request struct (e.g. *Make FFlockPlayerLoginRequest*).
3. Create a custom event matching the callback signature and bind it.
4. Call the node (e.g. *Login Player*).
5. In the callback, branch on `Response → Meta → bSuccess`.

The same auto-token / auto-session / offline-spool behavior applies in
Blueprint — authenticate through the **Auth** subsystem and everything else
follows.

---

## Troubleshooting

| Symptom | Cause / Fix |
|---------|-------------|
| All requests fail with `401` | No/invalid token. Authenticate via `UQwackAuthSubsystem` first. |
| Requests fail before any network call | `ApiKey` / `GameId` not set in **Project Settings → Plugins → Flock**. |
| Events seem delayed | They were spooled offline and will flush on the next successful send or token change. |
| `GetGameVersionId()` is empty | The name→UUID lookup hasn't completed yet; the SDK queues requests until it does. Confirm `GameVersion` matches a version that exists in Flock. |
| Auto-session never starts | Requires auth **and** a resolved game version; check `AnalyticsAutoStartSession` and that login succeeded. |
| Callback never fires | C++ callbacks must be `UFUNCTION()` and bound with `BindDynamic`. |
| Want verbose logs | Enable **EnableDebugLogs**; logs use the `LOG_FLOCK_GAME_SDK` / `LOG_QWACK_SDK` categories. |

---

**SDK Version:** 1.0 · **Unreal Engine:** 5.0+ · © Qwacks. All Rights Reserved.
