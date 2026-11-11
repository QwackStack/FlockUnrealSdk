# Logging & debugging

The SDK routes every breadcrumb and error through a logger. Turn on **Enable Debug Logs** for verbose
output under the `LogFlock` category; warnings and errors always surface.

> This page is about the SDK's own log output. Reports your game sends to the Flock dashboard are
> [Diagnostics](diagnostics.md) (what went wrong) and [Analytics](analytics.md) (what players did).

## Turning logs on

**Enable Debug Logs** is in **Project Settings → Plugins → Flock SDK Settings**, its **General** section.
It takes effect when the SDK initializes, so restart Play after changing it.

| Enable Debug Logs | What reaches the log |
|---|---|
| Off (the default) | Warnings and errors: every failed call, with the server's reason |
| On | Also every call starting and succeeding, and every network request and answer (the trace below) |

Turning it on also raises the `LogFlock` category to `Verbose` so the breadcrumbs actually reach the
console — it only ever raises, so `Log LogFlock Verbose` typed at the console still works with the
setting off. A game that initializes the SDK itself with `InitializeWithConfig` sets the same thing with
`FFlockInitConfig::bEnableDebugLogs`.

## Reading the network trace

**Every network call is traced** while Enable Debug Logs is on, and each line names the caller — `[C++]`,
or the graph it came from — the same way provider lines do:

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

## C++ — routing logs somewhere else

Inject your own logger (to feed an on-screen debugger or telemetry, for example) by implementing `IFlockLogger`.
Your logger receives every line, Debug and Info included, whatever Enable Debug Logs says:

```cpp
#include "FlockLogger.h"
#include "FlockSubsystem.h"

class FMyFlockLogger : public IFlockLogger
{
public:
    virtual void LogDebug(const FString& Message) override { /* ... */ }
    virtual void LogInfo(const FString& Message) override { /* ... */ }
    virtual void LogWarning(const FString& Message) override { /* ... */ }
    virtual void LogError(const FString& Message) override { /* ... */ }
};
```

**Set it before the SDK initializes.** Each part of the SDK takes the logger when initialization builds
it, so a logger set afterwards receives only the subsystem's and the event hub's own lines, not the
calls' or the network trace. With **Auto Initialize on Load** on, initialization runs as the game
instance starts, before your code can reach the subsystem; so turn it off and initialize the SDK yourself
from your game instance, logger first:

```cpp
// MyGameInstance.cpp, with Auto Initialize on Load turned off.
void UMyGameInstance::Init()
{
    Super::Init();   // creates the Flock subsystem

    if (UFlockSubsystem* Flock = GetSubsystem<UFlockSubsystem>())
    {
        Flock->SetLogger(MakeShared<FMyFlockLogger>());
        Flock->InitializeFromSettings();
    }
}
```

`UMyGameInstance` is your `UGameInstance` subclass (set it as the project's Game Instance Class under
**Project Settings → Maps & Modes**). The logger stays across `ShutdownSdk` and later initializations.
`FFlockNullLogger` silences the SDK entirely.

Every SDK log line for a provider call names its caller, so a log shows where a request came from:

```
[Flock SDK] Email login [Blueprint 'bpTest'] starting...
[Flock SDK] Email login [C++] successful for player: 01KY1QNRFNZO4E2KHNGV9F175N
```

## The self-test

To watch the whole flow narrate itself — init, auth state, the local guards, then a chained
register → login → email-verification run — use the console command (development builds):

```
Flock.SelfTest
```

It initializes from **Project Settings → Plugins → Flock SDK Settings** and calls your real backend, so the
first run registers a demo player (`pUE@x.com`). Before running it, point the settings at a game you keep for testing,
never the one your players use.

---

[← Back to the README](../README.md)
