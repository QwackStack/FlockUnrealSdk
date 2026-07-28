# Logging & debugging

The SDK routes every breadcrumb and error through a logger. Turn on **Enable Debug Logs** in the
settings for verbose output under the `LogFlock` category; warnings and errors always surface.

## Turning logs on

Turning it on also raises the `LogFlock` category to `Verbose` so the breadcrumbs actually reach the
console — it only ever raises, so `Log LogFlock Verbose` typed at the console still works with the
setting off.

## Reading the network trace

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

## C++ — routing logs somewhere else

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

## The self-test

To watch the whole flow narrate itself — init, auth state, the local guards, then a chained
register → login → email-verification run — use the console command (development builds):

```
Flock.SelfTest
```

It initializes from your Project Settings and calls your real backend, so the first run registers a
demo player (`pUE@x.com`). Point the settings at a dev environment before running it.

---

[← Back to the README](../README.md)
