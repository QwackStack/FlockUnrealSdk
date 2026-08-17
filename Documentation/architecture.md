# Architecture

How the SDK is put together and why it is shaped this way. Written for anyone changing it, reviewing a
change to it, or reimplementing the same backend surface somewhere else — the last section separates the
decisions that belong to Flock from the ones that belong to Unreal.

The feature guides in this folder cover *what each surface does*. This covers what is true across all of
them.

## The decision everything else follows from

**Unreal builds with exceptions off.** A backend SDK is nothing but fallible I/O, so the usual answer —
throw on failure — is unavailable at the bottom of the stack where every call lives.

So the core is **plain C++: a callback plus `TFlockResult<T>`**, a success flag with either a value or a
coded `FFlockError`. No `UObject` appears until the outermost edge. That one constraint explains most of
what follows:

- Every asynchronous call takes `TFunction<void(TFlockResult<T>)>` rather than returning a future.
- Errors are values, so a caller decides what is fatal — `IsAlreadyRegistered()` turning a failure into a
  success is a caller's call, not the transport's.
- The whole stack below `UFlockSubsystem` is testable without an engine loop, which is why the automation
  suite runs headless and in seconds.

## Layers

Each layer knows only the one beneath it. Reading bottom-up:

| Layer | Lives in | Job | Must not know |
|---|---|---|---|
| Transport seam | `Http/FlockHttpAdapter.h` | Issue one HTTP request, hand back status + body | Anything about Flock |
| HTTP client | `Http/FlockHttpClient.h` | Verb families, envelope unwrap, tracing, the offline latch | Which feature is calling |
| Retry + errors | `Http/FlockRetryHandler.h`, `FlockError.h` | Backoff policy, coded errors, what is permanent | Any specific route |
| Provider base | `Http/FlockProviderBase.h` | Retry wrapper, silent refresh-on-401, snapshot policy | Any specific route |
| Providers | `Providers/*` | One feature's routes, caching and rules | UObjects, Blueprint |
| UObject edge | `FlockSubsystem.h`, `FlockEvents.h`, `Blueprint/*` | Lifetime, events, Blueprint nodes | Wire details |

Two consequences worth stating because they are easy to violate:

- **Request tracing lives in the HTTP client's send sites, not in providers.** Every call funnels through
  three places, so one implementation covers the whole surface. Adding a trace to a provider would cover
  one feature and imply the others were covered too.
- **Providers never touch `UObject`s.** The one exception is a weak pointer to the events hub, held by the
  providers that raise events — weak precisely because the hub's lifetime is the engine's, not theirs.

## Modules

| Module | Type | Depends on | Contains |
|---|---|---|---|
| `Flock` | Runtime | Core, CoreUObject, Engine, HTTP, Json, JsonUtilities, Projects, DeveloperSettings | The entire client surface |
| `FlockEditor` | Editor (`PostEngineInit`) | `Flock`, UnrealEd, Slate, ToolMenus, AssetRegistry, DataValidation | Setup panel, validators, version bake, codegen |

The split is one-directional and enforced by the build rules: `FlockEditor` depends on `Flock`, never the
reverse. Nothing in the runtime module includes an editor header, so a packaged game contains no editor
code at all — not by stripping, but because the dependency never existed.

`FlockEditor` loads at `PostEngineInit` rather than `Default` because it registers commandlets and menu
entries that need the engine up. That is also why the codegen commandlet must be invoked
module-qualified (`-run=FlockEditor.FlockCodegen`): commandlet lookup happens in `PreInit`, before this
module exists.

## Folder map

`Source/Flock/` splits `Public/` (consumable by a game) from `Private/` (implementation), with the same
area names on both sides:

| Area | Holds |
|---|---|
| `Http/` | Transport seam, client, retry, errors, JSON helpers, snapshot store, provider base |
| `Auth/` | Token store seam and its file implementation, JWT parsing, the auth session |
| `Providers/` | One file per feature: auth, config, game, shop, player, command, leaderboard, notification, asset, analytics |
| `Models/` | Wire structs, one file per feature |
| `Analytics/` | The independently-testable analytics parts: consent, spool, session, lifecycle pump, termination tracker |
| `Assets/` | The binary half: the download seam and the on-disk cache |
| `Blueprint/` | Async action nodes and pure function libraries — the Blueprint face of each provider |
| `Codegen/` | The runtime half of code generation: the struct binder and the content catalog |
| `Config/` | `UFlockConfig`, the project settings object |
| `Misc/` | `FlockEngineCompat.h` — **the only file allowed to contain engine-version guards** |
| `Private/Tests/` | Automation tests, `Flock.` prefix, co-located per feature; fakes in `Tests/Support/` |

`Source/FlockEditor/` mirrors the same convention under `Setup/`, `Codegen/`, `Guards/` and `Version/`.

Outside `Source/`: `Documentation/` ships (these guides), `Resources/` holds the plugin icon, `Config/`
holds default settings, and `Tooling/` holds the multi-engine build script. The last is excluded from the
release archive along with `.github/` — both verify claims about the plugin rather than being part of it.

## Cross-cutting rules

**The API has two response shapes, and picking the wrong one fails only against a real server.** Most
routes wrap their payload in `{error, response, result}`; the `/v1/player/*` auth routes and a handful of
others answer with the model at the root. The client exposes a verb family for each — `Get`/`PostJson`
unwrap, `GetRaw`/`PostJsonRaw` do not — and the choice is read off the endpoint's response schema, never
guessed. An enveloped test fixture passes against either, so this is invisible until it reaches a
backend. Test fixtures therefore mirror the real wire shape.

**Money is never retried and never queued.** A shop purchase and a funds grant post non-idempotently, so
an ambiguous failure surfaces rather than risking a double charge, and both fail outright when the server
is unreachable rather than replaying later. Everything else in the commands surface does queue offline.

**Cache and state are different things.** Read caches are disposable — clearing one costs a refetch. State
is not: the notification seen-watermark and the pending-schedule list survive `ClearCache()`, because
losing them either re-announces old notifications or strands a reminder nothing can cancel. Anything
stored per player carries the player id in its key, so a shared device cannot serve one account's data to
the next.

**Completion lambdas never capture `this`.** They capture shared references, weak object pointers, or
values. A provider that re-enters itself pins a `TWeakPtr` to itself first. This is what makes teardown
with requests in flight safe, and it is not optional — a captured `this` is a crash waiting for a slow
network.

**Engine-version guards live in exactly one file.** `Misc/FlockEngineCompat.h` holds the floor, the
ceiling, a `static_assert` below the floor and a warning above it. CI fails a change that puts a version
guard anywhere else, because a guard scattered through the source is one nobody finds when the floor
moves. Where an engine API changes, the fix is to move onto the portable API rather than add a guard.

## Testing

Automation tests live beside the code they cover and run headless in two contexts. The editor pass covers
everything; the `-game` pass runs the subset declaring `ClientContext` — the disk-touching paths and the
wire layer. **A lower count in `-game` is expected; zero is a failure.** Both contexts matter because an
editor-only run cannot see a defect that depends on the editor being absent, which is exactly how a
plugin-loading bug once survived a green suite.

`Flock.SelfTest` is a separate thing: a narrated console command that drives the real surface against a
real backend and prints each step. It proves wiring, not edge cases — guards belong in the automation
tests. It is the only way to catch the class of bug where a fixture and the live server disagree.

## What is Flock's design, and what is Unreal's

Useful when reviewing a change (is this rule mine to break?) and when reimplementing this surface on
another engine.

**Flock's design — carry it anywhere:**

- The two response shapes and the per-endpoint choice between them
- Condensed snake_case request bodies with optional members omitted rather than sent empty
- The money rules: non-idempotent purchase and funds grant, never queued offline
- The offline snapshot policy: unreachable with a cache serves the cache without a call; a cache with a
  transient failure serves the cache after one attempt; a permanent 4xx propagates regardless
- Name-first addressing for leaderboards and notification templates, with the resolved id memoized
- State vs cache, and player-scoped keys for anything per-player
- "Received" as fetch-derived — there is no realtime channel, and the SDK does not poll
- Which routes are player-scoped, read off the schema rather than inferred from an observed 401

**Unreal's mechanics — the equivalent decision, not the same code:**

| Here | Because | Elsewhere, decide |
|---|---|---|
| Callback + `TFlockResult<T>` | Exceptions are off | Whatever the language's idiomatic async + error type is |
| `USTRUCT` wire models, reflection-driven JSON | UE reflection is the serialization story | The host's serialization story |
| `UObject`s only at the edge | Reflection has a cost and a lifetime model | Where the managed/native boundary sits |
| Async nodes + pure libraries for Blueprint | Blueprint is a first-class consumer | Whether a visual-scripting face exists at all |
| Downloads stream to disk | A CDN payload must not sit whole in memory | Same constraint, different file API |
| Opaque JSON behind a reflected string | UE cannot express free-form or recursive data in a struct | Whether the host can hold a dynamic value directly |
| A pending queue rather than a semaphore | The game thread owns provider state | The host's concurrency model |

---

[← Back to the README](../README.md)
