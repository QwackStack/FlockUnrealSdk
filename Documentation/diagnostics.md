# Diagnostics

Log entries, exceptions and crashes — what went wrong, for the people who fix it.

This is the `log_event` surface. It travels separately from [Analytics](analytics.md) and is read on a
different part of the dashboard by different people. The SDK's own output in the editor and in your game's
log files is a third thing again: see [Logging & debugging](logging.md).

| | [Analytics](analytics.md) | Log events |
|---|---|---|
| Answers | What did players do? | What went wrong? |
| Read by | Design, product, LiveOps | Engineering |
| Routes | `/v1/analytics/*` | `/v1/log_event` |
| Dashboard | **Dashboards → Game Metrics** | **Diagnostics → Events** / **Diagnostics → Errors** |
| Calls | `Flock Track Event`, `Flock Record Screen View`, sessions, purchase transactions | `Flock Log Event`, `Flock Log Error`, `Flock Log Exception`, automatic exception capture |

**Do not use these to record gameplay.** A level-complete written as a log entry lands in the engineering
diagnostics stream, where nobody building a retention chart will ever find it, and it does not appear in
Game Metrics at all. Gameplay goes through [`Flock Track Event`](analytics.md#gameplay-events).

## The three kinds of entry

Which one you pick decides where the entry appears.

| Node | C++ | Recorded as | Appears on | Use for |
|---|---|---|---|---|
| `Flock Log Event` | `LogAnalyticsEvent` | `debug` | **Diagnostics → Events** | Trace and context — things that are not faults |
| `Flock Log Error` | `LogAnalyticsError` | `logic_error` | **Diagnostics → Errors** | Something wrong that did not raise |
| `Flock Log Exception` | `LogAnalyticsException` | `exception` | **Diagnostics → Errors** | A failure you caught, with its callstack |

## Blueprint

The nodes live under *Flock | Analytics* and need no subsystem wired in. All are safe no-ops before the SDK
has initialized.

![A graph showing Flock Log Event with an Event Name filled in and its Extra Data pin fed by two chained Flock Metadata builder nodes](images/analytics-log-event.png)

Build the **Extra Data** map by dragging off that pin — the same builders the C++ side uses appear as
chainable nodes: *Flock Metadata (Integer)*, *(Float)*, *(Boolean)*, *(String)*. Add one per field and chain
them:

```
Flock Metadata (Integer) "level" 3 → Flock Metadata (Boolean) "flawless" true → Extra Data
```

**The first node needs nothing wired into its own Metadata pin** — leaving it empty starts a fresh map, so a
single field is a single node. Each node copies the map coming in and adds its one key, so the chain reads
left to right and mixes types freely. Keys are a map, so order doesn't matter — but if two nodes use the
same key, the **last one wins**. Leaving an *Extra Data* pin unconnected is fine.

On *Flock Log Error*, right-click the **Details** pin and choose **Split Struct Pin** to get Logical
Expression, Error Code, Error Data and Extra Data as separate pins.

## C++

```cpp
UFlockSubsystem* Sdk = UFlockSubsystem::Get(this);

Sdk->LogAnalyticsEvent(TEXT("matchmaking started"),
    FFlockMetadata().Add(TEXT("queue"), TEXT("ranked")).Add(TEXT("party_size"), 3));

FFlockLogDetails Details;
Details.LogicalExpression = TEXT("ItemCount >= 0");
Details.ErrorCode = TEXT("INV_DESYNC");
Sdk->LogAnalyticsError(TEXT("Inventory desynced"), Details);

Sdk->LogAnalyticsException(TEXT("Save failed"));   // callstack captured for you
```

## Things worth knowing

All three return immediately: the entry is written to disk and delivered later, so a call is cheap and
nothing is lost to a crash or a dead network. Keys in your metadata reach the backend exactly as you write
them. `FFlockMetadata` builds the string map without an `FString::FromInt` at every call site — it takes
ints, floats and bools directly.

`FFlockLogDetails` carries the optional detail on an error or exception as one named argument:
`LogicalExpression` (the invariant that failed), `ErrorCode` (yours), `ErrorData` (structured facts about
*what* was wrong) and `ExtraData` (context about *where* the player was). Leave it default when you have
nothing to add.

**You do not need a stack trace to report an exception.** Leave the trace argument off and the SDK walks the
callstack itself. Pass one only when you genuinely have something better — a script's own stack, say.

> If an entry never reaches the backend, check consent first: logging is **silently dropped** without it.
> Call `Flock Set Analytics Consent (true)` once, and use `Flock Flush Analytics` to send the queue now
> instead of waiting for the next interval.

## Automatic exception capture

With **Analytics Capture Exceptions** on (the default), the SDK reports faults as exceptions with no wiring:

- **Engine `Error` and `Fatal` log lines**, each with the callstack from the point of capture. Frames read
  `Module+0xOffset` — measured from the module base rather than the raw address, so a frame reads the same
  on every run and can still be matched to your build's symbols afterwards.
- **Blueprint script exceptions** — Accessed None, a missing property, a runaway loop — carrying the
  Blueprint call stack that locates the node. Breakpoints and tracepoints belong to the debugger and are
  never reported.
- **Hard crashes** that never reach the log.

Each report carries `category` and `exception_source` (`log`, `blueprint` or `crash`); a Blueprint exception
also carries `blueprint_exception_type` (`access_violation`, `infinite_loop`, `non_fatal_error`,
`fatal_error` or `abort_execution`). A manual `Flock Log Exception` is recorded whatever this setting says.

**Categories that are never reported.** The SDK's own categories are always excluded, so a failed upload
cannot report itself in a loop. **Analytics Exception Excluded Categories** adds more; it starts with the
automation framework's, which logs a failing test as an error. Adding `LogScript` silences Blueprint
reports.

**Repeats are counted, not sent again.** An error inside a tick fires every frame. The first occurrence of a
fault is reported at once; further occurrences inside **Analytics Exception Repeat Window** (60 seconds by
default) are counted, and when the window closes one more entry says how many there were, in `repeat_count`,
with the window length in `repeat_window_seconds`. Numbers and addresses inside a message do not make two
occurrences different faults. Set the window to 0 to report every occurrence. An exception you log yourself
is always reported.

Listening for Blueprint exceptions stops the engine writing its own script call-stack line to the log, so
when nothing else was listening, the SDK writes that line back.

Once the queue of captured faults is full, further ones are dropped *before* their callstack is walked, so
an error storm stays cheap.

## What a build can see

Shipping and Test builds compile error log lines and `ensure()` reports out, and a game built on an
installed engine cannot turn logging back on. Those builds still report Blueprint script exceptions and
crashes; the engine's runaway-loop detection for Blueprints is compiled out of them too.

`Flock Get Exception Capture Coverage` (`GetExceptionCaptureCoverage`) says exactly what the running build
can see. A build that cannot see everything also sends one `exception_capture_limited` entry to
**Diagnostics → Events** — once per build, not on every launch — naming its configuration and each kind of
fault it can and cannot see.

**Playtest builds should use the Development configuration.**

## Crash reporting

A run that ends without a clean quit is detected on the next launch and reported once, classified
`background_kill` (OS eviction or swipe-close) or `abnormal` (died in the foreground), with the lost
session's id, an approximate time of death, and how many exceptions preceded it. Disabled in the editor,
where stopping Play-In-Editor is not an app death.

## Delivery

Log entries follow the same delivery rules as gameplay events: written to disk first, sent in batches, kept
through any length of time offline, and dropped only when the server refuses them or has answered 50 sends
without taking them. See [Delivery and the offline queue](analytics.md#delivery-and-the-offline-queue).

---

[← Back to the README](../README.md)
