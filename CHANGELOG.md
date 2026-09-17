# Changelog

All notable changes to this plugin will be documented in this file.

The format is based on [Keep a Changelog](http://keepachangelog.com/en/1.0.0/)
and this project adheres to [Semantic Versioning](http://semver.org/spec/v2.0.0.html).

## [1.18.0] - 2026-09-16

### Added

- **A playtest feedback form**, built from the questions the playtest publishes: text, longer text, a 1-to-5 rating, a
  list to pick from, and a tickbox, each with its own help text and marked when an answer is needed. Editing the form
  on the dashboard changes what players see without a new build, and a kind of question this version does not know is
  still shown as a text box rather than left blank.
- Answers are checked before anything is sent, with every problem shown against the question it belongs to at once, so
  a player is not told about them one at a time. Nothing is said while someone is still typing.
- The form opens with a key (F9 by default, and settable to none), from Blueprint or C++ with **Open Feedback Form**, or
  from a game's own pause menu. **Can Open Feedback Form** is false when the playtest published none, so a game can
  leave its feedback entry out rather than offering something that does nothing.
- While the form is open the mouse is shown and typing goes to the form; closing or sending it puts both back exactly
  as they were. The game can be paused meanwhile with **Pause The Game While The Form Is Open**, off by default.

- **Answers are sent to the playtest they belong to**, and a form that cannot be sent right away is kept and sent by a
  later launch, so a player who has answered never loses the work to a dropped connection or to closing the game.
- Sending the form again in the same session **replaces** the earlier answers rather than adding a second report, so a
  player can change their mind. One sent with no session running is never re-sent, because there the server would
  record it twice.
- The player is not made to wait: the form closes as soon as the answers are ones the server will take.
- **An "Upload your recording" button on the form**, offered only when the recording has somewhere to go: one is
  running, it belongs to the playtest, and a playtest session has started for it. Asking stops recording for the rest
  of the session and sends what was recorded, and the offer is replaced by a word that the video is on its way -- shown
  only when sending really began. Opening the form does not stop recording by itself.

### Fixed

- A video recording no longer looks up the same engine setting by name on every frame, which made the engine warn
  about it after five hundred frames of recording.
- A recording that cannot be uploaded now says why in the log — no playtest session to send it to, no finished file, or
  the game closing — instead of stopping without a word.

## [1.17.0] - 2026-09-16

### Added

- **Playtest recordings are uploaded.** A finished recording is sent to the playtest session it belongs to: when a
  length or size limit ends it, when the game asks for it to be stopped and uploaded, and — for anything an earlier
  launch could not send — at the start of a later one. A recording is deleted once it has been uploaded, and kept
  otherwise, so nothing is lost by a failed upload, a crash or the game closing.
- **Stop the recording and upload it**, for a game that offers the player a way to hand in what they recorded. Opening
  a feedback form does not stop recording on its own.
- Recordings an earlier launch left are uploaded **even when playtesting is switched off** in the launch that finds
  them, and nothing else playtest-related happens in that launch. Otherwise turning playtesting off would strand every
  recording still waiting to be sent.
- A recording is sent straight from disk and is never held in memory, so uploading one costs the same whether it is a
  few megabytes or the size limit.

## [1.16.0] - 2026-09-15

### Added

- **A playtest recording that was not uploaded is kept for a later launch.** Each launch's recording now has a folder
  of its own under `Saved/FlockPlaytest/Recordings/Playtest/`, and the Protokite session it belongs to is saved beside
  it: the session id, the Protokite API URL and the Game Version ID the session started with, never the API key.
  Uploading comes in a later version.
- **A recording cut off when its game ended** (closed from the task manager, crashed, or the power went) is finished by
  the next launch with every whole frame it holds, so it plays. A recording whose file could not be written to the end,
  on a full disk for example, keeps the frames written before.
- **A playtest recording that no Protokite session started for is deleted by the next launch.** The player never
  signed in, or the game ended first, so it has nowhere to be uploaded. The log says how many were deleted.
- **Recordings Disk Budget** in *Project Settings > Plugins > Flock Playtest Settings* (4096 MB): how much the
  recordings may take together. Before a recording starts, recordings of games that are no longer running are deleted
  until it fits: test videos first, oldest first, and a playtest recording waiting to be uploaded only when that is not
  enough. A recording only makes room for what its length limit can record at its bitrate. It is cut shorter when the
  budget has less left than **Recording Size Limit**, and with less than 1 MB left none starts.
- Nothing touches a recording while the game that made it is still running, so two games started from one project, or
  several Play In Editor clients, never delete each other's recordings. A Game Version ID changing deletes nothing.

### Changed

- **Playtest recordings are saved as WebM files, which a browser plays with nothing installed** — 1.15.0 wrote IVF
  files, which needed a desktop player. The video inside is the same VP9, so nothing is re-encoded: a recording costs
  no more time and no more disk than before, and is ready to watch the moment it is saved. A recording whose game
  ended part-way through is still finished by the next launch, and plays up to the point where it stopped.
- Test videos, from `FlockPlaytest.RecordTestVideo` or **Record Video In Play In Editor**, are saved under
  `Saved/FlockPlaytest/Recordings/TestVideos/` and kept until Recordings Disk Budget needs their room.

### Fixed

- **A session that ends as the game closes is no longer abandoned part-way.** Both the analytics session end and the
  playtest session end were sent from shutdown and then torn down around, so the requests were dropped while still in
  flight: the engine reported unbinding them, nothing failed, nothing was logged, and the playtest dashboard showed
  those sessions as still in progress with no duration. Requests already on their way now get a bounded chance to
  finish while the SDK is still alive, so their outcome is known. The wait is limited by the engine and cannot hold up
  a game's exit, and anything that does not make it is cancelled exactly as it would have been a moment later. This
  matters most for the playtest session, which has no second chance -- an analytics session end is saved to disk and
  re-sent on a later launch, so it was only ever delayed.

- **Two games started from one project folder no longer lose each other's saved files** (two game clients on one
  machine, or Play In Editor beside a standalone game). When Flock started, it deleted every temporary file in its
  analytics queue and asset cache folders, including one another running game had just written and was about to move
  into place. That game's queued event or downloaded asset was lost, and its game thread waited five seconds while the
  engine retried the move and logged an error. A temporary file is now deleted only once it is a minute old, and each of
  these saves uses a temporary file of its own, so two games saving the same offline cache entry or downloading the same
  asset no longer share one.
- The offline cache now deletes the temporary files a crash left behind. It never did.
- A save that cannot be moved into place gives up at once instead of holding the game thread for five seconds.
- **A second game started from the same project folder no longer reports the first as crashed, ends its session, or sends
  its queued analytics a second time.** Each launch now keeps its crash marker, its live-session record and its event
  queues in a folder of its own under `Saved/Flock/analytics/launches/`, locked for as long as the game runs, and takes over
  only the folders of launches that have ended, however they ended: their queued entries join its own queues, and a crash
  or an unfinished session is reported once. The files an earlier build kept straight in `Saved/Flock/analytics/` are
  taken over the same way, once. Session numbers carry on across launches.
- **A game killed while it saves the consent choice or the sign-in no longer loses it.** Both are written beside the old
  file and moved over it, and a save cut off in between is read back from the file it was written to. Before, a kill could
  leave an empty consent file, which reads as no decision, so a project that does not require explicit consent collected
  again from a player who had opted out.

## [1.15.0] - 2026-09-15

### Added

- **Flock Playtest records the game's screen when the playtest turns video recording on.** Recording starts as soon as
  the playtest config is loaded, and saves VP9 video to `Saved/FlockPlaytest/Recordings/` as an IVF file, which VLC
  plays. It records what the player sees, the game's interface included. Uploading the file comes in a later version.
- **Video Recording settings** in *Project Settings > Plugins > Flock Playtest Settings*: **Video Width** and **Video
  Height** (1280 by 720; the video keeps the screen's shape and fits inside, and a smaller window is recorded at its own
  size), **Video Frames Per Second** (30), **Video Bitrate** (2000 kbps, about 0.9 GB an hour), **Recording Length Limit**
  (60 minutes) and **Recording Size Limit** (1536 MB).
- **One recording per launch.** It stops for good at either limit, when the game calls `StopVideoRecording` or the
  **Flock Stop Video Recording** node, when playtesting stops, or when the game instance shuts down, and the file is
  finished before shutting down returns. Time the game spends in the background is not recorded. `IsRecordingVideo()`
  and **Flock Is Recording Video** say whether it is running.
- **Recording can be tried without a playtest.** Tick **Record Video In Play In Editor** in *Project Settings > Plugins >
  Flock Playtest Local Settings* (saved for you only, never committed) and press Play, or type
  `FlockPlaytest.RecordTestVideo <seconds>` in the console of any build that is not Shipping;
  `FlockPlaytest.StopVideoRecording` stops it early. The log names the saved file. A test video is refused while the
  playtest records video itself.
- **Video is recorded on 64-bit Windows only.** Everywhere else, and in a run that draws nothing (a dedicated server, or
  `-nullrhi`), one warning says so and the rest of the playtest carries on.
- Each captured frame is read back from the GPU on the render thread, which adds about 7 ms of render-thread time to
  that frame at 1280 by 720 (measured at 30 captured frames a second). In a game capped at 60 frames a second the
  frames still met their 16.7 ms budget where it was measured; a game whose render thread is already near its budget
  can miss it on captured frames.

## [1.14.0] - 2026-09-15

### Added

- **Flock Playtest sends heavy analytics when the playtest turns it on.** While playtesting is ready and the Flock
  SDK's **Analytics Enabled** is on, every ten seconds of play become one `performance_window` event: the frame count,
  the median, 95th and 99th percentile frame times, the hitches (frames at or over the engine's
  `t.HitchFrameTimeThreshold`), and the memory in use and at its peak. Each map the game instance loads from then on
  becomes one `level_loaded` event, with the map before it and, when the load held the game up, how long it took. Both
  go through the Flock SDK's analytics under the category `playtest`, so they follow its consent setting and wait for a
  signed-in player the same way.
- **Time away is not play.** The timeline pauses while the game is in the background, leaves out the frame time that a
  level load or a return from the background stretched, and drops a window that playtesting stopping cut short. The
  session's length, pauses and frame rate stay the Flock SDK's, and are never sent twice.
- **`UFlockPlaytestSubsystem::RecordPlaytestEvent`** and the **Flock Record Playtest Event** Blueprint node record
  the game's own events for the playtest, under the same category. Outside a heavy analytics playtest they record
  nothing and return false, so the call is safe in every build. `IsMeasuringPerformance()` says whether the timeline is
  running.

### Changed

- **Track Event refuses a name over 200 characters or a category over 100.** The server cannot store either, and
  failed the whole request for it, so every event sent alongside was held back and sent again until its attempts ran
  out. An event like that already waiting from an earlier build is dropped with a warning instead of being sent.

## [1.13.0] - 2026-09-15

### Added

- **Flock Playtest starts one Protokite session per launch.** Once this build's playtest is loaded and the first
  Flock session of the launch has reached the server, the plugin starts a Protokite session that names that Flock
  session. It sends the player's Steam id when a Steam subsystem is already running, and otherwise a device id kept
  in `Saved/FlockPlaytest/device_id.txt`, which stays the same from one launch to the next. Steam is never required
  and never started.
- **The session lasts the launch.** A later Flock session, a sign-out or the Flock SDK shutting down neither ends nor
  restarts it. It ends when the game instance shuts down, or when `UFlockPlaytestSubsystem::EndPlaytestSession()` is
  called. `GetPlaytestSessionState()`, `GetPlaytestSessionId()` and `GetPlaytestIdentity()` report where it is.
- **A start is sent once.** Every start creates a session, so a start that failed, or whose answer never arrived, is
  not tried again that launch. With neither a Steam id nor a device id, nothing is sent and a warning says why.
- **A new status: the playtest has closed.** When Protokite refuses the session because the playtest takes no more
  sessions (HTTP 400), playtesting stays off until the game is launched again.
- **`UFlockEvents::OnSessionRegistered`**, raised once a session reaches the server, with its local id and the id the
  server gave it. `OnSessionStarted` carries only the local id.
- **Session Platform** (Project Settings > Plugins > Flock SDK Settings > Analytics) replaces the engine's platform
  name when a session starts, for example `steam` for a Steam build. A value that starts or ends with a space is not
  used, and a warning says so.

### Changed

- **Flock Playtest depends on the engine's OnlineSubsystem plugin**, which is on by default, to read the Steam id of a
  Steam subsystem that is already running. It does not depend on the Steam plugin.
- **The Flock panel's live view shows a session reaching the server.**

## [1.12.0] - 2026-09-14

### Added

- **Flock Playtest fetches this build's playtest from Protokite.** Once **Enable Playtesting** is on and the Flock
  SDK has initialized, the plugin asks Protokite for the playtest linked to this build's Game Version ID, sending
  the API key and Game Version ID the Flock SDK initialized with, once per Flock initialization.
  `UFlockPlaytestSubsystem::GetPlaytestConfig()` returns it: the playtest id, its feature switches, and its
  published feedback form when it has one.
- **`UFlockPlaytestSubsystem::IsPlaytestFeatureEnabled`**, with the feature names in `FlockPlaytestFeatures`. A
  feature the playtest does not mention is off, and every feature is off until the status is ready.
- **The status says why a build is not ready yet:** fetching the playtest, no playtest linked to this Game Version
  ID, Protokite refused the API key, the playtest could not be fetched, or Protokite answered for a different Game
  Version ID. Each change is logged once, and the ones that keep playtesting off are warnings that say what to
  check.
- **`UFlockSubsystem::GetRequestHeaders()`** (C++ only) returns the API key and Game Version ID headers the SDK
  initialized with, for code that calls another Qwacks service on the game's behalf. It is empty before
  initialization and after shutdown.

### Changed

- **Flock Playtest is ready only once the playtest has been fetched**, not as soon as its settings are complete
  and the Flock SDK is initialized.

### Fixed

- **Packaging a Development build of a project with the Flock SDK failed to compile** (since 1.7.0): a test read
  Blueprint pin metadata, which a packaged build does not carry.
- **Building a Shipping game with the Flock SDK failed to compile** (since 1.0.0): the SDK raised its log
  category's verbosity for **Enable Debug Logs**, and a Shipping build compiles logging out.

### Notes

- **When Protokite cannot be reached, the game carries on with playtesting off, and the playtest is fetched again
  when the next Flock session starts.** A refusal (no linked playtest, a refused key) is not asked again. The
  retries within one attempt use the Flock SDK's own HTTP settings.

## [1.11.0] - 2026-09-14

### Added

- **Flock Playtest, an optional plugin for Protokite playtests (beta).** It ships as its own download,
  `FlockPlaytest-<version>.zip`, and installs next to the Flock SDK as `Plugins/FlockPlaytest/` — never inside
  `Plugins/FlockUnrealSdk/`, where Unreal does not look for it. It stays disabled until you enable it for your
  project, and even then does nothing until **Enable Playtesting** is turned on.
- **Flock Playtest Settings** (*Project Settings > Plugins*): **Enable Playtesting**, off by default, and
  **Protokite API URL**. A URL that does not start with `http://` or `https://`, names no host, or contains a
  space or line break is refused with a warning that quotes it, rather than quietly trimmed.
- **`UFlockPlaytestSubsystem::GetStatus()`** says whether playtest work may run and, if not, why: turned off,
  Protokite API URL missing or unusable, waiting for the Flock SDK to initialize, ready, or stopped. This release
  collects no playtest data yet; later releases build on this status.

### Changed

- **Each release publishes two zips from the same tag, at the same version**: the Flock SDK and the optional
  playtest plugin. The release notes say where each one goes.

### Notes

- **Cloning this repository into `Plugins/FlockUnrealSdk/` does not install the playtest plugin.** Its source sits
  at `OptionalPlugins/FlockPlaytest/`, where Unreal does not load it; copy or link that folder to
  `Plugins/FlockPlaytest/`. An `AdditionalPluginDirectories` entry pointing inside `Plugins/` does not work: the
  editor ignores it.

## [1.10.0] - 2026-09-14

### Added

- **Gameplay events for the Game Metrics dashboards: `Flock Track Event` (`TrackAnalyticsEvent`).** Records
  what a player did — a name, an optional category, and properties whose keys and value types reach the
  dashboard unchanged. Events are written to disk and delivered in batches while a player is signed in; an
  event recorded before anyone signs in is credited to whoever signs in next. The name `session_started`
  (the server records it itself) and empty names are refused on the spot. Each player's events travel in a
  batch of their own, so a player the server does not know never takes anyone else's events down with it.
- **Blueprint script exceptions are captured automatically** — Accessed None, a missing property, a runaway
  loop — carrying the Blueprint call stack that locates the node, in every build configuration. Breakpoints
  and tracepoints are ignored.
- **Repeats of the same exception are counted, not sent again.** The first occurrence is reported at once;
  repeats inside **Analytics Exception Repeat Window** (60 seconds by default) are counted and reported as one
  more entry carrying `repeat_count`. Numbers and addresses in a message do not make a new fault.
- **Exception capture has its own settings** under *Analytics | Exceptions*: **Analytics Capture
  Exceptions**, **Analytics Exception Excluded Categories** (starting with the automation framework's, which
  logs failing tests as errors) and **Analytics Exception Repeat Window**.
- **`Flock Get Exception Capture Coverage`** says what the running build can see. A Shipping or Test build,
  which compiles error log lines out, also sends one `exception_capture_limited` entry to Diagnostics → Events
  — once per build, not on every launch.
- Captured faults carry `exception_source` (`log`, `blueprint` or `crash`), and a Blueprint exception also
  carries `blueprint_exception_type`.
- A [Diagnostics](Documentation/diagnostics.md) guide for log entries and exception capture. The
  [Analytics](Documentation/analytics.md) guide now covers only what players did, and both open with the same
  table so the two are not confused.

### Fixed

- **A log entry the server refuses no longer holds up every entry behind it forever.** It is dropped and the
  flush reports the refusal. When the server refuses a whole batch because of what is in it, the entries are
  sent one at a time, so only the entry it refused is lost.
- **With caching switched off, log entries are sent rather than silently lost.**
- **A server outage no longer empties the queue.** Only sends the server answered count against an entry, an
  entry is dropped after 50 of them, and after each one the interval flush waits twice as long, up to 15
  minutes.
- **Crash reports carry their category and source**, like every other captured fault.

### Changed

- `Flush` reports a failure when an entry was dropped because the server refused it, even though the rest of
  the queue was delivered: success means nothing was lost.

## [1.9.0] - 2026-09-07

### Fixed

- **Shipping a build with a new Game Version no longer discards the player's queued offline writes.** A
  write made with no reachable server is kept on disk and replayed when one comes back — but the queue
  was stored under the game version, and the plugin deletes every *other* version's stored data at
  startup. So a player who saved offline and then took an app update lost those writes: no error, no
  log, nothing to look at. The queue now lives outside the version-scoped tree, where that cleanup
  cannot reach it.

  **A queue written by an older build is moved for you, once, on the next start** — nothing is lost by
  upgrading.

  A write queued under an older version still routes correctly and still addresses the right row. If
  that row's template changed in the meantime the server refuses the replay, and the queue reports that
  and drops it — a far better outcome than deleting the write unasked.

### Added

- **`FFlockSnapshotStore::StateScope`**, the reserved place for things that are not re-fetchable. The
  offline cache is scoped by game version and pruned when that version changes, which is right for a
  cached response and wrong for anything the server has never seen. Anything written under this scope
  survives a version change.

### Notes

- **Cached answers behave exactly as before**: a new game version still drops the previous one's
  snapshots, so a running build picks up dashboard changes rather than serving stale content.

## [1.8.0] - 2026-08-31

A failure now tells you what broke and what to do about it. Printing an error names the call that
failed, the server's own reason, the coded error, and the next step to take.

**Engine support re-verified for this release.** `Tooling/Build-AllEngines.ps1` cleaned, built and ran
the full automation suite against **UE 5.5, 5.6, 5.7 and 5.8** — **415/415 editor and 133/133 `-game` on
each** — and reported *"The declared claim (UE 5.5 to UE 5.8) is verified."*

### Added

- **`FFlockError::Hint`** — the SDK's next step for this failure, filled from a table keyed on
  `EFlockErrorCode` and stamped when the error is built, so any coded failure carries its remedy
  whichever layer produced it.
- **`FFlockError::Operation`** — a short label of the call that failed (`Device login`,
  `Purchase shop item`), taken from the context each provider call site already declares.
- **`FFlockError::ToDisplayText()`** — one line naming both the problem and the fix:
  `Device login failed: Invalid credentials [player.invalid_login_credentials, HTTP 401]` followed by
  `Fix: This device is not registered yet. Call Flock Register With Device once...`. Each segment is
  dropped when its source is empty, so a client-side failure still composes to exactly its message.
- **`FFlockErrorHints`** — the `EFlockErrorCode` to next-step table, public so a game can reuse the
  wording in its own UI. `ForAuth(Code, Method)` exists because one code is genuinely ambiguous:
  `player.invalid_login_credentials` means "register this device first" for a device login and "wrong
  password" for email, and the HTTP layer cannot know which.
- **Eight `EFlockErrorCode` members** that were missing, all on surface this SDK calls:
  `NotificationTemplateNotFound`, `PlayerInventoryAlreadyUsed`, `PlayerInventoryInventoryEntryNotFound`,
  `ShopMalformedReward`, `ShopPackGrantsNothing`, `ShopRewardCurrencyNotHeld`, `GameCommandRateLimited`
  and `AnalyticsInvalidCurrencyId`. Each previously arrived as `Unknown` — including both `Consume`
  failures and all three reward failures, which had shipped uncovered alongside 1.7.0's own shop work.
- **[Errors guide](Documentation/errors.md)** — what an `FFlockError` carries, branching on codes rather
  than text, and what is worth retrying yourself.

### Changed

- **`To String (Flock Error)` now composes the developer-facing line** rather than returning the log
  text. The raw response body is no longer part of it — bucketing an error tracker by unbounded payload
  is what that exclusion was always about. Read `FFlockError::Body`, or `ToString()`, which is unchanged
  and still appends it.
- `FFlockError::Message` is unchanged and stays terse. The composed text is additive.

### Fixed

- **A request-validation failure now names the offending field.** `detail` arrives in two shapes: the
  game routes' coded `{code,message}` object, and the request-validation layer's array of `{loc,msg}`,
  which carries no code at all. Only the object was read, so that entire class of 422 surfaced as a bare
  "Validation failed" with no reason — the server's explanation sat unread in the body. Arrays now render
  as `body.player_data: Input should be a valid dictionary`, three fields spelled out and the rest
  collapsed into a `(+n more)` tail. A plain-string `detail`, which is what an authentication failure
  returns, is carried through the same way.

## [1.7.0] - 2026-08-30

Shop items can grant rewards server-side and the SDK now surfaces them, and the player's scheduled
notifications can be read back from the server instead of only from what this install wrote down.

**Engine support re-verified for this release.** `Tooling/Build-AllEngines.ps1` cleaned, built and ran
the full automation suite against **UE 5.5, 5.6, 5.7 and 5.8** — **409/409 editor and 132/132 `-game` on
each** — and reported *"The declared claim (UE 5.5 to UE 5.8) is verified."*

### Added

- **Shop item rewards.** `FFlockShopItemReward` (`Type`, `Code`, `Amount`) with `FFlockShopItem::Type`
  and `::Rewards`, so a shop tile can show "500 gold" before anyone buys. A plain item carries an empty
  list, never a null to check.
- **`FFlockShopProvider::Consume(InventoryId, OnComplete)`** — consumes an owned inventory entry and
  reports the updated row, what it granted, and the wallet afterwards. It is money-moving, so it takes
  the same non-idempotent path as a purchase: an ambiguous failure surfaces rather than being re-sent,
  and only a failure that proves the request was never processed is retried. Never queued offline.
- **`FFlockConsumeResult`** and **`FFlockPurchaseResult`**, both parsed from the route's real root-shaped
  response.
- **`FFlockPlayerInventory::Rewards`** — the reward snapshot taken when a row was bought. Empty on the
  plain inventory listing, which does not carry it.
- **`Flock Consume Inventory Item`** Blueprint node, and **`UFlockShopLibrary`**: `Is Currency Reward`,
  `Flock Currency Reward Type`, `Has Inventory Row`, `Has Wallet`, `Has Wallet (Consume)`.
- **`FFlockNotificationProvider::GetScheduled(Status, Page, Limit, OnComplete)`** — the player's
  schedules as the *server* knows them, so a listing survives a reinstall and sees reminders made on
  another device. Filtered by status, never cached, with a `FFlockScheduledNotificationPage` result.
- **`Flock Get Scheduled Notifications`** Blueprint node, plus `Flock Schedule Status Pending` /
  `Delivered` / `Canceled` for its filter pin.
- **`Flock Cancel All Scheduled Notifications`** and **`Flock Get Pending Schedules`** Blueprint nodes.
  Both surfaces were C++-only, so a "clear my reminders" button could not be built in a graph — and the
  natural substitute (a ForEach over Cancel Scheduled Notification) fires every cancel in one frame,
  where they race over the stored pending list and resurrect entries each other removed. The node walks
  them sequentially.
- `FFlockPurchaseResult::HasInventoryRow()` / `::HasWallet()` and `FFlockConsumeResult::HasWallet()`, so
  the "this may legitimately be empty" check has a name at the call site instead of a bare `Id.IsEmpty()`.
- `FlockShopItemRewardTypes` and `FlockScheduledNotificationStatuses` — the wire spellings as constants.
  Both are **string constants rather than enums, deliberately**: the server owns each set and can extend
  it, and an enum would answer a value added later by failing to parse or by silently defaulting.

### Upgrading

- A shop snapshot written by 1.6.1 has no `Type` or `Rewards` on its items, so the first **offline** launch
  after upgrading can show a catalog with empty reward lists. It self-heals on the next successful fetch;
  the snapshot envelope version is deliberately unchanged rather than invalidating every cached shop.

### Fixed

- **A purchase or consume now updates the cached player row with the wallet it returns.** The
  per-player data cache previously kept pre-purchase balances, so the next `GetMyDataByTemplate` served
  the old number while the money had already moved on the server — and a read-modify-write wrote that
  stale row straight back, silently undoing the purchase. Found live: the self-test's own commands sweep
  was resetting a balance its purchase had just changed. `FFlockShopProvider::SetPlayerProvider` wires
  the same write-through seam the commands surface already used. An absent wallet (the purchase moved no
  currency) leaves the cache untouched rather than blanking it.

### Changed

- **BREAKING — `FFlockShopProvider::Purchase` now completes with `TFlockResult<FFlockPurchaseResult>`
  instead of `TFlockResult<FFlockPlayerInventory>`.** The inventory row moved to `.Inventory`, joined by
  `.Granted` and `.Wallet`. The SDK had been deserializing `POST shop/transaction` onto the inventory
  model, but the route answers a purchase result with the row *nested* — against a real backend that
  produced a populated-looking object whose fields were all empty. Fixing it is a compile error at every
  call site, which is the point: silent nulls are worse.
  - **`.Inventory` can legitimately be empty.** An item that hands its contents over outright creates
    nothing to own and reports the grant instead. Use `HasInventoryRow()` (C++) or *Has Inventory Row*
    (Blueprint) rather than assuming a row.
  - **`.PurchaseId` is the transaction id, not the row id** — the row id is `.Inventory.Id`. Old code
    reading `Result.Value.Id` will not compile, but `.PurchaseId` sits first in the struct and looks like
    the obvious replacement; it is not. This is the one part of the migration the compiler cannot catch.
  - **Blueprint graphs need reconnecting too.** The `Flock Purchase` success pin changed type, so wires
    downstream of it are orphaned. Break the result for `Inventory`, `Granted` and `Wallet`.
  - The `Flock Purchase` Blueprint node's output pin changes type to match, and a generated `Purchase`
    macro's output pin is renamed `Entry` -> `Purchase Result`. Regenerate after upgrading.
- **`CancelAllScheduled` now cancels what the server lists**, falling back to this install's local list
  only when that read fails. It previously walked local bookkeeping alone, so a reinstall or a second
  device left the player's own reminders firing with no way to cancel them. The two lists are never
  merged: an id the server does not report is one it no longer considers pending.
- `GetPendingSchedules()` is unchanged and still synchronous. Its role changed — it is now the offline
  fallback rather than the primary way to list schedules.
- `Flock.SelfTest`'s shop leg reports the purchase id, item type, each granted reward and the wallet.
  Its previous narration — that the response carries an inventory row and no reward detail — was a
  contract fact that has stopped being true. That narration moved into `FlockSelfTestNarration.h` so it
  can be covered by tests: the three states it distinguishes (no inventory row, nothing granted, no
  wallet) are exactly the ones a live run cannot reach while reward granting fails server-side, so
  without them the first sight of this output would be the first time the backend fix worked.

## [1.6.1] - 2026-08-22

Three defects found by a whole-codebase review, each shipping with the regression test it was missing,
plus two more found reviewing those fixes.

**Engine support re-verified for this release.** `Tooling/Build-AllEngines.ps1` cleaned, built and ran the
full automation suite against **UE 5.5, 5.6, 5.7 and 5.8** — **385/385 editor and 108/108 `-game` on each**
— and reported *"The declared claim (UE 5.5 to UE 5.8) is verified."*

### Fixed

- **The leaderboard read surface could not succeed against any backend.** `GetStandings`, `GetMyRank` and
  `GetAroundMe` resolved the board name to an id and then requested `leaderboard/{id}`, `/{id}/me` and
  `/{id}/around-me` — paths that do not exist under `/v1`. The id-addressed leaderboard routes in the API
  are the unversioned dashboard ones, which take OAuth2 and a different game header and are not the SDK's
  to call. All three now request `leaderboard/by-name/{name}/standings|me|around-me`, which take exactly
  the query parameters the SDK already sent and return the same enveloped shapes, so nothing else changed.
  Leaderboards were non-functional beyond `GetByName` in 1.2.0 through 1.6.0.
- A read now costs **one request instead of two** — the name goes on the wire, so there is no id to
  resolve first. The board-config memo still serves `GetByName` and `ResolveId`.
- A board name your game does not have still fails as a **Validation** error naming the board, rather than
  as a bare 404 or an empty page. The server is what reports it now, so the 404 is translated; the status
  is kept on the error, so a board that really was deleted still propagates instead of serving a stale
  cached page.
- **One unparseable response could wedge the offline write queue forever.** A captive-portal login page
  answering a write with an HTML `200` produces a serialization failure carrying status 200, which is
  outside the 4xx range — so the classifier read it as transient, the replay stopped at that entry, and
  every write behind it was blocked for the life of the install. The queue persists across relaunch, so
  short of a game calling `ClearPendingWrites()` — which discards the writes rather than delivering them —
  the block was permanent.
- The rule is now **who said no, not what number came back**: a queued write is discarded only when the
  backend authoritatively rejected it. An unparseable response stays queued (the server may never have
  seen the write — discarding it would lose a change the player made), and a **bounded 50 replay attempts**
  backstops it, so no failure the SDK cannot classify can hold the queue indefinitely. The attempt count
  is persisted with the entry, so the bound survives the relaunch the queue survives.
- A **403 now only ends a write when it carries a coded body from the backend.** A bare 403 is a proxy,
  WAF or corporate gateway that never consulted the server, and the write is kept. 401 still keeps the
  write and clears on the next sign-in, as before.
- **Only a completed exchange counts against the 50-attempt budget.** A failure that never reached the
  server — Connection, Timeout, Cancelled — does not spend one. Without this gate the backstop defeated
  its own purpose: a flush fires on the rising edge of reachability and the offline latch self-expires
  every 30 seconds, so a game left open with no network manufactured an attempt every half-minute and
  would have discarded the player's change after about half an hour offline.
- **A replay no longer continues under a different player's bearer.** Nothing on the sign-out/sign-in path
  reloads the command queue, so a player switch during an in-flight replay left the queue's head matching
  while the session had changed underneath it — the next write would post with the new player's token, and
  the server rejecting it would drop the previous player's write for good. The flush now compares the
  queue's player against the live one and abandons rather than continuing.
- **The notification state migration no longer deletes the previous copy on an unverified write.**
  `FFlockSnapshotStore::Write` returns void and has three exits that only log, so a full disk or a locked
  file meant the new copy silently never landed while the old one was removed — losing exactly the pending
  reminders the change exists to protect. The new location is read back first, and the old copy is kept for
  a later retry if it did not.
- **`Notification ClearCache()` warns instead of silently doing nothing when no player is signed in.** The
  Logout-ordering hazard was inverted rather than removed: it used to be "clear before the tokens go or the
  watermark is lost", and is now "clear before the tokens go or nothing is cleared at all". A reordered
  logout would have left the departing player's inbox on disk with every test still passing.
- **An empty snapshot scope is treated as "no cache" rather than as a location** (`FFlockProviderBase`).
  `SanitizeScope` culls empty segments and falls back to `"_"`, so a player-scoped scope built with no
  player would have resolved to a shared directory outside any game version — and outside the reach of the
  delete meant to clean it up.
- **`Notification ClearCache()` destroyed every other player's records on a shared device.** It deleted the
  whole notification snapshot category — which held the inbox cache, the seen-watermark and the
  pending-schedule list for *every* account on the device — and then restored the current player's two
  state records. Another player's pending reminders were gone: a scheduled notification still fires
  server-side and its id is the only handle on it, so those reminders became uncancellable. Called with
  nobody signed in, an empty player id resolved to the bare category and it cleared everyone, restoring
  nothing.
- Inbox caches now live under a **per-player scope** (`<version>/notification/<player id>`), so clearing
  one player's cache is a directory delete that cannot reach another's. The seen-watermark and the
  pending-schedule list moved to their **own category**, so they survive a clear by construction rather
  than by being read out and written back — which also removes the hazard that reordering `Logout()` would
  silently break them. Signed out, `ClearCache()` now does nothing.
- State written by 1.3.0–1.6.0 is **migrated on first read** and the old copy removed, so upgrading does
  not strand a pending reminder.

### Added

- `Flock.Http.Endpoints.LeaderboardPathsMatchV1` — a literal lock on the four `/v1` leaderboard paths,
  written against the API rather than against the implementation. The previous lock was literal, green,
  and wrong: it locked the three id paths the provider was building, which is a test that can only ever
  agree with the code it was derived from.
- `FFlockSnapshotStore::DeleteKey` — removes one entry rather than a whole scope.
- `FFlockCommandProvider::MaxReplayAttempts` and `FFlockPendingCommand::Attempts`.
- Ten tests, all in both the editor and client contexts: leaderboard by-name routing and the unknown-name
  translation; the write queue's unparseable-response, attempt-cap, attempt-persistence, coded-403,
  connection-failures-do-not-spend-attempts and player-switch-mid-flight behaviours; and notification
  `ClearCache` sparing other players, doing nothing signed out, and migrating state written by an earlier
  version.
- The fake transport used by the test suite now matches routes **case-sensitively**. `FString::Contains`
  defaults to ignoring case, so a board legitimately named `Medals` made `leaderboard/by-name/Medals`
  match a `/me` route — a fixture answering the wrong request with nothing looking wrong.

## [1.6.0] - 2026-08-17

**Engine support re-verified for this release.** `Tooling/Build-AllEngines.ps1` cleaned, built and ran
the full automation suite against **UE 5.5, 5.6, 5.7 and 5.8** — **375/375 editor and 98/98 `-game` on
each** — and reported *"The declared claim (UE 5.5 to UE 5.8) is verified."*

### Added

- **Notification events on the events hub.** `OnUnreadCountChanged` (the player's unread count, as the
  server last reported it) and `OnNotificationReceived` (one raise per notification the SDK has not
  surfaced before). Both are `BlueprintAssignable`, so a graph binds them off `Get Events` with no new
  nodes.
- `OnUnreadCountChanged` fires only when the server actually reports a count — an unread-count or
  summary fetch, or a mark-all-read, which reports zero because that call has exactly one possible
  outcome. It never fires from a background poll, because the SDK does not run one.
- **`Documentation/architecture.md`** — how the SDK is layered and why, the module split, the folder map,
  the rules that hold across every feature, and a table separating Flock's design decisions from Unreal's
  mechanics. Linked from the README's guide index.
- **`GetPendingSchedules()`** — the schedules this install created that have not reached their delivery
  time yet. Synchronous; it never touches the network.
- **`CancelAllScheduled()`** — cancels everything still tracked and reports how many the server actually
  cancelled.

### Fixed

- **Release notes no longer duplicate.** `generate_release_notes` does not set a body — when a release
  already exists for the tag it is updated, and the generated notes end up combined with the body already
  there, so each re-run stacked another "What's Changed". v1.1.0 and v1.4.0 both shipped that way. The
  workflow now fetches the notes explicitly and passes the whole body, so publishing the same tag twice
  overwrites instead of accumulating.

### Changed

- **Releases state the supported engine range.** Every release body now carries a "Supported Unreal
  Engine versions" section — the floor, the ceiling, and what each boundary actually does (below the
  floor refuses to compile; above the ceiling compiles with a notice). The range is published by the
  engine-claim checks *after* they validate it against the manifest, README and compat header, so the
  notes cannot state a range those checks disagreed with and there is no second parser to drift.
- **`Flock.SelfTest` covers the pending-schedule list and the notification events.** Scheduling now
  narrates the tracked entry, cancelling shows it untracked, and a second reminder is scheduled purely to
  exercise `CancelAllScheduled` — cancelled immediately, so nothing live is left behind. The two events
  are reported at the end of the sweep.
- **`Flock.SelfTest` funds the wallet before it buys.** The demo player had no balance in the item's
  currency, so every run stopped at `shop.insufficient_funds` and the purchase *success* path had never
  run against a real backend. The sweep now grants exactly the item's price first, which is net-neutral,
  and then narrates what the purchase response does and does not contain.
- **`Flock.SelfTest` narrates the purchase's analytics transaction.** The shop provider fires those
  fire-and-forget, so a failure only ever reached the log; the sweep now sends one with a completion and
  reports the outcome.
- `Flock.SelfTest` now sweeps assets: the index listing, one asset resolved **by name**, and that
  asset's bytes downloaded. It runs signed out, alongside the config and shop-catalog sweeps, because
  neither asset route declares `security`. Point `DemoAssetName` at an asset on your backend; an empty
  name skips the by-name and download steps and leaves the index listing running.
- `Documentation/assets.md` gains two worked recipes: showing a downloaded texture in a Widget
  Blueprint, and building a loading screen around the preload nodes.

### Notes

- **"Received" means first seen by a read, not the moment the server created the row.** There is no
  realtime channel and the SDK never polls, so a persisted per-player watermark is what separates a
  notification the game was already told about from a genuinely new one. It rides the inbox and summary
  reads and adds no traffic of its own.
- **The first fetch for a player seeds silently.** Handing a game an existing inbox as a burst of
  arrival events on launch is worse than not reporting its history at all.
- **The watermark survives `ClearCache()`** — it is state, not cache. Dropping it on sign-out would
  make that player's next session either re-announce their whole inbox or silently swallow everything
  older than the sign-out. Its key carries the player id, so a shared device cannot apply one account's
  cutoff to another's mail.
- Notifications are raised **oldest first**, which means walking the page backwards: the route answers
  newest-first, and handing a game its mail in reverse order is the bug that ordering avoids. A row
  whose `created_at` cannot be parsed is skipped rather than announced — with no comparable timestamp
  it would raise on every fetch, and a duplicate is worse than a miss.
- **The pending-schedule list exists because `/v1` has no route to read a schedule back.** The id a
  schedule call returns is the only handle on a pending reminder, so the SDK persists what it scheduled.
  That bounds what it can know: it sees only what *this install* created, and it infers delivery from the
  clock. Entries whose time has passed are dropped as the list is read.
- **It survives `ClearCache()` for the same reason the watermark does** — it is state, not cache, and the
  server cannot tell us again. Its key carries the player id, so a second player on a shared device
  neither sees nor can cancel the first player's reminders.
- **`CancelAllScheduled` drops entries the server permanently rejects** (delivered, already cancelled,
  unknown) rather than failing the whole batch — they are not pending either way. A *transient* failure
  stops the run instead, leaving the rest tracked so a later call can retry them.
- An **unparseable `deliver_at` is kept**, the opposite of the watermark's handling of a bad timestamp:
  there the risk is announcing a notification twice forever, here it is stranding a reminder nothing can
  cancel.
- **Verified live 2026-08-17.** `Flock.SelfTest` runs all eleven notification calls green against a real
  backend — templates, by-name, schedule and cancel included — and both new state files persist under
  `Saved/Flock/snapshots/{version}/notification/`: the watermark seeded silently on the first inbox read,
  and the pending list recorded the schedule then cleared it on the cancel.
- Live sweep output for the new surfaces: pending 1 after scheduling → 0 after cancel → 1 after a second
  schedule → `cancel all scheduled -> ok (1 cancelled server-side, 0 still tracked)`. `OnUnreadCountChanged`
  fired **three** times — the unread-count fetch, the summary fetch and the mark-all-read, which is exactly
  the set of calls where the server reports a count. `OnNotificationReceived` reported 0, the correct
  steady state once a player's watermark is seeded.
- **Note on repeat runs:** the funded purchase means each self-test run leaves one more inventory row on
  the demo player. There is no client route to consume or refund one, so that grows over time — the cost
  of proving the purchase success path at all.
- **The backend echoes `deliver_at` with a microsecond fraction and no timezone suffix**
  (`2026-08-17T07:52:03.879000`), not the `...Z` form. It parses correctly as UTC — a fraction longer
  than three digits is rounded to milliseconds and a bare terminator is accepted — so elapsed entries do
  drop in production. A fixture now pins that exact shape, because one built only from the `Z` form would
  pass while the pending list grew forever.
- **The index is listed before the by-name lookup on purpose.** There is no by-name route on the
  backend — the SDK filters the index — so printing what the game version holds turns a name miss into
  a visible spelling difference rather than a bare failure.
- **The download step is the one a metadata read cannot stand in for.** An asset record can be
  perfectly correct while its bytes are unreachable: the presigned URL carries whatever host the
  backend was configured to sign for, and one that only resolves inside the API's own network fails at
  the client and nowhere else. The sweep compares the received byte count against the record's reported
  size and narrates a mismatch.

## [1.4.0] - 2026-08-15

**Account linking** — attach more than one credential to the same player, so a guest who started on a
device can add an email or a social login and keep their progress. Lives on the auth provider beside
login and register, because these are the same `/v1/player/*` credential routes with the same bearer
gate; it is not part of the player-data surface.

Read the list with `GetLinkedAccounts`, attach with `LinkEmail`, `LinkDevice`, `LinkGoogle`,
`LinkApple`, `LinkSteam`, `LinkFacebook` or `LinkDiscord`, and detach with `Unlink`. Every one of them
answers with the player's **full updated credential list**, so a link or unlink doubles as a refresh —
there is no separate re-read to remember. Each listed account carries a typed `Provider Type` you can
hand straight back to `Unlink`. The same nine calls are Blueprint nodes under **Flock|Auth**.

Two new events, `OnAccountLinked` and `OnAccountUnlinked`, both carrying the provider.

Four new error codes surface the cases worth handling in-game: `PlayerAccountAlreadyLinked` (the
credential belongs to another player — there is no account-merge flow, so this reaches your code),
`PlayerAccountNotLinked`, `PlayerCannotUnlinkLastCredential` (the server refuses to leave a player with
no way back in), and `PlayerInvalidLinkRequest`.

`ResetPassword`'s gate widened: it used to require signing in *with* email, and now also accepts an
email credential linked during this session. Never narrower than before. That knowledge is
session-scoped and deliberately not persisted — after a restored session the SDK does not know what is
linked until the game reads the list.

Credential state is never cached and never queued offline: a re-sent link comes back as
`account_already_linked`, so these writes are posted non-idempotent.

## [1.3.0] - 2026-08-13

Notifications, in three parts: the in-app **inbox**, server-side **scheduled reminders**, and **push
device-token registration**. The inbox works on its own — it needs no push setup at all — and push works
without the inbox. Adopt whichever half you need.

**Flock does not fetch your push token.** Your game gets one from its push plugin (Firebase Cloud
Messaging, OneSignal) and hands the string to `RegisterDeviceToken`; Flock registers it against the
signed-in player so the backend can deliver. This is where every comparable backend SDK draws the line,
and on Android it is the only line available — Unreal ships no implementation behind its own Android
remote-notification hook, so there is nothing for the engine to hand over.

### Added

- `FFlockNotificationProvider`, reachable from `UFlockSubsystem::GetNotificationProvider()`.
- **Inbox**: `GetNotifications` (paged, with an unread-only filter), `GetUnreadCount`, `GetSummary`,
  `MarkRead` and `MarkAllRead`. Reads are backed by the offline snapshot cache, so an inbox screen shows
  the last-known messages when the network is down rather than an error.
- **Scheduling**: `ScheduleByTemplateName` and `CancelScheduled`, plus `GetTemplates` and
  `GetTemplateByName` for the template catalog. Reminders are delivered server-side, so they fire whether
  or not the game is running.
- Scheduling is addressed by template **name** — what you see on the dashboard — with the id resolved
  internally and memoized. `ScheduleByTemplateId` is there for a caller that already holds an id.
- **Push device tokens**: `RegisterDeviceToken` (taking the platform from the running build) and
  `UnregisterDeviceToken`, plus an explicit-platform overload for a token that did not come from this
  build. `GetCurrentDevicePlatform` reports whether push is available at all.
- Blueprint nodes: `Flock Get Notifications`, `Flock Get Unread Notification Count`,
  `Flock Get Notification Summary`, `Flock Mark Notification Read`, `Flock Mark All Notifications Read`,
  `Flock Get Notification Templates`, `Flock Get Notification Template By Name`,
  `Flock Schedule Notification`, `Flock Cancel Scheduled Notification`, `Flock Register Device Token`,
  `Flock Register Device Token For Platform` and `Flock Unregister Device Token`.
- `UFlockNotificationLibrary`: `Is Read`, `Is Pending`, `Is Delivered`, `Is Canceled`,
  `Flock Get Current Device Platform`, `Device Platform To String` and `Notification Channel To String`,
  all pure nodes.
- `Flock.SelfTest` now covers notifications end to end: template catalog, schedule and cancel, inbox
  reads, mark read and mark all read, and device-token register and unregister.

### Notes

- **Every notification call except the template catalog requires a signed-in player** and fails with an
  auth error without one — an inbox and a device token both belong to a player. The template catalog is
  game-scoped, so it works signed out and survives a logout.
- **Read-receipts are never queued offline.** `MarkRead` and `MarkAllRead` fail when the server is
  unreachable rather than replaying later, because a receipt delivered an hour late marks messages the
  player never saw.
- **Scheduling is not retried after an ambiguous failure**, so a network blip cannot leave a player with
  two of the same reminder. Cancelling and registering a device token *are* retried — both are idempotent.
- **Registering a token on an unsupported platform fails with a validation error rather than guessing
  one.** The backend accepts `android`, `ios` and `web`; a token filed under the wrong platform is
  accepted and then never delivers. Branch on `Flock Get Current Device Platform` to hide an "enable
  notifications" toggle on desktop instead of letting the call fail.

### Testing push notifications

Push cannot be exercised in the editor, in Play-In-Editor, or in a Windows build — Unreal's desktop
implementation is a stub that reports "not registered" and does nothing. To test delivery you need:

1. A **device build** for Android or iOS.
2. A way to mint the token, which differs by platform:
   - **iOS needs nothing extra.** Unreal surfaces the APNs token itself and the backend speaks APNs
     directly — bind `FCoreDelegates::ApplicationRegisteredForRemoteNotificationsDelegate`, hex-encode the
     bytes, and pass the string to `Flock Register Device Token`. No Firebase, no purchase.
   - **Android currently needs a Firebase-based plugin.** Bind its token callback and pass the string in.
     It must be a real FCM registration token, not a vendor subscriber ID.

   **Built-in acquisition is planned for both** — a one-call iOS wrapper and a first-party Android UPL
   shipping the modern `firebase-messaging` binding — so push will not depend on a paid marketplace
   plugin. `RegisterDeviceToken` itself does not change when they land.
3. **Provider credentials on the dashboard**, under Settings > Push Notifications: the Firebase
   service-account JSON for Android and web, or an APNs `.p8` key with its Key ID, Team ID and Bundle ID
   for iOS. Without these the token registers successfully and nothing is ever delivered.
4. On Android, `google-services.json` where your push plugin expects it (Firebase Features:
   `<YourProject>/Services/`). Leave the Google Cloud Messaging Sender ID field in Project Settings
   blank — it gates Unreal's own `GoogleCloudMessaging` plugin, which targets APIs Google decommissioned
   in 2019, and blanking it avoids two registration paths competing for the same engine delegates.

Step-by-step: **Documentation/push-setup-android.md**.

`Flock.SelfTest` covers the registration call itself on any platform by registering a synthetic token
under `web`, then unregistering it. That proves the request, response and parse — everything the SDK
owns — but not that a banner appears on a handset, which only a device build can show.

## [1.2.0] - 2026-08-11

Leaderboards. Read-only by design: a board projects over a player-data field, so a score is submitted by
writing that field through the existing game-command calls. If you are looking for a `SubmitScore`, it is
`UpdatePlayerDataField`.

### Added

- `FFlockLeaderboardProvider`, reachable from `UFlockSubsystem::GetLeaderboardProvider()`. Boards are
  addressed by **name** throughout — the id is resolved internally and never appears as an argument on a
  read. `GetByName`, `GetStandings`, `GetMyRank`, `GetAroundMe`, plus `ResolveId` for logging and deep
  links, and `ClearCache`.
- Name-to-board resolution is memoized for the session, so reading a board repeatedly does not re-resolve
  its name on every call.
- Standings, board configuration and player placements are backed by the offline snapshot cache: a
  leaderboard screen shows the last-known board when the network is down rather than an error. An
  authoritative 4xx still surfaces.
- Blueprint nodes `Flock Get Leaderboard`, `Flock Get Leaderboard Standings`, `Flock Get My Rank` and
  `Flock Get Standings Around Me`.
- `UFlockLeaderboardLibrary`: `Make Current Window`, `Make Season Window`, `Make Period Window`,
  `Is Current Window`, `Format Score` and `Is Higher Better`, all pure nodes.
- `FFlockLeaderboardWindow` selects which window to read — the board's live window by default, a finished
  season, or a raw period key such as `2026-W31`.
- `Flock.SelfTest` now sweeps leaderboards: board lookup, id resolution, standings, the player's own rank,
  the entries around them, and the unknown-board guard.
- `EFlockErrorCode::LeaderboardNotFound`, so a game can branch on "no such board" without matching on
  the message or the status code. It is the only coded error the leaderboard read routes return.

### Notes

- `FFlockPlayerRank` carries a `Ranked` flag rather than a sentinel rank or score. A player with no entry
  yet is a valid result, and rank 0, score 0 and negative scores are all legitimate values — so there is
  no number that could stand in for "no entry". Check `Ranked` before showing `Rank` or `Score`.
- Duration boards measure in seconds; `Format Score` renders them as a clock, and the enumerator is named
  `DurationSeconds` because the wire value does not state the unit.
- Reading a board's configuration and its standings is open to signed-out players. The player's own rank
  and the entries around them require a signed-in player and fail immediately when there is none.
- A board name this game does not have fails rather than returning empty standings.

## [1.1.0] - 2026-08-08

**Engine support is now a verified range: Unreal Engine 5.5 to 5.8.** Previously the SDK claimed 5.5 and
said nothing about anything newer. That turned out to hide two facts in opposite directions — 5.6 and 5.7
already worked, and 5.8 did not — because a floor cannot express either one.

### Added

- Support for Unreal Engine 5.6, 5.7 and 5.8. Every version in the range is compiled *and* run against
  the full automation suite before release.
- A warning, not an error, on engines newer than the verified ceiling. A new engine usually breaks
  nothing, so the SDK no longer blocks a project on the day one ships; the warning names the tool that
  re-verifies the range.
- `FFlockJsonUtils::GetFieldNames`, the one place the SDK enumerates JSON object keys.
- `Flock.Http.Json.KeySemantics`, pinning that key lookup ignores case and enumeration returns author
  spelling verbatim — the two properties the dotted-path getters and the commands write path depend on.
- Test coverage outside the editor. 42 tests covering the disk-touching paths (token store, snapshot
  store, asset cache, command queue) and the wire layer now also run with the editor not hosting them,
  and the release sweep runs both contexts on every engine. Previously the entire suite was editor-only,
  which cannot see a defect that depends on the editor being absent — the same shape of blind spot that
  hid the `EngineVersion` loading bug.
- The README now states which platforms are verified. Windows is what has been run; other targets are
  expected to work but unverified, and nothing restricts them.

### Changed

- `Flock.uplugin` no longer declares `EngineVersion`. That field version-gates plugin *loading*, not
  just compilation: with it set, the editor refuses to load the plugin on any engine but the one named,
  so the SDK would compile on the whole supported range and load on one version of it.

### Fixed

- Compilation on Unreal Engine 5.8, which changed `FJsonObject`'s key storage. The SDK now reaches for
  `HasField` / `TryGetField` instead of indexing the underlying container, which is both the supported
  API and stable across every engine in the range. No behaviour changed: key comparison is
  case-insensitive on all of them.
- `Tooling/Build-AllEngines.ps1` could not see engines installed outside `C:\Program Files\Epic Games`,
  so a machine with engines on another drive got a passing report covering one engine. It now sweeps
  every fixed drive, accepts `-SearchRoots`, and prints what it discovered.
- `Tooling/Build-AllEngines.ps1` built every engine into one shared `Intermediate`, and stale generated
  headers from one engine were compiled against another's — producing failures attributed to the SDK that
  were entirely artefacts of the sweep. It now cleans between engines.
- `Tooling/Build-AllEngines.ps1` failed immediately when run with `-File`, because its `-Project` default
  could not resolve under `[CmdletBinding()]`.

## [1.0.0] - 2026-07-30

**First stable release.** The versions below it are the development history and were never published;
1.0.0 is the first release on GitHub. Nothing is removed or renamed relative to 0.16.0 — the version
number is a statement that the feature set is complete and the API is one we intend to keep.

### Added
- **A Flock panel.** A dockable editor tab (`Window > Tools > Flock`, or `Tools > Flock > Flock Panel`)
  listing everything that is wrong with your setup, each with the button that fixes it. Credentials are
  editable inline and save to `DefaultGame.ini`, so a new project can be configured without leaving it.
- **The same status on the settings page.** Project Settings > Flock SDK now carries a banner above its
  properties reporting the same findings. Wherever you look, you get the same answer — the panel and the
  settings page cannot tell you different things about the same project.
- **The panel opens itself only when there is something to do.** It appears when setup is actually broken,
  or the first time you open a project with the plugin in it, and stays quiet otherwise. It is never
  modal, never blocks the editor, and a healthy project on a fresh clone sees nothing at all. Any notice
  can be muted per-developer, which silences the interruption without hiding the finding.
- **Test Connection tells you which credential is wrong.** It checks the API URL, the key, and the game
  version against the backend and reports the specific failure — unreachable host, rejected key, or no
  version by that name — instead of one undifferentiated "request failed". It also reads back the game
  your API key belongs to and warns when your Game Name disagrees with it.
- **The panel shows live SDK state during Play In Editor.** It switches to initialization state, the
  signed-in player, the analytics session, consent, the offline command queue, and connectivity, plus a
  running list of SDK events as they happen. Previously the editor went dark the moment you hit Play.
- `Is Likely Offline` on the subsystem, for an offline indicator in your own UI. Reports the HTTP layer's
  offline latch: only a request that never reached the server sets it, any completed exchange clears it.
- **The plugin's own icon** on the Flock tab and menu entry, with meaningful icons on the menu actions.

### Changed
- **The Play-In-Editor warning now lists everything blocking the session**, not just the first problem it
  found, and carries an "Open Flock" link straight to the panel rather than naming a menu path to hunt for.
- **Packaging is blocked by any setup error**, not only an unresolved Game Version.
- Per-developer editor state (last-seen SDK version, muted notices) lives in
  `EditorPerProjectUserSettings` rather than the committed `DefaultGame.ini`, so one developer upgrading
  or muting a notice no longer changes what the rest of the team sees.
- The edit-time version resolve now carries its typed error rather than flattening it to a sentence,
  which is what lets the connection test name a specific credential.

### Fixed
- **A build could package successfully with credentials that no longer worked.** The packaging guard only
  ever checked whether the Game Version ID was present. Because that ID is baked once by a successful
  resolve, clearing or rotating the API key afterwards left a project that packaged clean and then failed
  to initialize at runtime.

### Engine support
- The supported engine version is now stated in one place and checked. `Flock.uplugin`, the README, and a
  new compile-time floor must agree, and a CI job fails the build when they drift apart. Compiling
  against an older engine now fails with one line naming the problem instead of a wall of unrelated errors.
- `Tooling/Build-AllEngines.ps1` builds against every installed engine at or above the floor and reports
  what it could **not** cover, failing when the declared floor was never verified. Support is still
  **Unreal Engine 5.5** — unchanged, because 5.5 is what has actually been compiled.

## [0.16.0] - 2026-07-29

### Added
- **Assets.** Fetch the asset list for your game version, look one up by name or id, and download it —
  as a texture, as text, as raw bytes, or as a file on disk. Blueprint gets one node per flavour, each
  taking a single pin that accepts either a name or an id, with a progress pin alongside the usual
  success and failure pins. C++ gets the same surface on `GetAssetProvider()`.
- **Downloads stream to disk instead of being held in memory.** A large asset costs the disk write and a
  socket buffer rather than its full size in RAM, so a few hundred megabytes of downloadable content no
  longer has to fit in the heap twice on its way to the cache.
- **Downloaded bytes are cached on disk, keyed by asset and version.** A second read of the same version
  never touches the network; re-uploading an asset supersedes the old copy only once the new one has
  landed. The cache has a size budget with least-recently-used eviction, and a download larger than the
  whole budget is fetched but deliberately left uncached rather than evicting everything else.
- **An expired download link recovers by itself.** Download URLs are signed and time-limited, so one held
  in the offline cache can go stale. A refused download now refreshes that single record and retries once
  with the new link, instead of failing and making you refetch by hand.
- `Flock Preload Assets` / `Flock Preload All Assets` warm the cache for a loading screen, reporting
  progress and how many landed. Individual failures don't fail the batch.
- `Flock Is Asset Cached`, `Flock Get Uncached Assets`, `Flock Get Cached Asset Path`,
  `Flock Get Asset Cache Directory` and `Flock Clear Asset Cache` answer without a network call.
- **Asset Max Concurrent Downloads** setting (default 4) caps how many transfers run at once; the rest
  queue. The existing Asset Cache settings are now live — they had nothing reading them before this
  release.

### Notes
- There is no sound-wave download flavour. The engine has no way to turn compressed audio (mp3, ogg)
  into a playable sound at runtime, and supporting only uncompressed WAV would fail silently on exactly
  the files a content server usually holds. Use `Flock Download Asset File` and hand the path to whatever
  imports audio in your project.

## [0.15.0] - 2026-07-29

### Added
- **The offline cache now skips calls it knows cannot succeed.** When a request fails because the server
  could not be reached at all, cached reads are served directly for a short window rather than each one
  waiting out a request that has no chance of completing. Previously every offline read paid a full
  timeout before falling back to its snapshot; the fallback was correct, just slow. `Add Game Funds` gets
  the same benefit — its refusal to queue a grant while unreachable is now immediate.
- Any answer from the server ends that window at once, **including an error**: a 500 or a 401 still took a
  round trip, so the network is up whatever the server thought of the request. The window also lapses on
  its own, so the next read always re-tests the network for real.

### Fixed
- **A crash while writing a spooled analytics entry could leave a truncated file behind.** Entries are now
  written to a temp file and moved into place, and any temp left over from an interrupted write is swept
  at startup — matching how the snapshot cache has always written.
- **A spooled analytics entry that became permanently unreadable held its slot forever**, counting against
  the cache limit for the life of the install and being skipped on every flush. Such an entry is now
  dropped after two consecutive failed reads. Two rather than one so a file that is briefly locked — by a
  virus scanner or a backup agent — costs a retry instead of a good event.

### Changed
- A slow server is no longer treated as an offline one. Only a request that never reached the server marks
  the connection as down; a timeout, a validation failure, or any other response does not.

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
