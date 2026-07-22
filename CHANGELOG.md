# Changelog

All notable changes to this plugin will be documented in this file.

The format is based on [Keep a Changelog](http://keepachangelog.com/en/1.0.0/)
and this project adheres to [Semantic Versioning](http://semver.org/spec/v2.0.0.html).

## [0.7.0] - 2026-07-21

### Added
- **Analytics** end to end (`FFlockAnalyticsProvider`, reached with
  `UFlockSubsystem::GetAnalyticsProvider()`): a diagnostic log API, session tracking, an offline
  spool with explicit flush, consent gating, and automatic crash reporting on the next launch.
- **Log API.** `LogEvent` / `LogError` / `LogException` record diagnostics, recoverable logic faults,
  and exceptions. All three are fire-and-forget at the call site: the entry is written to disk
  immediately and delivered on the next flush, so a call costs nothing and nothing is lost to a crash
  or a dead network. **Reporting an exception needs no stack trace** — leave it off and the SDK walks
  the callstack itself; pass one only when you have something better, such as a script VM's stack.
- **`FFlockLogDetails`** carries the optional detail on an error or exception (`LogicalExpression`,
  `ErrorCode`, `ErrorData`, `ExtraData`) as one named argument rather than four positional ones, and
  gives Blueprint a single pin. **`FFlockMetadata`** builds the wire's string map from ints, floats
  and bools directly, so attaching a level or a count no longer means converting by hand.
- **Automatic exception capture.** Engine `Error` and `Fatal` log lines become exception entries
  without any wiring, alongside a hard-crash hook for failures that never reach the log. Each one
  carries the callstack walked at the point of capture, with frames recorded as `Module+0xOffset`
  (plus function and source line when symbols are available). The offset is measured from the
  module's base, not the raw program counter, so a frame means the same thing on every run and can
  still be symbolicated from a build's symbols long after the report arrives.
  The SDK's own log categories are excluded so a failed upload cannot report itself in a loop, and a
  game can exclude its own noisy categories. Entries beyond the queue cap are dropped before the
  callstack is walked, so an error storm does not pay for reports it will not keep.
- **Sessions.** A session opens on sign-in and closes on logout or quit, tracking duration, screen
  views, pause count and time, and FPS (average/min/max). Backgrounding pauses it; returning after
  the configured timeout rotates to a fresh session instead of resuming. Raises `OnSessionStarted`,
  `OnSessionEnded` (with the reason), `OnSessionPaused`, and `OnSessionResumed`.
- **Offline spool.** Entries queue as plain JSON under `Saved/Flock/analytics/` and drain batch by
  batch on an interval, on backgrounding, or on an explicit `Flush`. A failed send leaves the batch
  queued for the next attempt; the queue is capped, dropping oldest first.
- **Crash reporting.** A run that ends without the quit path — crash, force-kill, foreground OOM,
  power loss — is detected on the next launch and reported once as an `app_termination` entry,
  classified `background_kill` (OS eviction or swipe-close) or `abnormal` (died in the foreground),
  carrying the dead session's id, an approximate time of death, and the unhandled-exception count.
  Disabled in the editor, where a PIE shutdown is not a real app death.
- **Consent.** Consent is a hard gate, not a send filter: with it withheld there is no session and
  nothing is collected, not even locally. The decision persists across runs, an explicit withdrawal
  ends the session and drops the queue, and `OnConsentChanged` reports every change. Granting consent
  opens the session that sign-in could not, so an opt-in flow still gets a session when the player
  agrees after signing in. Set *Analytics Require Explicit Consent* for that flow; leave it off to
  collect by default.
  `EraseLocalAnalyticsData` drops the queue, the consent decision, and any crash marker.
- **Blueprint**: *Flock | Analytics* nodes for Flush, Start Session, and End Session, plus
  `Log Analytics Event` / `Error` / `Exception`, `Record Screen View`, `Set Analytics Consent`,
  `Has Analytics Consent`, `Has Active Analytics Session`, `Get Analytics Session Id`, and
  `Get Analytics Snapshot` on the subsystem. Every one is a safe no-op before the SDK is initialized.
- Starting a session takes the signed-in player automatically — leave the player id empty and the
  SDK uses whoever is signed in, rather than making every caller fetch and pass it.
- **Automation tests** (`Flock.Analytics.*`, 91 in the suite): wire models and free-form key
  preservation, consent persistence and resolution, the spool's ordering/cap/persistence/resilience,
  session accounting and the lifecycle pump, crash-marker classification and folding, the log sink's
  filtering and re-entrancy guard, the provider's consent gate, flush and failure recovery, session
  lifecycle and termination reporting, and the Blueprint nodes' uninitialized-SDK guard.

### Changed
- `FFlockHttpClient` gained `PatchJsonRaw` for non-enveloped PATCH routes (session end).
- `Flock.SelfTest` now drives the analytics session lifecycle against the live backend — consent and
  session state, screen views, one entry of each log type, the queue depth, flush, session end, and a
  consent round trip — and prints the callstack of an automatically captured error so a capture
  problem is visible locally rather than only in the dashboard. It erases its local analytics data
  afterwards, so a run leaves nothing behind.

### Fixed
- **Registering an identity that already exists no longer logs errors.** The call reports success
  with `Already Registered`, but two error lines were written on the way there ("Operation failed
  after 1 attempt(s)" and "… registration failed"), so a normal first-run path looked broken in the
  log. Providers can now declare an expected failure, which drops those lines to debug; the outcome
  is still reported once as a warning. Genuine registration failures — a taken display name, a server
  error — are unchanged and still log as errors.

### Notes
- Analytics timestamps come from the device clock, so they are wrong if the player's clock is wrong.
  Session *durations* are unaffected — they accumulate from frame deltas rather than clock readings.

## [0.6.0] - 2026-07-21

### Added
- **Player authentication** end to end: email/device/Google/Apple/Steam login and registration,
  plus Facebook/Discord login via the generic route (`FFlockAuthProvider`, reached with
  `UFlockSubsystem::GetAuthProvider()`). Success adopts the returned tokens, records the sign-in
  method, and raises `OnAuthenticated`.
- **Session persistence & restore.** Tokens persist between launches in an AES-encrypted file
  bound to the machine/user and game (`FFlockFileTokenStore`; `IFlockTokenStore` is the seam for
  custom or platform-secure stores). A persisted session is restored automatically after SDK init —
  expired tokens are refreshed — surfacing via `OnSessionRestored` / `OnAuthenticated`, with the
  original sign-in method re-adopted for method-gated flows.
- **Token lifecycle.** Single-flight access-token refresh (concurrent callers share one request);
  authenticated calls that hit an auth failure silently refresh and replay once; a refresh
  rejection clears the session and raises `OnAuthExpired`, while transport failures keep the
  session so a flaky network doesn't sign the player out.
- **Account flows**: forgot/reset password (reset is gated to email-method sessions), email
  verification send/confirm, server-side token revoke (confirmation required), and an advisory
  display-name availability check.
- **Blueprint**: async nodes for every auth call with success/failure exec pins under *Flock |
  Auth* (login, register, restore session, password/email flows, revoke, refresh, name check),
  plus `IsAuthenticated` / `GetPlayerId` / `IsRestoringSession` / `Logout` on the subsystem.
- **Registration UX.** Registering an identity that already has an account completes successfully
  with an `Already Registered` flag instead of erroring; a taken display name stays an error.
- **JWT claims parser** (`FFlockJwt`) with claim-spelling fallbacks and UTC expiry handling.
- **Call origin in the log.** Every SDK log line for a provider call names its caller —
  `Email login [Blueprint 'bpTest'] starting...` versus `Email login [C++] starting...` — so a log
  shows which graph (or C++ path) made a request. Blueprint nodes tag their own dispatch; anything
  else reads as `C++`.
- **Automation tests** (`Flock.Auth.*`, 51 in the suite): JWT parse/fallbacks/rejects, token store
  roundtrip and corruption handling, auth session state/persistence/refresh (including
  single-flight), silent refresh-and-replay in the provider base, every provider route and guard,
  subsystem wiring with auto-restore, and the async nodes' uninitialized-SDK guard.

### Changed
- `FFlockJsonUtils::StructToWireJson` now emits condensed JSON (wire payloads were pretty-printed)
  and can omit top-level empty-string fields so optional request members drop off the wire.
- `FFlockHttpClient` gained `PostJson` (enveloped) and `PostJsonRaw` (non-enveloped) for
  caller-serialized bodies. The API uses both response shapes: most endpoints wrap their payload in
  `{error, response, result}`, while every `/v1/player/*` auth route returns its model at the root,
  so the auth provider and session use the raw verbs. Pick the verb family from the endpoint's
  OpenAPI response schema.
- `FFlockProviderBase::Execute` gained the silent-refresh path and a `bAllowAuthRetry` opt-out
  (used by the auth routes themselves).
- `Flock.SelfTest` now initializes from **Project Settings > Flock SDK** instead of a demo config,
  and drives a chained register → login → email-verification flow against the configured backend
  (so it registers a demo player on first run). It narrates auth state, the local guards, and each
  call's real result.

## [0.5.0] - 2026-07-16

### Added
- **SDK event hub.** `UFlockEvents`, reached via `UFlockSubsystem::GetEvents()` — lifecycle
  (`OnInitialized`, `OnInitializationFailed`, `OnShutdown`), auth (`OnAuthenticated` with
  `FFlockAuthInfo`, `OnTokenRefreshed`, `OnAuthExpired`, `OnLoggedOut`, `OnSessionRestored`), session
  (`OnSessionStarted`, `OnSessionEnded` with `FFlockSessionEndedArgs`, `OnSessionPaused`,
  `OnSessionResumed`), and consent (`OnConsentChanged`). All Blueprint-assignable, raised on the game
  thread, and debug-logged per raise with the subscriber count. Lifecycle events are live now; auth,
  session, and consent events are declared and get raised by their features as they land.
- **Late-binder-safe init callbacks.** `CallOrRegister_OnInitialized` /
  `CallOrRegister_OnInitializationFailed` fire immediately when init already happened — under
  auto-init the SDK initializes during GameInstance startup, before any Blueprint can bind, so a plain
  event binding would miss it. One-shot.
- **Event payload models.** `EFlockAuthMethod`, `FFlockAuthInfo`, `EFlockSessionEndReason`,
  `FFlockSessionEndedArgs`, plus the session wire models they carry (`FFlockSessionSnapshot`,
  `FFlockDeviceInfo`) ready for the session/analytics features.
- **Event automation tests** (`Flock.Runtime.Events.*`): lifecycle raises through the subsystem
  (including the misuse guard staying silent and subscriptions surviving re-init), CallOrRegister
  parked/immediate/one-shot behavior, and payload delivery for the feature raises.
- **Retry + pagination coverage gaps closed.** `Flock.Http.Retry.OverrideAndExhaustion` pins the
  `MaxRetriesOverride = 0` single-attempt contract (the offline layer's "one attempt, then serve
  cache" path depends on it) and that the last failure propagates once the budget is exhausted;
  `Flock.Http.Client.Paginated` drives `GetPaged` through the transport seam (success, status→error
  mapping, and missing-items bodies).

### Changed
- **Init events moved into the hub.** `UFlockSubsystem::OnFlockInitialized` /
  `OnFlockInitializationFailed` are now `GetEvents()->OnInitialized` / `OnInitializationFailed`
  (pre-1.0 breaking change — rebind in Blueprint via **Get Events**). Subscriptions are not cleared by
  `ShutdownSdk()`; they persist for re-init and release with the GameInstance.

## [0.4.0] - 2026-07-16

### Added
- **Server error message.** The human-readable message from the backend's coded-error body
  (`detail.message`) is now parsed into `FFlockError::ServerMessage`, alongside the machine-readable
  `Code`/`ErrorCode` — the server's wording of the failure, ready to show in-game, while `Message` stays
  terse for error-tracker bucketing.
- **Coded-error group check.** `FFlockError::IsAlreadyRegistered()` — true when a register/login route
  reports the identity (email / device / Google / Apple / Steam) already belongs to an account. A taken
  display name is deliberately excluded — that's a different fix for the player.
- **Blueprint error surface.** `UFlockErrorLibrary`, a Blueprint function library over `FFlockError`:
  **To String (Flock Error)** (log-friendly display text) and **Is Already Registered** (the group
  check). The struct's fields already break out in Blueprint; this covers the derived views that
  Blueprint can't reach as USTRUCT member functions.
- **Error automation tests** (`Flock.Http.Error.*`): the already-registered group (membership,
  exclusions, and library parity) and display text; the coded-error JSON and client tests now also
  cover `detail.message`.

### Changed
- `FFlockJsonUtils::ParseCodedErrorCode` is replaced by `ParseCodedError`, which returns the code and
  the message in one parse (same `detail`-first, `error.code`-fallback behavior).

### Fixed
- First full run of the automation suite against a real UE 5.5 editor surfaced and fixed two test bugs:
  the logger tests still created `UFlockSubsystem` under the transient package (the invalid-Outer ensure
  fixed for the other tests in 0.3.0), and `Flock.Editor.Lookup.Registry` still assumed the pre-0.3.0
  stub default — it now verifies the module-registered HTTP lookup and restores it instead of leaving
  the session disarmed.
- `Flock.uplugin` now declares the `DataValidation` plugin dependency `FlockEditor` already relied on
  (fixes a UBT warning).

## [0.3.0] - 2026-07-15

### Added
- **HTTP layer — the SDK network transport.** `FFlockHttpClient` (an instance client with `Get`/`Post`/`Put`/`Patch`/`Delete<T>`) over the engine HTTP module, behind an `IFlockHttpAdapter` transport seam. The C++ surface is callback + result: every call reports a `TFlockResult<T>` (value **or** error) to a `TFunction` completion, with no C++ exceptions. It unwraps the backend's `{error, response, result}` envelope into your `USTRUCT` model (and `GetPaged<T>` into `{items, total, page, limit}`), handling the snake_case ↔ PascalCase field mapping in one place.
- **Typed error model.** `FFlockError` (`USTRUCT(BlueprintType)`) carries the failure type (`EFlockErrorType`: Network / Auth / Validation / Serialization / Timeout / Connection / Cancelled), HTTP status, response body, and the server's machine-readable code as a typed `EFlockErrorCode` mirroring the backend OpenAPI `detail.code` set. Blueprint-ready, so the feature providers' async nodes surface typed errors without rework.
- **Automatic retry.** `FFlockRetryHandler` / `FFlockRetryPolicy` — exponential backoff with jitter, honoring a server `Retry-After` hint, with careful failure classification (never retry permanent 4xx, Auth/Validation/Serialization, or cancellation; non-idempotent callers retry only provably-not-processed 408/429). Backoff runs on the core ticker, so it works in-editor and at runtime.
- **Real edit-time Game Version resolve.** The stubbed `IFlockVersionLookup` from the previous release is replaced by an HTTP-backed lookup that resolves `game_version/by-name/{name}` for real. **Resolve Game Version** now bakes an actual ID, and the packaging build guard goes live.
- **Automatic Game Version baking.** The ID now bakes itself — no manual menu click. It resolves when a resolve input (**API URL** / **API Key** / **Game Version**) is edited to a valid state, and once on editor startup when the project has a version name but no baked ID. Runtime init stays synchronous and network-free (the ID is still baked into `DefaultGame.ini` and consumed directly). The manual **Tools → Flock → Resolve Game Version** action remains for a forced resolve.
- **`FlockEndpoints`** — every relative API path the SDK calls, in one place, plus a retry-only `FFlockProviderBase` that the feature providers build on.
- **HTTP automation tests** under the `Flock.Http.` group: JSON case round-trip / envelope unwrap / coded-error parse / pagination, error-code parsing, endpoint construction, client status→error mapping and deserialization, and retry classification / backoff / cancellation — driven by a new `FFlockFakeTransport` test seam.

### Changed
- **`Flock.SelfTest` is a feature smoke-run only.** Dropped the double-init and unresolved-init demos — misuse guards are test cases (already covered by the subsystem automation tests), not feature demos. The self-test now walks init → getters → shutdown.

### Fixed
- **Transient `UFlockSubsystem` now uses a valid Outer.** The self-test and subsystem tests created the subsystem under the transient package, tripping a handled "created in invalid Outer" ensure — a `UGameInstanceSubsystem` requires a `UGameInstance` Outer. They now create a throwaway transient `UGameInstance` first.

## [0.2.0] - 2026-07-14

### Added
- **Global SDK accessor + auto-init.** `UFlockSubsystem` (a `UGameInstanceSubsystem`) is created when the game starts and is the entry point to the SDK. Fetch it with `UFlockSubsystem::Get(WorldContext)` or `GetGameInstance()->GetSubsystem<UFlockSubsystem>()` (C++ and Blueprint). With **Auto-Initialize On Load** on (default), it initializes from **Project Settings → Flock SDK** at startup; otherwise call `InitializeFromSettings()` or `InitializeWithConfig()` yourself. `IsInitialized()`, `GetInitializationError()`, and the `OnFlockInitialized` / `OnFlockInitializationFailed` Blueprint events expose init state. Init is fail-safe — a bad config or unresolved version leaves the SDK uninitialized and logs the reason instead of crashing startup.
- **Synchronous, offline init with a baked Game Version ID.** Init makes no network call: the Game Version ID is resolved at edit time and baked into `DefaultGame.ini`, then consumed directly at runtime. Init fails cleanly when the ID is unresolved. `UFlockSubsystem::ApiVersion` (`v1`) and `GetVersionedApiUrl()` are the single source of truth for the API version segment (bump both together when the backend cuts a new major API version).
- **`FFlockInitConfig` + `UFlockConfig::IsValid`.** A runtime init struct built from project settings via `FromSettings()`, and a config-completeness check (API URL / API Key / Game Name / Game Version) shared by runtime auto-init and the editor guards.
- **Editor version baking (`FlockEditor` module).** **Tools → Flock → Resolve Game Version** resolves the Game Version name to its ID and bakes it into `DefaultGame.ini`. A notification-first Play-In-Editor setup guard warns when the SDK can't initialize, and a Data Validation build guard blocks packaging on an unresolved ID (toggle via **Fail Build If Version Unresolved**).
- **Pluggable logger.** `IFlockLogger`, with `FFlockUnrealLogger` (routes to the `LogFlock` category with a `[Flock SDK]` prefix) and `FFlockNullLogger` (silent) — inject your own via `UFlockSubsystem::SetLogger()` to feed an on-screen debugger or telemetry. **Enable Debug Logs** turns on verbose Debug/Info; warnings and errors always surface.
- **`Flock.SelfTest` console command** (development builds) — drives the boot/init surface and narrates each step to the log, so you can watch the flow without a backend or a baked version.
- **Automation tests**, co-located per feature and grouped under the `Flock.` prefix (run from Session Frontend → Automation): config validation, init-config mapping, subsystem init/gate/shutdown, logger routing and the null logger, version-resolver URL construction and bake, the build-guard decision, and the version-lookup stub and registry.

### Known limitations
- **Network transport is stubbed.** The edit-time version lookup sits behind `IFlockVersionLookup`; its default `FFlockStubVersionLookup` fails cleanly until the HTTP layer ships and registers a real lookup via `FFlockVersionLookupRegistry`. Until then, **Resolve Game Version** cannot resolve, and the build guard stays inert (it will not block a package) — so the SDK cannot complete a real init yet. Everything else (accessor, auto-init, gate, guards, logger) is live-wired around the stub.
- **Feature providers absent.** Authentication, config, player data, shop, analytics, and the error model are not present in this release — they build on this foundation.
