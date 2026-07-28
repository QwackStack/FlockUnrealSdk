# Changelog

All notable changes to this plugin will be documented in this file.

The format is based on [Keep a Changelog](http://keepachangelog.com/en/1.0.0/)
and this project adheres to [Semantic Versioning](http://semver.org/spec/v2.0.0.html).

## [0.14.0] - 2026-07-28

### Added
- **A C++ code generation target.** A new **Codegen Target** setting picks what a schema sync emits:
  `Blueprint` (the default, unchanged) or `C++`. The C++ target writes a generated module under
  `Source/FlockGenerated`, registers it in your `.uproject`, and emits a `USTRUCT` per template and
  config, a `UENUM` for shop items / currencies / achievements, and a typed call surface over them.
- **Typed C++ accessors.** `FFlockGenerated::GetPlayerLevel(...)`, `SavePlayerLevel(...)`,
  `GetGameplay(...)`, `Purchase(...)`, `UnlockAchievement(...)`, `AddFunds(...)`. Every function resolves
  the SDK from a world context object, so no call site holds a subsystem or a provider, and ids are baked
  so none is typed by hand. A call made before the SDK is initialized fails through its callback.
- **Generated C++ structs are `BlueprintType`**, so a graph in a C++-target project still sees the typed
  types and can drive them through *Flock Data To Struct* / *Flock Struct To Command Data*.

### Changed
- **Generated C++ members read as idiomatic C++.** A field declared `game_currencies` becomes
  `GameCurrencies`, and a name that is not a legal identifier at all (`200`, `class`) becomes one
  (`_200`, `Class`) instead of being skipped. The declared name is carried in a generated lookup table
  that the SDK consults at every nesting depth, so writes still go out under the names your template
  declares. Nothing is registered by hand — codegen emits the table and the module that installs it.
- **`Get <Template>` hands back the row id beside the struct rather than inside it**, matching the
  Blueprint target. A row id member would show up in a Break node as though it were template data, and the
  write path would need to exclude it by special case.
- **Switching target clears the other one's output** on the next sync. The generated C++ *module skeleton*
  is always kept — deleting it would leave a registered module with no sources, so a project that switched
  away would stop building. Removing Blueprint assets needs the editor's referencer check, so a headless
  sync reports what it could not remove instead of leaving duplicates unmentioned.
- **Clean Generated** now removes both targets' output, keeping the module skeleton and resetting its
  manifest header rather than deleting it.

- **The signed-in player no longer has to be passed.** `Purchase`, `Get Player Inventory`,
  `Get All Player Data`, `Get Player Ban`, and `Get Player Features` all resolved an empty player id to
  the signed-in player already; now C++ has overloads that omit the argument entirely, and the Blueprint
  `Player Id` pins are collapsed into each node's advanced section. Reading or acting for *another*
  player is still there, just no longer the thing you see first.
- **`GetMyData(Page, Limit, ...)`** joins the player provider. On `GetAllData` an empty player id means
  *every* player, so the signed-in case needed its own name rather than an overload that looks like the
  admin read.

### Notes
- A C++ sync **does not build or restart for you**. Unreal cannot adopt new reflection data into a running
  editor, so the restart is unavoidable however the build is started; the sync writes sources, registers
  the module, and says what to do next.
- A **Blueprint-only project is refused**, with a message naming the way out, rather than being silently
  converted into a C++ project.

## [0.13.0] - 2026-07-28

### Added
- **Typed Blueprint code generation.** **Tools > Flock > Sync Schemas** fetches this game version's player
  templates, game configs, and shops, and generates typed Blueprint assets from them — no C++, no
  toolchain, no compile, no editor restart. Everything lands in `Content/Flock/Generated` (configurable).
  There is also a `Flock.SyncSchemas` console command for headless or scripted runs.
- **Generated structs.** One Blueprint struct per player template and game config, with real typed pins —
  ints, floats, strings, bools, arrays, string-keyed maps, and nested objects as their own structs. Break
  one open in a graph instead of reading values by string path.
- **Generated enums.** `FlockShopItemId`, `FlockCurrencyId`, and `FlockAchievementId`, so you pick a shop
  item or an achievement from a dropdown instead of typing an id that is only checked at runtime.
- **A one-node read per game config and player template.** `Get Gameplay` fetches, converts, and hands
  back the typed struct with `Completed` / `Failed` exec pins and an `Error` — no id to pass, no separate
  fetch to wire. These are Blueprint macros, so they belong in event graphs; the library is parented to
  Actor, covering Actors, ActorComponents and the Level Blueprint.
- **A one-node write per player template.** `Get Currencies` also hands out the row id, and
  `Save Currencies` takes it back with the struct — so a read-modify-write is `Get`, the engine's
  *Set members in struct*, and `Save`, with no strings anywhere. The row id is an output rather than a
  hidden detail because it identifies this player's row and nothing else can supply it. Game configs get
  no `Save`. Note that `Save` sends the whole struct, so always `Get` first: a struct built from scratch
  writes `0` or `""` over every field you did not fill.
- **A one-node command per family.** `Purchase`, `Unlock Achievement`, and `Add Funds`, each taking the
  matching generated enum, so the lookup is no longer nested inside the SDK's own node. `Add Funds` bakes
  the `currency`-tagged template id and skips the runtime tag scan.
- **Tools > Flock > Clean Generated** (and `Flock.CleanGenerated`) removes every generated asset and the
  manifest. Blueprints still referencing a generated struct or enum are listed before anything is deleted.
- **A CI commandlet.** `UnrealEditor-Cmd.exe <project> "-run=FlockEditor.FlockCodegen" -mode=verify`
  answers `0` when the committed assets match the backend, `2` on drift, and `1` when it could not run —
  an unreachable backend or bad settings. Keeping those last two apart is the point: a network blip must
  not read as "your generated code is stale". `-mode=sync` regenerates instead.
- **Generated function library.** The pieces the macros are built from, useful on their own: each
  template's and config's id as a constant, `Read …` / `Make … Update` conversions between a fetched row
  and its typed struct, and lookups turning a picked enum member into the id the SDK sends. Grouped in the
  action menu under `Flock > Generated > Ids / Structs / Lookups`.
- **Content catalog asset.** A read-only asset listing every template, config, shop, item, currency, and
  achievement the backend declares — select it in the Content Browser to browse your content model
  without code or a dashboard login. It is regenerated on every sync and never referenced at runtime, so
  it is not included in packaged builds.
- **Drift detection.** Each sync records what it generated for. The editor warns on startup when your
  generated assets are for a different game version than the one baked into project settings.

- **Every network call is traced.** Requests log as `-> GET <url> [Blueprint 'bpTest']` and responses as
  `<- 200 GET <url> (34 ms, 1892 bytes) [Blueprint 'bpTest']`, from one place in the HTTP client, so no
  feature can forget to. Network lines name their caller the same way provider lines already did, and the
  origin is captured when the request goes out — a slow call stays attributed to the graph that made it.
  Bodies are deliberately not logged — the sign-in body carries a password and the rest carry a bearer
  token. A failure response is the exception: its first 512 characters are included, because that is the
  server's coded error and the single most useful line in the trace.

### Fixed
- **A generated struct with a nested object now writes correctly.** Unreal's JSON converter lower-cases
  the first letter of every name it emits, which reached the members of a *nested* struct and the keys of
  a map — but not the top level, where the SDK sets the keys itself. So a template field declared
  `Level: { Map, Stage }` went out as `{"Level":{"map":…,"stage":…}}` and the server rejected the write
  for a required property the graph had visibly set. Names are now emitted exactly as declared, at every
  depth.

- **Enable Debug Logs now actually shows anything.** Debug breadcrumbs are written at `Verbose`, but the
  `LogFlock` category is declared at `Log`, so Unreal built every line and then filtered it out — the
  setting appeared to do nothing. Initialization now raises the category when the setting is on. It only
  raises, so `Log LogFlock Verbose` typed at the console still works with the setting off.
- **A failed HTTP call is now visible without debug logs on.** Failures log at warning level with the
  server's reason, and an unreachable server is worded differently from a rejected request — they need
  different fixes.

### Changed
- **Generated struct members carry the field names your template declares.** This is what lets an update
  built from a generated struct be accepted by the server: a field declared `game_currencies` reads back
  as `GameCurrencies`, and writing that spelling is rejected. The generated conversions handle the
  difference so you never see it.
- A field name that cannot be a Blueprint member (a space, a dot, a leading digit) is **skipped with a
  warning** rather than renamed — a renamed field would silently produce updates the server rejects.
  Rename it on the dashboard to use it from Blueprint.
- A field whose shape Blueprint cannot express falls back to an opaque JSON handle, with a warning naming
  it, instead of being guessed at.
- **Game configs get a read but no update builder.** A config is game-wide and changed from the dashboard;
  offering a client-side write would have been offering a call the server always rejects.

## [0.12.0] - 2026-07-27

### Added
- **Game commands.** The server-validated way to change a player's data. `Update Player Data` writes a
  set of fields onto a data row, `Update Player Data Field` writes a single one, `Unlock Achievement`
  unlocks an achievement on the player's achievements row, and `Add Game Funds` credits their wallet.
  The achievement and wallet calls resolve the right row for you from the player template tagged
  "achievement" / "currency", so there is no id to look up first. Every command answers with the whole
  updated row, and that row is written straight back into the player cache — so a read right after a
  write sees the new values, not the old ones.
- **Offline queue with automatic replay.** A data update, field update, or achievement unlock made with
  no connectivity is saved to disk, applied optimistically to the cached row so your UI stays honest,
  and replayed in order when the connection returns. The queue belongs to the player who made the
  writes — signing in as someone else never replays them — and survives quitting the game. Replays
  happen automatically on sign-in, on returning to the foreground, and when connectivity comes back;
  `Flock Flush Pending Commands` triggers one by hand, and `Flock Get Pending Command Count` drives a
  "syncing…" indicator. A write the server permanently rejects is dropped (with the optimistic value
  rolled back) rather than blocking everything behind it; a temporary failure keeps everything queued.
- **Money is handled differently, on purpose.** `Add Game Funds` is never queued offline — it fails so
  you can tell the player — and is never re-sent after an ambiguous failure, because a request that
  timed out may already have credited the account. No offline grants, no double credits.
- **Blueprint.** Async nodes for the whole surface — Flock Update Player Data, Update Player Data
  Field, Unlock Achievement, Add Game Funds, Flush Pending Commands — plus a new
  `UFlockCommandDataLibrary` for building the values to write. Drag off a Data pin and chain
  `Set Command Int / Float / String / Bool / String Array` (and `Set Command Json` for a nested shape);
  values keep their real type on the wire instead of all becoming strings. Field names are sent exactly
  as you type them.



### Added
- **Player data & templates.** Read a player's saved data. Fetch a data row by id, a page of rows
  (optionally filtered to one player), or resolve the signed-in player's row for a template — by the
  template's id or by its tag (e.g. "currency", "achievement"). A player's rows are paginated and cached
  in memory on the first read, then served locally until you sign out or clear the cache. Player
  templates (the schema + default data for each kind of record) are fetched all-at-once or by
  id/name/tag; the all-templates read is cached in memory and backed by the offline snapshot. A
  row's/template's structured `data` comes back as a handle you read by dotted path (see below).
- **Player bans.** Fetch a player's ban record (empty player id = the signed-in player). Always fetched
  fresh (never cached), and an unbanned player is a normal success with an empty record; per-feature
  bans are keyed by feature name.
- **Blueprint.** Async nodes for the whole surface — Get Player Data By Id, Get All Player Data, Get My
  Data By Template / By Tag, Get Player Templates, Get Player Template By Id / By Name / By Tag, Get
  Template Player Data, and Get Player Ban.
- **Blueprint: `Flock Get Player Features`.** Per-player resolved feature flags were C++-only; they now
  have an async node like the rest of the config surface. Leave **Player Id** empty for the signed-in
  player.
- **Blueprint convenience nodes (no "Get Flock Subsystem" needed).** A new `UFlockLibrary` exposes the
  fire-and-forget analytics calls and the auth/session/lifecycle state as static nodes that resolve the
  SDK from the calling graph's world context — `Flock Log Event` / `Log Error` / `Log Exception`,
  `Record Screen View`, `Set/Has Analytics Consent`, `Get Analytics Session Id/Snapshot`, `Is
  Authenticated`, `Get Player Id`, `Logout`, `Is Initialized`, `Get Game Id`, `Get Events`, and more. So
  logging an event is one node, like the async provider nodes, instead of grabbing the subsystem and
  wiring it into a Target pin. The subsystem methods stay for C++ and existing graphs.

### Changed
- **The opaque structured-data handle is now `FFlockStructuredData`** (was `FFlockGameConfigData`), and
  its Blueprint reader is **`UFlockStructuredDataLibrary`** (was `UFlockGameConfigLibrary`; its nodes are
  now Get Data Int/Float/String/Bool/String Array, Has Data Field, Get Data Field Names, Data To Json
  String, Is Data Valid). One handle is shared by game config, patches, and player data/templates — the
  same dotted-path reads over any of them. Config reads are unchanged in behaviour; only the type and
  node names moved.

  **Migration (breaking).** C++: replace `FFlockGameConfigData` with `FFlockStructuredData` and
  `UFlockGameConfigLibrary::GetConfigInt(...)` with `UFlockStructuredDataLibrary::GetDataInt(...)` (same
  for Float/String/Bool/StringArray, `HasConfigField`→`HasDataField`, `GetConfigFieldNames`→
  `GetDataFieldNames`, `ConfigDataToJsonString`→`DataToJsonString`, `IsConfigDataValid`→`IsValidData`).
  Blueprint: graphs using the old *Get Config Int* / *Has Config Field* / … nodes will show them as
  missing after upgrading — replace each with its *Get Data \** equivalent and reconnect the pin. Cached
  offline snapshots are **not** affected (they are keyed by field name, not type name).
- Signing out now drops the signed-out player's cached data. Templates and their offline snapshot are
  kept — they are game-version-scoped, not player-specific.
- `GetPlayerFeatures` now treats an empty Player Id as "the signed-in player" (previously a Validation
  error), matching every other Player Id argument in the SDK. Signed out it is still a Validation
  failure, raised before any request.

## [0.10.1] - 2026-07-27

### Changed
- **Shop free-form data is now read with typed accessors instead of a raw JSON string.** A shop's
  `stats` and a shop item's `data` (open, game-authored objects) come back as an `FFlockJsonData`
  handle: read values by dotted path with `TryGetInt/Float/String/Bool/String Array`, `HasField`, and
  `GetFieldNames` in C++, or the new **UFlockJsonDataLibrary** nodes (Get Json Int/Float/String/Bool/…)
  in Blueprint — no hand-written JSON parsing. Replaces the previous `DataJson` / `StatsJson` strings on
  `FFlockShopItem` / `FFlockShopData`. The blob still round-trips the offline snapshot untouched.

## [0.10.0] - 2026-07-26

### Added
- **Shop.** Browse your game's shops and items, buy an item, and read a player's inventory. Fetch all
  shops (paged), a shop by id or by name, a single item, or the items in a shop (optionally as of a
  patch). Catalog reads are cached in memory and backed by the offline snapshot, so a second read is
  free and a fetch survives an outage. A shop's and an item's free-form `data` comes back as verbatim
  JSON you parse in-game.
- **Purchase.** Buy a shop item for the signed-in player (or a named one). A purchase is money-moving,
  so it is never retried on an ambiguous failure and never queued — the outcome surfaces so you can
  decide. Around a purchase the SDK records a Started / Purchased / Failed transaction for revenue
  metrics; that recording is best-effort and never blocks or fails the purchase.
- **Player inventory.** Fetch a page of a player's owned items. Inventory changes on every purchase,
  so it is always fetched fresh (never cached) — offline it fails rather than reporting stale ownership.
- **Recording transactions.** `RecordTransaction` sends a purchase to the analytics backend for
  revenue/LTV/ARPU metrics. Requires a signed-in player; sent immediately (not spooled).
- **Blueprint.** Async nodes for the whole surface — Get All Shops, Get Shop By Id / By Name, Get
  Shop Item, Get Shop Items, Purchase, and Get Player Inventory. An empty Player Id means "the
  signed-in player".

## [0.9.0] - 2026-07-26

### Added
- **Game config.** Fetch your game's config and patches — by id, by name, all patches, patches for a
  config, and configs filtered by tag or by version tag — plus per-player resolved features. The one
  you'll reach for is **Resolve Config Data**: it returns a config's effective values, applying the
  patch for this game version if one exists and otherwise falling back to the config's own base data
  (never empty defaults). Config values come back as an opaque handle you read with typed accessors.
- **Game & version info.** Fetch the game record, this build's game version, and a version looked up
  by name.
- **Reading config values in Blueprint.** A **Get Config Int / Float / String / Bool / String Array**
  library reads values off a resolved config by a dotted path (`stats.max_health`), each with a
  fallback so the common read is one node. Names resolve whether you type the dashboard's
  `snake_case` or the Pascal form. Plus **Has Field**, **Get Field Names**, **To Json String**, and
  **Is Valid**. Blueprint async nodes cover every fetch (Get Config By Name/Id, Get Configs By Tag,
  Resolve Config Data, Get Game, Get Game Version).
- **Offline snapshot cache.** Successful config and game reads are cached to disk and served when the
  network is unavailable, so a fetch survives an outage. Entries are scoped to the game version and
  pruned when it changes; per-player features are deliberately never cached. Toggle it (and point it
  at a custom directory) under **Project Settings → Flock SDK → Offline Cache**.

### Changed
- Concurrent requests for the same config are de-duplicated into a single backend call — several
  Blueprints asking on screen-open cost one round trip, not one each.

## [0.8.0] - 2026-07-22

### Added
- **Session ends survive everything.** Every close — manual, quit, timeout, logout, or a session
  replaced by a new one — is written to disk before it is sent. A failed send leaves the record
  queued and the next flush retries it, so quitting, signing out, or losing the network costs
  delivery time rather than the session. Ends drain ahead of log events: they are small, rare, and
  the record you most want to arrive.
- **Crashed sessions are recovered.** A run that dies with a session open is picked up on the next
  launch, closed at the last moment it was known to be alive, and delivered. Previously that session
  stayed open on the backend indefinitely. The live session is recorded on start, on registration,
  on each heartbeat, and when the app is backgrounded, so a crash costs at most one heartbeat of
  duration rather than the whole session.
- **Sessions that never registered register themselves.** A session opened while offline runs
  locally with no server id; the heartbeat now retries the registration, and if it never succeeds,
  the end registers the session (with its original start time) before closing it.
- **Queued ends wait for a signed-in player** instead of being attempted on every flush. Session
  routes need a bearer, so a drain while signed out could only ever fail; the queue is emptied at
  the next sign-in, which is the first moment it could have worked. Should a token expire mid-drain,
  the record is kept and the failure is reported as a debug line rather than an error, because
  nothing was lost.

### Changed
- **Starting a session while one is open replaces it** instead of returning the open session's id.
  The previous session is closed and reported with reason `Restarted`, which no longer leaves its
  metrics unrecorded.
- **Withdrawing consent discards the session** rather than ending it: nothing is queued, nothing is
  sent, and no `OnSessionEnded` is raised. Previously the session was reported to the backend after
  the player had opted out. Queued events and any pending session ends are dropped as before.

### Fixed
- **Signing out no longer loses the session's end.** Tokens were cleared before the analytics side
  was told, so the close went out unauthenticated, was rejected, and was never retried — every
  logout lost its session. The session is now closed before sign-out completes, and the queued
  record covers the token-expiry and revoke paths too.
- An explicit `EndSession()` left the crash marker in place, so a clean exit afterwards was reported
  as a crash on the next launch. Every end path now clears it.
- A registration answered with a success status but no session id sent the SDK into unbounded
  recursion. It is now treated as a failed registration and retried later.

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
- **Blueprint metadata nodes.** Dragging off an *Extra Data* pin now offers *Flock Metadata
  (Integer / Float / Boolean / String)* and *Make Flock Metadata* — chainable nodes that build the
  map without hunting for *Make Map* or converting values to strings first. They write values
  identically to the C++ builder.
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
