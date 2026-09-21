# Playtesting with Protokite

Flock Playtest is an optional plugin that turns a Development build of your game into a playtest build for
Protokite. While a playtest runs, each launch of the game becomes one session on the playtest's
**Sessions** page, and the playtest decides what that session collects:

| Playtest feature | What the game sends | Where it shows |
|---|---|---|
| Always | One session per launch, named after the player's Steam id or this install's device id | Protokite **Sessions** |
| `video_recording` | A recording of the game's screen, uploaded when it ends | The session's recording player |
| `heavy_analytics` | A performance window every ten seconds of play, each level load, and your own playtest events | Flock **Game Metrics**, under the `playtest` category |
| `exception_capturing` | Nothing extra: the Flock SDK already reports exceptions, and the playtest only asks for them | Flock **Diagnostics → Errors** |
| A published feedback form | The player's answers | The session card, and the playtest's **Feedback form** page |

Everything is switched on the playtest's page in Protokite, so changing what a playtest collects needs no new build.

## Before you start

- **The Flock SDK is set up and working** in the project: API URL, API Key, Game Name and Game Version, with the
  Flock panel showing nothing to fix. The playtest runs through the Flock SDK and sends its API key.
- **A playtest exists in Protokite.** Creating one makes a Flock game version for it, named `pt-` followed by the
  test's id.
- **Playtest builds are Development builds.** A Shipping build compiles logging out, so the Flock SDK cannot report
  error lines or failed ensures from it (see [Shipping builds](#shipping-builds)).
- **Video is recorded on 64-bit Windows only.** Other platforms run every other part of the playtest and log once that
  they record no video.

## 1. Install the plugin

Releases carry `FlockPlaytest-<version>.zip` beside the Flock SDK's zip, from the same tag and at the same version.

1. Extract it into your project's `Plugins/` folder, so `Plugins/FlockPlaytest/` sits beside
   `Plugins/FlockUnrealSdk/`.
2. Enable **Flock Playtest** under **Edit → Plugins**, and restart the editor so it builds.

If you cloned the SDK repository, copy or link `Plugins/FlockUnrealSdk/OptionalPlugins/FlockPlaytest/` to
`Plugins/FlockPlaytest/`: Unreal does not load a plugin from where that folder sits. Leave the plugin out of projects
that are not running playtests.

## 2. Point the build at the playtest

Protokite finds the playtest from the Flock game version the build sends.

1. Open the test in Protokite. Its **SDK** block shows the playtest's Flock version **ID**.
2. In *Project Settings > Plugins > Flock SDK Settings*, set **Game Version** to that version's **name**: `pt-`
   followed by the test's id (for example `pt-01JABCDEFGHJKMNPQRSTVWXYZ0`). The Flock SDK resolves the name to the ID
   Protokite showed. Do not paste the ID into the settings file instead: the Flock SDK resolves Game Version again
   whenever Game Version, API URL or API Key changes, and replaces an ID written by hand.
3. In *Project Settings > Plugins > Flock Playtest Settings*, set **Protokite API URL** and turn on
   **Enable Playtesting**. The URL must start with `http://` or `https://` and contain no spaces; one that does not is
   refused rather than tidied up.

Pressing **Play** now says in the Play message log whatever in these settings would stop the playtest or change it,
each with a link to the page that fixes it: an empty or unusable Protokite API URL, the Flock SDK's analytics turned off,
a Game Version that is not a playtest's, and the settings a playtest session waits on. Nothing is said while
**Enable Playtesting** is off.

## 3. Sign a player in

**A playtest never signs a player in.** The playtest session starts from the Flock session, and a Flock session starts
once your game signs a player in, with **Analytics Enabled** and **Analytics Auto Start Session** on (or a Start
Session call) and consent granted when **Analytics Require Explicit Consent** is on. A device sign-in is enough and
needs no account.

A playtest build with no sign-in screen of its own can sign in from the console in Development builds:
`Flock.LoginWithDevice`.

## 4. What the player is asked

**A playtest build asks its player what it may collect, and collects nothing until they answer.** The question is drawn
over the game as soon as this build's playtest is loaded, and offers four answers:

| Answer | What the playtest then collects |
|---|---|
| Record my screen and collect play data | Everything the playtest turns on |
| Record my screen only | The recording; no performance windows, level loads, playtest events or exceptions |
| Collect play data only | Those; no recording |
| Collect nothing | Nothing at all: no recording, nothing sent, and no session for the launch -- exactly what this build does with **Enable Playtesting** off |

**It is the playtest's own question and its words say so.** The Flock SDK's **Analytics Require Explicit Consent** is a
different question, asked by your game in your game's words about your game's analytics; neither answer moves the other.
The playtest's answer is kept in `Saved/FlockPlaytest/playtest_consent.json` and used by every later launch of that
build. Nothing an earlier launch left waiting is sent while the question is still on screen, and nothing is sent at all
while the answer is *collect nothing* -- a player who changes their mind has it sent in the same launch. The feedback form is not covered by it either way: it is sent only when a player fills it in and presses Send.

What a playtest asks for and what the player allowed are both named in the log when playtesting becomes ready, and
`playtest_consent` and `playtest_consent_asked` ride along with every session start, so a session with no recording
reads as a player who asked for none rather than a build that went wrong.

Your game can take this over: **Flock Get Playtest Consent** and **Flock Get Players Consent Answer** read it,
**Flock Set Playtest Consent** records an answer from your own screens, and **Flock Ask For Playtest Consent** puts the
question again -- what a "change what this playtest collects" entry in your menu calls. Setting it to **Not Answered**
forgets the answer, so the question is asked again.

While the question is up the player's input goes to it and the game keeps running, so a game that asks during play
pauses first if being unable to act would cost the player something. The plugin never pauses for it: your game knows
when its player can be interrupted, and pausing does nothing in a multiplayer match.

Turn **Ask The Player For Playtest Consent** off in *Flock Playtest Settings* only where your players have been asked
another way, for an internal test on your own machines, or for an automated run with nobody there to answer. The build
then collects what the playtest turns on, and each session says the player was never asked. **An answer a player has
already given is still honoured** in such a build: the setting decides whether the question is put, not whether an
answer counts.

## 5. Play

The log, under `LogFlockPlaytest`, says what the playtest is doing:

1. *Playtesting is set up and waiting for the Flock SDK to initialize*, then *fetching this build's playtest from
   Protokite*.
2. *Playtesting is ready*, naming the playtest and the features it turns on.
3. *Protokite session … started for this launch*, once a player is signed in and the Flock session has reached the
   server.

The session appears on the playtest's **Sessions** page. It ends when the game shuts down, or earlier with
**Flock End Playtest Session**. A later Flock session, a sign-out or the Flock SDK initializing again neither ends nor
restarts it: a launch is one session.

When something stops the playtest, the log says why and names the setting, once, as a warning:

| Status | Meaning | Fix |
|---|---|---|
| Protokite API URL missing | **Enable Playtesting** is on and the URL is empty | Set **Protokite API URL** |
| Protokite API URL unusable | No `http://` or `https://`, no host, or a space or line break in it | Correct the URL; it is never trimmed for you |
| Playtest not linked | Protokite has no playtest for this build's Game Version ID | Point **Game Version** at the playtest's `pt-` version |
| Protokite refused API key | Protokite turned down the Flock API key | Check **API Key** in the Flock SDK's settings |
| Playtest config unavailable | Protokite or the network kept failing | Nothing: it is fetched again when the next Flock session starts |
| Playtest no longer collecting | The playtest has closed | Reopen it in Protokite, or point Game Version at a running one |
| Waiting for player consent | The playtest is loaded and the player has not said what it may collect | Nothing: it is asked over the game. A console can answer with `FlockPlaytest.AnswerConsent` |
| Player refused playtest | The player asked for nothing to be collected | Nothing: it is their answer. **Flock Ask For Playtest Consent** puts the question again |

In Blueprint, **Flock Get Playtest Status** answers the same question, and **Flock Describe Playtest Status** turns it
into the sentence the log uses.

## Video recording

When the playtest turns `video_recording` on, the game's screen is recorded from the moment the playtest loads, one
recording per launch, to a WebM file a browser plays. Background time is not recorded. The **Video Recording**
settings in *Flock Playtest Settings* set the size, frame rate and bitrate (1280×720, 30 frames a second and
2000 kbps by default), and two limits, whichever comes first: **Recording Length Limit** (60 minutes) and
**Recording Size Limit** (1536 MB).

A recording is uploaded to its session:

- when a length or size limit ends it;
- when the game asks, with **Flock Stop And Upload Playtest Recording** (C++: `StopVideoRecordingAndUploadIt`) or the
  feedback form's **Upload your recording** button;
- when the game stops it for good, with **Flock Stop Video Recording**;
- and at the start of a later launch, for anything an earlier launch could not send: a failed upload, a quit, a crash.

Quitting does not upload: a whole recording cannot be sent inside a shutdown, so it is kept and the next launch sends
it. A recording is deleted once uploaded. Recordings wait under `Saved/FlockPlaytest/Recordings/`, inside
**Recordings Disk Budget** (4096 MB), and when room is short the oldest test videos go first and recordings waiting to be
uploaded go last. A recording no playtest session started for can never be uploaded, so the next launch deletes it.

Offer a player a way to send their recording only while **Flock Can Send Playtest Recording** is true. It is false
when no session has started for the recording, and pressing a button then would stop the recording and send nothing.

To tell the player how it went, bind the playtest subsystem's **On Recording Upload Finished** event. It is raised once
when the launch's recording finishes: uploaded, or not uploaded with the reason, including when the upload could not
begin at all. It is not raised while the game is closing.

## Heavy analytics

When the playtest turns `heavy_analytics` on, the plugin sends through the Flock SDK's analytics, under the `playtest`
category:

- `performance_window` for every ten seconds of play: median, 95th and 99th percentile frame time, hitches, and memory
  used and at its peak;
- `level_loaded` for every level this game instance loads, with the level before it and, for a blocking load, how long
  it took;
- your own events, with **Flock Record Playtest Event**. Build their properties with the Set Command nodes.

Nothing is sent while the Flock SDK's **Analytics Enabled** is off; the log says so once.

## Exceptions

Exceptions stay the Flock SDK's: it captures error lines, failed ensures, Blueprint script errors such as Accessed
None, and crashes, with its own settings under **Analytics | Exceptions**, and reports them on **Diagnostics → Errors**
(see [Diagnostics](diagnostics.md)). A playtest's `exception_capturing` switch never turns that on or off. When a
playtest asks for exceptions and **Analytics Capture Exceptions** is off, the log warns once as the session starts.

To check exceptions reach the dashboard, type `Flock.RaiseTestException error` or
`Flock.RaiseTestException blueprint 100` in the console of a Development build. A hundred of the same fault arrive as
one report, then one repeat report counting the other ninety-nine once the repeat window closes.

## The feedback form

When the playtest publishes a feedback form, players open it with **Feedback Form Key** (F9 by default; set it to none
to turn the key off), or your game opens it with **Flock Open Feedback Form**, from a pause menu for example. Check
**Flock Can Open Feedback Form** first: it is false when the playtest published no form. While the form is open the mouse
shows and typing goes to it; closing or sending it puts both back as they were. **Pause The Game While The Form Is
Open** is off by default, because pausing does nothing in a multiplayer game and loses what a single player was about to
describe.

Every question comes from the playtest, so editing the form in Protokite changes what players see with no new build.
Answers are checked before sending, with every problem shown against its question. A form that cannot be sent is kept
and sent by a later launch, and sending again in the same session replaces the earlier answers.

**A form of your own.** **Flock Get Feedback Form** hands your UI the questions: each one's id, kind, label, help text,
options and whether it needs an answer. Record answers with **Set Feedback Text Answer**, **Set Feedback Rating Answer**
(1 to 5), **Set Feedback Checkbox Answer** and **Set Feedback Chosen Option**, check them with **Find Feedback Form
Problems**, and send them with **Flock Send Feedback Form Answers**. Compare a question's kind with the **Flock Feedback
Question Kind** nodes rather than typing it, and treat a kind you do not know as text, which is how the server reads
it. Two rules that are easy to miss:

- **Set every checkbox**, ticked or not. The server counts an unticked box as an answer, and a needed checkbox that
  was never set is missing.
- **A chosen option must match letter for letter.** The server compares options exactly, so `crash` is not `Crash`.

## Blueprint nodes

All under *Flock | Playtest*. Each finds the playtest from the calling graph and is safe in every build: with no
playtest running it answers false, empty or Turned Off and changes nothing.

| Node | Answers or does |
|---|---|
| Flock Get Playtest Status, Flock Is Playtest Ready, Flock Describe Playtest Status | Whether playtest work may run, and why not |
| Flock Is Playtest Feature Enabled, with Flock Playtest Feature Video Recording / Exception Capturing / Heavy Analytics | Whether the playtest turns a feature on |
| Flock Get Playtest Consent, Flock Get Players Consent Answer, Flock Describe Playtest Consent | What the player let the playtest collect |
| Flock Set Playtest Consent, Flock Ask For Playtest Consent, Flock Is Consent Question Open | Answering it from your own screens, and asking it again |
| Flock Get Playtest Session Id, Flock End Playtest Session | The launch's Protokite session |
| Flock Record Playtest Event | Records one of your own playtest events |
| Flock Is Recording Video, Flock Stop Video Recording | The launch's recording |
| Flock Can Send Playtest Recording, Flock Stop And Upload Playtest Recording | Sending the recording now |
| Flock Can Open Feedback Form, Flock Open / Close Feedback Form, Flock Is Feedback Form Open | The built-in form |
| Flock Get Feedback Form, Set Feedback … Answer, Find Feedback Form Problems, Flock Send Feedback Form Answers | A form of your own |

## Trying it in the editor

- **Before any playtest exists**, tick **Record Video In Play In Editor** in *Project Settings > Plugins > Flock
  Playtest Local Settings* and press Play, or type `FlockPlaytest.RecordTestVideo 30` in a Development build's console.
  Test videos are saved under `Saved/FlockPlaytest/Recordings/TestVideos/` and never uploaded.
- **Against a real playtest**, press Play with the settings above and sign in with `Flock.LoginWithDevice`. Then:

| Console command (Development builds) | Does |
|---|---|
| `FlockPlaytest.AnswerConsent <answer>` | Answers what the playtest may collect: `video_and_play_data`, `video_only`, `play_data_only`, `nothing`, or `not_answered` to be asked again |
| `FlockPlaytest.OpenFeedbackForm [seconds]` | Opens the form, after a wait if given |
| `FlockPlaytest.SendTestFeedback [seconds]` | Fills in the published form and sends it, to check answers reach Protokite |
| `FlockPlaytest.StopVideoRecordingAndUploadIt [seconds]` | Stops the recording and uploads it |
| `FlockPlaytest.StopVideoRecording` | Stops the recording; like any finished one, it is uploaded when it has a session |
| `Flock.RaiseTestException [error\|blueprint] [times]` | Raises a fault the Flock SDK reports |
| `FlockPlaytest.SelfTest [closed playtest's Game Version ID]` | Checks the whole playtest against Protokite; see below |

The waits exist because every `-ExecCmds` command runs on the first frame, before anything has been recorded or a
session has started.

## Checking a playtest build

`FlockPlaytest.SelfTest` checks everything a playtest build does against your real Protokite in one go, and logs a
line per step and a count at the end: `Playtest self-test finished: 16 passed, 0 failed, 1 skipped.` Development builds
only.

Each check sits beside a request Protokite must refuse: a wrong API key, a missing one, a version no playtest is linked
to, a session start that names no player, a form missing a needed answer or choosing an option that is not on the list,
and a form, upload link and end for a session that does not exist. A check that only ever succeeds cannot tell a working
build from a broken one. A refusal passes only with its own HTTP status and, for a form, the question it names.

It signs nobody in, so sign in first. From a script:

```
UnrealEditor-Cmd.exe MyGame.uproject -game -windowed -ExecCmds="Flock.LoginWithDevice, FlockPlaytest.SelfTest, Flock.QuitAfterSeconds 90" -log
```

- **It ends the launch's session as its last step**, so run it in a launch of its own. A session a refusal should have
  prevented is ended at once, so a run leaves nothing open.
- **It leaves a trace on your dashboards:** one filled-in form on its session, one `playtest_self_test` event, one
  Blueprint fault naming `FlockPlaytestSelfTestTarget` (raised twice, so its repeat is counted) and the launch's
  recording.
- **A step is skipped, saying why,** when the playtest does not turn its feature on; when the build has no video
  encoder, which is every platform but 64-bit Windows; when the launch cannot draw (`-nullrhi`), since nothing is
  recorded; when **Analytics Cache Failed Events** is off, since a report sent the moment
  it is made cannot be watched; and for a closed playtest unless you name one:
  `FlockPlaytest.SelfTest <Game Version ID of a closed playtest>`.

## Shipping builds

A Shipping build that has **Enable Playtesting** on, or that carries a playtest's `pt-` Game Version, gets a warning
while it builds, and the build still succeeds. The warning comes from the plugin's build rules, which a clean build
reads but an unchanged rebuild skips, so package a release from a clean build. A Shipping build also sends the Flock SDK's `exception_capture_limited`
entry, once per build, naming what it cannot capture there. Build playtests as Development, and point Game Version back
at a release version before shipping.

## Telling your players

A playtest build records the player's screen when the playtest turns video on, and sends what they type into the
feedback form. Tell your testers before they play: what is recorded, that it goes to your studio's Protokite
playtest, and how to reach you to have it removed. Nothing is collected unless **Enable Playtesting** is on in the build
and the playtest in Protokite turns the feature on, and a build without the plugin collects none of it.

The build asks them too, before collecting anything, and does what they answer -- see [What the player is
asked](#4-what-the-player-is-asked). That question is about this playtest alone; anything your game collects of its own
is still yours to ask about.

## What stays on the player's machine

| Folder under `Saved/FlockPlaytest/` | Holds | Until |
|---|---|---|
| `Recordings/Playtest/` | Playtest recordings, each with the session it belongs to | Uploaded, or deleted for disk room, oldest last |
| `Recordings/TestVideos/` | Test videos | Deleted for disk room, oldest first |
| `FeedbackForms/` | Answers that could not be sent | Sent, or refused by the server |
| `device_id.txt` | This install's device id, when no Steam id is available | Kept, so one install stays one player |
| `playtest_consent.json` | What the player let this playtest collect | Kept, so they are asked once rather than every launch |
