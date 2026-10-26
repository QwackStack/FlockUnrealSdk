# Changelog

All notable changes to this plugin will be documented in this file.

The format is based on [Keep a Changelog](http://keepachangelog.com/en/1.0.0/)
and this project adheres to [Semantic Versioning](http://semver.org/spec/v2.0.0.html).

## [1.38.3] - 2026-10-25

Flock's relay reached over UDP, TCP or TLS, and over IPv6.

### Fixed

- **A player whose network blocks UDP to Flock's relay now reaches it another way.** Until now the relay was reached over UDP
  and IPv4 alone, so on a network that drops UDP to it (some phone hotspots, offices, schools and hotels) hosting or joining
  through the relay failed with `relay_unreachable`. The SDK now tries UDP over IPv4, then UDP over IPv6, then TCP and TLS over
  IPv4, then over IPv6, each way Flock lists (for Flock's relay today: UDP and TCP on 3478, TLS on 5349). Each next way starts
  once the one before it has failed or had 2 seconds (half a second for another port of the same kind), so a blocked way costs
  about that, and a network where UDP over IPv4 works tries nothing else. The first way to open is kept and every other is
  closed, giving back any relay address it reserved. Over TCP or TLS a lost packet holds back the ones after it until it is sent
  again, so play may stutter on a poor connection; with Enable Debug Logs on, the log says once, at Info, which way the relay is
  reached by and why. Sends
  over TCP or TLS never wait on the network: a sending thread of the relay's own writes them, and a full queue drops a whole
  packet, as UDP does.
- **TLS is checked against what the engine trusts**: the platform's own certificates, as the engine's HTTP module uses, and any
  keys the game pins; the relay's certificate must name it. A network that inspects encrypted traffic is refused, and the reason
  says the certificate is not trusted.
- **A player on a network with IPv6 alone reaches the relay at its IPv6 address.** The relay still gives an IPv4 relay address,
  so the player plays with everyone else, and a direct path still offers IPv4 addresses alone. The relay server's IPv6 address
  is asked of the platform's own resolver: the engine's lookup drops every IPv6 answer while `net.DisableIPv6` is 1, its
  default, and the SDK leaves that setting and the engine's own networking as they are. On Android, whose engine sockets carry
  no IPv6 address, an IPv6 way is refused at once rather than waited on.
- **A joining player reaches the host's relay server by any way in to it**, not only by the exact address and port the host
  used: a host on UDP and a player on TLS play together. A relay server is told apart by its name.
- **When no way in opens, `relay_unreachable` names every way tried and why each failed** on one line (no answer within 5 s, the
  connection refused, a certificate this device does not trust), then one fix, which now names UDP, TCP and TLS. When one way
  reached the relay and was refused (a wrong login, no room), that reason is told rather than another way's silence.
- **A relay connection over TCP or TLS that the relay closes is told lost at once** (On Relay Lost, `relay_lost`), not after the
  relay's 45 s of silence.
- **With Enable Debug Logs on, the log says each way in to the relay as it is tried, and how and when each ended**, so a slow
  opening on a player's network can be read off the player's log.
- **`InitializeWithConfig` honors its config's Enable Debug Logs again when something logged before it.** With Auto-Initialize On
  Load off, the SDK says so at start, which made its default logger from the settings' Enable Debug Logs; the config the game then
  initialized with was ignored for it, so turning debug logs on in that config showed nothing (and turning them off there left
  the settings' breadcrumbs on). A logger the game hands in with `SetLogger` is still kept as it is. A game that builds its
  config in code and wants the settings' flag sets `bEnableDebugLogs` from them, or starts from `FFlockInitConfig::FromSettings`.

### Changed

- The plugin's `Flock` module links the engine's `SSL` module and its OpenSSL on Windows, Android, Mac, iOS and Linux, for TLS.
  A packaged game links nothing new there: the engine's HTTP module already links both.

## [1.38.2] - 2026-10-21

Codegen deletes only what it made.

### Fixed

- **Switching Codegen Target to C++, or running Clean Generated, deleted every asset under Generated Content Path**, at any
  depth, whether codegen made it or not, so a path of `/Game`, or a folder that also held the game's own assets, lost the
  project's content. Every asset codegen makes now carries a mark, and only marked assets are deleted; the game's own assets
  there are kept and named in the summary. A Generated Content Path that is a whole content folder (`/Game`, or a plugin's),
  is not a mounted content folder, has `.` or `..` in it, or is content outside the project (the engine's) is refused before
  anything is written or deleted, with what to set instead.
- **A Generated Code Path at the game's own module (`Source/MyGame`) deleted the game's code.** Every Blueprint sync and every
  Clean deleted each header in its `Public` folder and each source in its `Private` folder, and a C++ sync overwrote its
  `.Build.cs`. Codegen now replaces and deletes only files that open with its generated banner: the game's `.Build.cs` stops a
  C++ sync with what to set instead, and files of the game's own in a generated module's folders are kept.
- **A sync stopped the editor when the generated folder held a game asset of another kind under a name codegen writes** (a
  texture called `FlockContentCatalog`, say), because the engine cannot replace an object with one of another class. Codegen
  now leaves that asset alone, says which one it is and how to fix it, and generates the rest.

### Upgrading

- Assets generated by a Flock SDK before 1.38.2 carry no mark, so a Clean or a switch to C++ keeps them and names them. Run
  Sync Schemas once on the Blueprint target and codegen marks every asset it still makes; delete any left over by hand.
  Generated C++ files have always opened with the banner, so nothing changes for them.

## [1.38.1] - 2026-10-20

The SDK builds and runs on Android.

### Fixed

- **The SDK did not compile for Android.** Android's compiler refused two comments holding `/*` and two local names that hid a
  member, which the Windows compiler accepts; all changed, and a game for Android builds and runs (measured with UE 5.4 on a
  Galaxy S23 Ultra).
- **On Android no name was ever looked up**, so the relay server and the STUN servers were never reached and Travel To Session
  through the relay failed with `relay_unreachable`: the engine's lookup passed an absent service as an empty one, which
  Android's resolver refuses. The lookup now gives a numeric service. Measured on the phone on mobile data (an IPv6-only
  network) with a desktop host on another network: through Cloudflare's relay, then a direct path across the two networks.

## [1.38.0] - 2026-10-19

Direct paths for relayed players: a player who came through Flock's relay moves off it once it and the host reach each other.

### Added

- **Relay When Needed moves a relayed player to a direct path, and back.** Each end asks Flock's STUN servers and the relay
  server where they see it, offers that and its LAN address to the other end through the relay, and probes the other's
  addresses (and wherever the other's probes come from); once a probe is answered both ways the engine's packets go directly.
  A direct path quiet for 4 s goes back to the relay, the other end following, and the game's connection carries on through
  both: the host's net driver hears the player from the same port throughout. When no path proves itself (no offer within
  20 s, no answer within 6 s) the pair stays on the relay for that connection. Each move is said once at Info. A host offers
  its addresses only to players its check let in, and only probes naming both ends' keys count. Relay Always never moves, so
  every address stays hidden. Nothing new for a game to call.
- **A relay connection and a player's seat say the device's bridge reads the bridges' own messages** (`bridge_messages`, 1).
  The engine closes a connection that one of these messages reaches (measured: 10 of 10, at the host and at the player), so a
  bridge keeps every one of them from its engine, whatever their version, and sends none to a device that does not say it reads
  them: a host or player on 1.34.0 to 1.37.0 stays on the relay as before.

### Fixed

- **A relayed player connecting again through the same relay address was let back in only once its old connection timed out**,
  a minute later (measured: the engine handed the new connection to the old one, which still had that address, and Flock's
  check was never asked). The player's bridge now says through the relay when its game starts a new connection (its first, or
  one Travel To Session sees the engine start), and the host's gives it a loopback port of its own: back in within seconds (5 s
  after the player travelled, on Cloudflare's dev relay). A socket the engine remakes for the same connection keeps its place.
- **A relayed player's newer connection, let in before the host had seen its older one go, could lose its relay channel and,
  30 s later, its place on the bridge**: what the older one left was let go though the newer held it. Only what a gone connection
  held alone is let go now.

## [1.37.0] - 2026-10-19

Found by playing through Flock's relay on real networks (Cloudflare's relay, from this game's network).

### Fixed

- **A relay that stops answering this device is told lost within a minute** (On Relay Lost, `relay_lost`, the log saying the
  relay has answered nothing for 45 s), so Travel To Session again opens another. A relay's own packets can keep arriving after
  this device's stop reaching it (a router that moved the game to another port, a network that changed); until now nothing
  asked the relay while the game's packets went, so it was told lost only when an opening was renewed, minutes later, the
  player meanwhile timed out by the engine. The relay is now asked every 15 s whatever else goes (a binding request, which keeps
  the way open through a router too); a relay that never answered one is not judged by its silence.

## [1.36.0] - 2026-10-19

The multiplayer sample, made of Blueprints only.

### Added

- **`Content/Samples/Multiplayer`** (turn on Show Plugin Content in the Content Browser): `BP_FlockSampleGameInstance`, a Game
  Instance whose Event Graph signs in with a device name, hosts, joins by code, quick matches, travels to the session, travels
  again on On Host Changed and On Relay Lost, gives the seat up when a travel does not get in, and leaves, each part of its
  graph in a comment box saying what it does; `WBP_FlockMultiplayerSample`, a card in the screen's corner with a field and a button
  for each and the Game Instance's status line; and `FlockMultiplayerSample`, a level whose
  World Settings run Flock Game Mode Base (its host lets in only the session's players) and whose Level Blueprint shows the
  widget. There is no C++ in it: every Flock call is a node. Set the project's Game Instance Class to `BP_FlockSampleGameInstance`,
  open the level and play it in two standalone games; see the multiplayer guide's "The multiplayer sample".
- The assets are saved with Unreal Engine 5.4, so every engine the SDK supports (5.4 to 5.8) opens them; a test loads them and
  checks every event the sample's buttons call.

### Changed

- The guide and the comments name the engine's events as the editor shows them: the Game Instance's **NetworkError** and
  **TravelError**, the game mode's **OnPostLogin**, and an async node's **Async Action** pin.

## [1.35.0] - 2026-10-19

A host change, or a relay that stopped working, is one call for the game: Travel To Session again.

### Added

- **Travel To Session again takes over.** Called while a travel is under way, it ends that one with the new `restarted`
  outcome (its engine side left as a cancel leaves it: a level opening stops listening once open, a connection still being
  made is given up) and begins once that has settled; a travel started from the restarted answer takes over in turn. **Flock
  Travel Outcome Restarted** in Blueprint.
- **Called again for the session the game plays, Travel To Session does this device's part from where the game is**, read
  when it begins, so a host change is On Host Changed, then Travel To Session with the same options. A host already listening
  for the session keeps its level and its players, opens its relay address again when it was lost, and publishes again; a
  refused publish, a cancel or a newer travel leaves that level listening. A player that has become the host opens the level it
  played with the old host (unless `HostLevel` names one), though the engine may have taken it back to its default level
  meanwhile; its connection to the old host goes with the level change. A player with the old host waits for the new one in
  that level and connects (the engine closes the old connection as it sets off). A host that handed hosting over stops
  listening and joins the new host. A player already in the game of the host the session names, on the address that host
  published, is answered `connected` at once and keeps its connection (a level's Begin Play that travels runs on every player
  that arrives). A game connected to a server of its own is refused, though it played the session.
- **`OnRelayLost`** on the session (Blueprint **On Relay Lost**): raised once a relay when this device's relay address stops
  working while the session lasts, with why (a `FlockRelayFailureReasons` value), and never for a relay the SDK gave back (the
  session ended, the player left, another relay server, a shutdown). On a player, it is raised too when its host publishes
  another relay address (the host's own relay was lost), since the player's relay no longer reaches it; a new host's address is
  On Host Changed's. Travel To Session again opens another.

### Changed

- **A travel asked for while the engine changes level waits for it** (a seamless travel included), at most 30 s and then
  refused saying so, rather than being refused at once: the engine's own way out of a lost connection takes a frame. A travel
  no longer refuses another that is under way. A player's Wait For Host Seconds is checked when the travel begins, against
  this device's part then.
- **A player's earlier connection the host has heard nothing from for 5 s gives way to their newer one** once the newer is
  let in (the earlier is closed, said in the log): what a lost relay or a game that stopped leaves on the host until the
  engine's own timeout of a minute. A newer one that comes sooner waits, up to 5 s, while the earlier stays quiet; one the
  host hears from again keeps its place, the newer refused `already_connected`. The newer must bring a join token of its own
  (Travel To Session asks for one each time): a token stays good for its 60 s and travels in the clear, so the earlier
  connection's own token presented again is refused `already_connected`. One still waiting when the host's level stops
  listening is turned away `flock_unreachable`, as every player still being checked is.

## [1.34.0] - 2026-10-18

Travel To Session connects players through Flock's relay: a player who cannot reach the host directly (on another network,
behind a router with no forwarded port) joins it through the relay, and the game's networking code stays as it is.

### Added

- **`Relay` on Travel To Session's options** (`EFlockRelayUse`): **When Needed** (the default) takes a player on the host's
  network straight to the host, and through the relay when the host does not answer there within 3 s; every other player goes
  through the relay. **Always** takes every player through the relay, on the host's network too, and the host turns away any
  player that reaches it another way (`relay_only`, without asking Flock). **Never** keeps the relay out of it. Set it the
  same on every player of a game.
- **The engine's own net driver carries relayed players.** The SDK stands in for the other side on this device's loopback:
  a player's game travels to 127.0.0.1 on a port of the SDK's, which passes what it sends to the host's relay address; a host
  gets each relayed player from a loopback port of its own, so the engine sees a connection a player and its handshake, level
  changes (seamless or not) and timeouts work as on a direct connection. A host takes at most 64 relayed players; a relay
  address its check has not let in within 30 s gives its place back once a new one is heard. Packets the relay cannot carry
  (over 1400 bytes; the engine sends at most 1024) are dropped with one warning.
- **Only players the host's check let in get the relay's cheaper channel** (4 bytes a packet rather than 36), and a player's
  `PlayerForConnection` and `FFlockJoiningPlayer` work through the relay as directly (the address reads 127.0.0.1).
- **Results say how a travel went**: `bThroughRelay`, the `relay_failed` outcome with `RelayFailureReason` (a
  `FlockRelayFailureReasons` value, each with its own fix: no relay offered, the host offering none, its relay server not
  listed for the player, a paused relay, too many logins, no seat, Flock or the relay out of reach, a wrong login, a full
  relay, an opening refused, a lost or closed relay, no threads), and the `relay_only` refusal. Blueprint has a node for each.
- **The connection mode `relay`**: the host publishes its relay address and relay server (`relay_address`, `relay_server`),
  with its direct addresses too under When Needed. A relayed player reserves its address on the host's relay server and
  publishes it for its seat, and the host lets each seated player's relay address in, reading the session every 2 s while it
  takes players through the relay, so a relay that gives each address an IP of its own (Cloudflare's) carries every player.

### Changed

- **A host whose relay cannot be opened** publishes directly under When Needed (saying why, unless the relay is switched off
  for the game) and stops hosting under Always (`relay_failed`). A host that stopped listening while its relay opened
  publishes nothing.

### Fixed

- **A host's level change without seamless travel no longer turns its players away.** The engine brings every player back
  through a new connection carrying the join token of its first travel, which Flock refuses once its 60 s are up
  (`join_token_invalid`, and the player was sent to its default level). A player in its host's game now keeps a fresh join
  token on hand (a new one once the one it holds has less than 20 s left: about one request every 40 s) and the SDK puts it on
  that connection before it logs in, for direct connections and relayed ones alike, with nothing asked of the game. A
  token on hand asked for once the host has ended the session (before this game's heartbeat brings the end) is the
  session's end, learned from Flock's answer, not a failed call in the log.

## [1.33.0] - 2026-10-18

The relay client: the SDK reserves an address on Flock's relay for a session and carries packets through it. Games do not use
it yet: a later release connects players through it from Travel To Session.

### Added

- **A relay client inside the SDK**, with no new API yet. For the held session it asks Flock for relay logins named with the
  session, reserves an address on the first relay server Flock lists that answers, opens it to other players' relay
  addresses, and carries packets both ways (on channels once asked, 4 bytes a packet), on a thread of its own so the game's
  thread never waits and a packet allocates nothing on it. A relay that answers only some of a device's ports, as Cloudflare's
  does, is reserved from whichever of 8 sockets it answers; every socket is dual-stack where the platform allows. It signs
  again when the relay's nonce goes stale, renews what the relay keeps before it lapses, and gives the address back when the
  session ends (before `OnEnded`), the player signs out or the game quits, its stale nonce answered too. Each failure has its
  own reason and fix: Flock out of reach, relay logins refused (more than 12 a minute, or the seat's allowance of 10 used),
  no seat, the studio's relay paused, no relay offered, a relay server out of reach, a wrong login, a full relay, an opening
  refused, the relay lost.

### Changed

- **The STUN servers' names and the relay's are looked up by one shared lookup**, and the UDP sockets the SDK opens toward
  the internet can be opened from any thread.

### Fixed

- **A project whose game changes no longer sends the old game's analytics to the new one.** Each game's crash marker,
  live-session record and queued events are kept apart, under its backend and API key
  (`Saved/Flock/analytics/games/<key>/`). Every game a project ran used to share one folder, and a changed game sent the old
  one's unsent sessions and events to its own backend, which refused and dropped them. The launches an earlier build kept are
  taken over by the first game to start after the update. Consent stays per install.
- **A saved sign-in is restored only by the game it was made with.** Each game keeps its own file, under its backend and API
  key (`Saved/Flock/auth-<key>.dat`); it was kept per Game Name, so two backends' games of one name restored each other's
  player. The sign-in an earlier build kept is taken over by the first game to read one, so nobody is signed out by updating.

## [1.32.0] - 2026-10-17

Blueprint support for multiplayer: a game made in Blueprint makes and joins parties and sessions, finds a match, travels into
the session's game and lets in only the session's players.

### Added

- **An async node for every multiplayer call**, under *Flock | Multiplayer*: Flock Create Party, Join Party, Get My Party,
  Leave Party, Kick From Party, Make Party Leader, Update Party, Disband Party; Flock Host Session, Join Session, Get Session,
  Get My Session, Leave Session, End Session, Make Session Host, Publish Connection, Publish Direct Connection, Request Join
  Token, Verify Join Token, Find Direct Address, Wait For Connection; Flock Find Match and Party Find Match (On Matched, On No
  Match); Flock Travel To Session (On Joined, On Not Joined). Each fires one pin, the failure pin with the error; Get My Party
  and Get My Session tell none from one; Find Match, Party Find Match, Wait For Connection and Travel To Session can be
  cancelled from their Async Action pin, and a cancel the game asks for fires no pin. The game instance keeps a node until it
  has fired. Travel To Session warns when called from an object of the current level, which is gone once its pins fire.
- **A pure node for every value a multiplayer result carries** (`UFlockMultiplayerLibrary`): Flock Travel Outcome, Flock
  Refused Reason, Flock Matchmaking Outcome and Flock Session End Reason nodes, the connection mode and direct connection
  values, and Is Matched.
- **`AFlockGameModeBase` and `AFlockGameMode`**, game modes that let in only the session's players while the game hosts
  through Travel To Session: their `PreLoginAsync` runs Flock's check, the engine's own, then **Approve Joining Player**, the
  game's own check, overridable in Blueprint.

### Changed

- **A Blueprint node's calls are logged under the Blueprint that made them on every line**, a call that goes through another
  part of the SDK included: a multiplayer call's own lines read "C++" before (only the HTTP lines beneath named the Blueprint).
  For C++ that tags its own calls: `FFlockCallOriginScope` takes the caller's name alone (`FFlockCallOriginScope Scope(TEXT("..."))`),
  and `FFlockProviderBase::SetCallOrigin`/`GetCallOrigin` are gone; every provider reads the one `FFlockCallOrigin`.

### Fixed

- **A sign-in node no longer keeps the level that called it alive.** The game instance keeps a sign-in node until it answers,
  and the node held its caller, such as a menu widget: a sign-in still on its way when the game changed level kept the old
  level's world past the change, which the engine reports as a world not cleaned up by garbage collection. A check in the
  repository's CI now fails a node the game instance keeps that holds its caller.

## [1.31.0] - 2026-10-16

Join verification: a host that travels to its session with Travel To Session lets in only the session's players.

### Added

- **`FlockJoinChecks::CheckJoiningPlayer(GameMode, Options, Address, UniqueId, OnComplete, ApproveJoiningPlayer)`**, called
  from the host's game mode's `PreLoginAsync` in place of the engine's own. While the game hosts through Travel To Session, a
  joining player is let in only once Flock has verified the join token their travel URL carries, one connection a player;
  then the engine's own checks (`PreLogin`, such as a full game) run, then the game's own `ApproveJoiningPlayer`, which gets
  an `FFlockJoiningPlayer` (the verified `PlayerId`, the URL's `Options` without the token, `Address`, `UniqueId`).
  `OnComplete` is always called and the game thread never waits. While the game does not host through Travel To Session,
  it is the engine's own `PreLoginAsync`.
- **`UFlockMultiplayerSession::PlayerForConnection(PlayerController)`** (Blueprint pure): the Flock player behind one of the
  host's player controllers, its own included; empty for a connection gone or never let in by Flock's check.
- **Travel To Session's `refused` outcome** with `FFlockTravelToSessionResult::RefusedReason`, and
  `FlockTravelToSessionRefusedReasons`: `no_join_token` (Flock not asked), `join_token_invalid` (Flock's one answer for a
  made-up token, another session's, an expired one and one whose player gave up their seat), `already_connected` (a token
  checks out any number of times within its 60 s, so the host itself lets one connection a player in), `flock_unreachable`
  (no answer within 8 s, an answer that cannot be read, or the host over its checks a minute: nobody is let in unchecked),
  `session_ended`, `not_host`, `refused_by_the_game`, and `join_check_missing` (the host's game mode does not call the
  check, so nobody is let in; told once at Error). The reason can also be the game's own or the engine's words. A refusal is
  logged on the joining player with its fix.

### Changed

- **Travel To Session asks for a join token** once the host's address is known and travels with it (`?FlockJoinToken=`); a
  token Flock will not give fails the travel with Flock's error, before anything travels. The host turns its check of joining
  players on before its level listens, and off once the game no longer listens (a host travel that does not start hosting
  stops listening), at another host travel, or when Flock shuts down; every player still being checked is then turned away
  rather than left waiting, since the engine never drops a player its game mode has not answered (measured). Flock shut down
  while the hosted level stays open turns every joining player away (`flock_unreachable`, told once) until that level closes.
- **A join token Flock does not vouch for is logged at Debug on the host**, not as a warning: it is Flock's answer, which the
  host's check turns into a refusal, and anyone who reaches a host can send one (a flood of made-up tokens wrote one warning a
  check, 60 a minute). Flock turning the host's checks away for volume, and any other answer of that route, still warns.

### Fixed

- **The SDK did not compile on UE 5.4 in the editor or a Development build** (since 1.30.1): three of its own test files used
  a string check UE 5.4 does not have without the stand-in the SDK supplies for it. A check in the repository's CI now fails a
  file that does that.

## [1.30.1] - 2026-10-15

Every failure says what is wrong and how to fix it.

### Changed

- **`FFlockError::Message` is what is wrong**: the server's own reason when it gave one (`Shop item with ID 'GoldItem' not
  found`), otherwise the plain meaning of the status or the transport failure (`The server found nothing at this address`,
  `The server did not answer in time`). It no longer reads `HTTP request failed (HTTP 404)`, `Authentication failed (HTTP 403)`
  or `Validation failed (HTTP 422)`, and a bare 403 is a refusal, no longer called an authentication failure. It never holds
  the status: `ToDisplayText()` tags it.
- **`FFlockError::Hint` is never empty**: the code's advice, or the advice for the kind of failure when the code has none
  (`FFlockErrorHints::ForFailure`): an answer without a Flock code (a proxy, a captive portal, an API URL pointing elsewhere),
  a code newer than this SDK (named in the fix), a server failure, a timeout (a change may already have been made, so read
  before re-sending), no connection, an unreadable success. `ToDisplayText()` always ends with its `Fix:` line.
- **The Output Log's line for a failed call** carries the same text as `ToDisplayText()`, fix included; it held only the
  terse message, which is what a developer saw for a purchase given the wrong id.
- **Every failure the SDK finds itself names what to do**, built with the new `FFlockError::MakeWithFix`: an empty argument
  says what to pass, a call with nobody signed in says to sign in, a node run before Flock initialized says how it starts,
  a session or party that has ended names how to get another. CI fails on a Validation, Network or Auth failure built with
  `FFlockError::Make` outside the HTTP client.
- **A board read's 404 is the caller's mistake only when it is the board's** (`leaderboard.not_found`); a wrong API URL's
  `request.not_found` keeps its own reason and fix rather than sending the developer to check a board name.
- **The setup panel's connection test** tells a rejected key and an unknown Game Version by their codes. A wrong API URL
  answers 404 or an uncoded 401, which it called "no game version by that name" or "the API Key was rejected"; it now shows
  the error's own reason and fix.
- `FFlockResolveResult::Fail` (editor) takes the fix with the failure.
- **A failed download of an asset's file** names the storage that serves it and storage's own fix (a missing file is
  uploaded again, an expired link read again), rather than calling storage's answer a wrong API URL.
  `FFlockError::MakeWithFix` takes the status of such an answer.
- **A failed call to Protokite** (the optional playtest plugin) says what Protokite meant and how to fix it
  (`FProtokiteClient::FixFor`): a key Protokite does not know names API Key, a build no playtest is linked to says to link
  the playtest to the build's Game Version, a missing form says to publish it, and an answer that is not Protokite's (a
  captive portal, a proxy) names Protokite API URL in the playtest's settings. Protokite's refusals carry no Flock error
  code, so the Flock SDK's fix for an answer without one, which points at the Flock API URL, no longer reaches them.

### Added

- `EFlockErrorCode` members for the codes the server sends on any route: `RequestNotFound` (no route at that address, or a record a route did not find; a wrong API URL lands here, as when
  API URL ends in `/v1`), `RequestMethodNotAllowed`, `RequestValidationFailed` (a member missing from the body, named in the
  message), `RequestUnauthorized`, `RequestForbidden`, `RequestConflict`, `RequestPayloadTooLarge`, `RequestRateLimited` and
  `RequestFailed`; the API key check's `GameMissingApiKey` and `GameInvalidApiKey`; and the sign-in check's
  `PlayerMissingToken`, `PlayerInvalidToken` and `PlayerExpiredToken`. Each has a hint.
- `FFlockError::MakeWithFix`, `FFlockError::ReasonAndFix()` (the display text without the operation), and
  `FFlockErrorHints::ForFailure` with the fixes several failures share.

### Fixed

- **A purchase given the item's name or the shop's id** read `HTTP request failed (HTTP 404)`. The shop item hints now say
  a purchase takes `FFlockShopItem::Id`, read from the shop, not the item's name or the shop's id, and the shop guide shows
  reading it.
- **The wallet hints** told a player with no wallet, or no balance in a currency, to call Flock Add Game Funds to create it,
  which refuses the same way: a wallet is made when the player registers, from the Game Version's currency template.
  `ShopWalletNotFound`, `ShopCurrencyNotHeld`, `ShopRewardCurrencyNotHeld`, `ShopCurrencyTemplateNotFound` and
  `GameCommandCurrencyNotFound` now say so.
- `ShopShopNotFound` names the Game Version a shop belongs to, and `ShopPlayerNotFound` the player a purchase acts on.
- **A feedback form Protokite refused with no reason of its own** could be logged as refusing a question cut out of the
  fix that follows the reason; the question is read out of Protokite's own reason only.

## [1.30.0] - 2026-10-15

Connecting players.

### Added

- **`UFlockMultiplayerSession::TravelToSession(WorldContextObject, Options, OnComplete)`** brings the game into the session's
  game. The host opens `Options.HostLevel` (empty: the level it is in) listening for players, the engine's `?listen`, and once
  it listens publishes how to reach it on the port its net driver bound (the engine takes the next free port when the one
  asked for is taken, so that is the one published). A player waits for that (`WaitForHostSeconds`, 60), travels to the host's
  LAN address when both share a public address and to the published address otherwise, and completes once the host has let it
  in. It completes with `FFlockTravelToSessionResult`: `Outcome` (`FlockTravelToSessionOutcomes`: `started_as_host`,
  `connected`, `host_never_published`, `session_ended`, `could_not_connect`, `could_not_start`, `not_direct`, an open set),
  `bIsHost`, `Address`, `Port` and the host's `Connection`. A connection that fails is the engine's to give up: it takes the
  game back to its default level, and the call completes `could_not_connect` with a warning naming the address and the port
  forwarding a host outside the player's network needs. The completion runs in the new level.
- **`PublishDirectConnection(Port, OnComplete)`**: the host publishes a `direct` connection, `{address, port, lan_address,
  public_address}`: its public address from one request to the STUN server Flock lists (asked with no session, so a direct
  connection is not counted as relay use; read once a launch, a failed read not kept), its LAN address from the route toward
  that server, and `address` the public one when known. An unknown address is left out; a port outside 1 to 65535 is refused
  before any request. The STUN request goes from a dual-stack socket where the platform makes one (a phone on mobile data
  hears some STUN servers only that way), a plain IPv4 one otherwise.
- **`WaitForConnection(TimeoutSeconds, OnComplete)`** answers the session once the host has published, once it has ended, or
  with neither when the time runs out, reading the session every 2 s meanwhile (heartbeats come every 20 s). A host change
  clears the address without ending the wait. **`FindDirectAddress(OnComplete)`** says which of the host's addresses this
  game uses: only a dotted IPv4 address and a whole port from 1 to 65535 count (a port sent as text is taken).
- A connection of the game's own (Steam, EOS) is published with `PublishConnection(Mode, Values)` as before; a player's
  `TravelToSession` ends `not_direct` holding it.
- `Documentation/multiplayer.md` covers connecting players.

## [1.29.0] - 2026-10-15

Matchmaking.

### Added

- **`FFlockMultiplayerProvider::FindMatch(QueueName, Options, OnComplete)`** searches a queue by its name (letter for letter)
  and completes with `FFlockMatchmakingResult`: `Outcome` (`FlockMatchmakingOutcomes`: `matched`, `expired`,
  `party_changed`, `cancelled`, an open set), the `Session` the match seats the player in, held like any other session, and
  the matched `PlayerIds`, the longest waiting first. A search that ends without a match is an outcome, not a failure.
  Cancelling the handle cancels the search on the server; a match that landed meanwhile gives its seat up first. A search
  of the player's own left from an earlier launch is cancelled and replaced; a party's is never touched by a search alone.
- **`UFlockParty::FindMatch`**, **`IsSearching()`**, **`OnSearchStarted`** and **`OnSearchEnded(Result)`**: the leader's game
  searches as the party, matched as one group and never split; another member's `FindMatch` waits for the leader's search,
  and every member's game hears the search start and end, from its own call or the party's refresh, before the call waiting
  for it completes. A search from before the game first held the party is not told.
- The search is checked every 3 seconds (2.25 to 3.75), only for the sign-in it was made under: a sign-in that ends ends
  the search as `Cancelled` and sends nothing more, and frees the next player's `FindMatch` at once. Quitting the game cancels
  it on the way out without waiting; a search still being made is cancelled once its ticket lands, and a match already
  landing gives its seat up.
- A member's wait for the leader's search ends (`Validation`) when the party ends first, or when the player comes to lead it.
- The queue list is read once a launch; a queue deleted and made again under the same name is found by its new id.
- `Documentation/multiplayer.md` covers matchmaking; the matchmaking error hints name `FindMatch`.

### Fixed

- **Cancelling a party create, join or `GetMyParty` while it read the player's search** did nothing: the cancel reached the
  request on its way only while something else still held the call, and that last reading held nothing. A call made of
  requests sent in turn now always reaches the one on its way; sessions' calls share the same mechanism.
- **A player with no search** (asked by every party refresh) is traced at debug instead of as a warning.
- **A shop item priced in real money** sent through `Purchase` now fails with its own code,
  `EFlockErrorCode::ShopRealCurrencyNeedsCheckout`, and a hint, instead of `Unknown`: the server refuses it because the call
  only spends game currencies from the wallet. The shop guide no longer says such a purchase can come back `pending`, nor
  that a purchase or consume fails "immediately" offline (each is sent once and reports the connection failure); the 1.28.0
  entry below names the status constants `FlockPurchaseStatuses`, as the code does.
- `FFlockMultiplayerProvider` no longer hands its matchmaking a lookup that called back into the provider; the queue list
  is one shared object, read through the matchmaking requests with the API key alone.

## [1.28.1] - 2026-10-14

Follow-ups to the 1.28.0 review, and keys compared letter for letter.

### Changed

- **Metadata keys and event property names are compared letter for letter.** `Score` and `score` are two keys. The engine's
  map and JSON object cannot keep both (they would replace one with the other without a word), so a second spelling is not
  added, a warning names both, and the first stays. `FFlockMetadata::Add`, the `Add Metadata ...` and `Flock Event Property
  (...)` nodes and `FFlockCommandData::Set` all follow it; repeated-exception windows have since 1.28.0.
- **Player ids are compared letter for letter** in the command queue's flush and reload and in the analytics events batch,
  as every other identifier is.
- **Player features** are documented as kept in memory per player for the run (`FFlockConfigProvider::GetPlayerFeatures`
  and `Documentation/game-config.md`), which is what the code has done since they were added; nothing changed in behaviour.

### Fixed

- **An API URL of nothing but spaces and slashes** is refused at initialization with an error naming the setting; it
  initialized and sent every request to `/v1/...`.
- **A playtest recording whose session file cannot be read** has its cut-off video finished before it is kept, so what is
  kept can be played.
- The three HTTP transports claim their one answer through `FFlockCancelToken::TryClaimAnswer()` rather than each by hand.

## [1.28.0] - 2026-10-14

Fixes found while writing the SDK guide, across the core, the editor, codegen and the Protokite Playtest plugin.

### Added

- **`FFlockPlayerData::LeaderboardRefusals`**: the boards that turned away the score an update fed them (Update Player Data
  and Update Player Data Field), each with a reason (`FlockLeaderboardRefusalReasons`, an open set), whether it can be retried
  and when. An empty list after Add Game Funds or Unlock Achievement means those routes do not report refusals.
- **`FFlockPurchaseResult::Status`** (`FlockPurchaseStatuses`) and `IsPaid()`, with Blueprint nodes.
- **Editor: Don't Open For This** on a finding that opens the Flock panel by itself; it stays listed.

### Changed

- **`ResetPassword` works signed out**: the route needs no sign-in, so a player who forgot their password can use it.
- **An analytics transaction is sent once**: a timeout or 5xx after the server may have stored it is not sent again (only
  408 and 429 are), so revenue is never counted twice.
- **Codegen, C++ target**: two templates or fields making one name give the later a `_2` suffix (the declared name still
  goes on the wire); dashboard text in generated code is escaped; a re-sync leaves an unchanged header untouched, so nothing
  rebuilds; entities are walked in id order, so a suffix never moves between syncs.
- **The upgrade notice counts as seen only once the Flock panel has shown it**; a launch that opens nothing keeps it.

### Fixed

- **Core**: a request the engine refuses to start is answered once; a cancelled request is no longer logged as a failure to
  reach the server; the call replayed after a token refresh can be cancelled; a sign-in whose refresh the server rejects
  ends as a sign-out does (analytics told, the player's caches cleared); a retry wait never passes `MaxDelay`, and a negative
  `Retry-After` means now; a failure that is never retried is not warned about, and a final failure is logged once; a
  paginated list refuses an item that is not an object; the API URL is trimmed like the editor's; shutdown waits for
  requests in flight with analytics off.
- **Commands**: an online save waits behind older queued saves, so their replay cannot undo it; a flush stops when its
  player signs out.
- **Analytics**: an ended launch's queued entry that cannot be moved stays for a later launch rather than being deleted; a
  launch that cannot hold a folder keeps nothing on disk; diagnostics logged from another thread are queued on the game
  thread; a crash is written to disk only, and its marker stays until its report is queued; faults differing only in
  letter case are counted apart.
- **Notifications**: the seen marker and the pending reminders survive a new game version (and an earlier build's are
  rescued); a reply landing after another sign-in raises nothing and is tracked for the player who asked; the template
  list no longer chooses a locale for a by-name schedule.
- **Account linking**: a reply landing after another sign-in no longer sets the next session's email flag.
- **Assets**: a download larger than the cache budget is not kept (and never evicts the file just kept); a download that
  is not kept is written beside the cache and cleaned up; a name is matched letter for letter.
- **Editor**: Resolve and the startup bake refresh the panel once they answer, and the startup checks wait for the bake;
  the settings banner counts what it names (notes, not "0 items"); the help text names both codegen targets.
- **Codegen**: Add Funds with no `currency`-tagged template finds the wallet by tag instead of sending an empty id, which
  was refused on every call; a Blueprint-target sync or Clean writes nothing under `Source/` for a project with no generated
  module; version ids are compared letter for letter; lists and maps of generated structs use the wire-name table in both
  directions.
- **UE 5.4: the editor target builds again.** Seventeen test files used `TestEqualSensitive`, which UE 5.4 lacks, without
  the stand-in `Misc/FlockEngineCompat.h` supplies, so a 5.4 editor build failed to compile its tests.
- **Protokite Playtest**: a run or launch folder that cannot be removed keeps its lock, so a later launch finds it; a
  recording whose session file cannot be parsed is kept; a test video never deletes a recording waiting to upload; the
  feedback form opens for any loaded playtest, a player who answered "nothing" included; kept forms are sent one at a time,
  each claimed so two launches never send one twice; a fresh upload link is asked for only where one can help; an upload
  that stops moving is given up after 60 s; recordings carry their colour standard (BT.601, studio range) in the stream and
  the file; a WebM's duration ends at its last frame's end; the self-test's wrong key no longer says `flock-playtest`.

- **A party or session this game is leaving ends as `Left`, however the SDK learns it is gone.** Three ways ignored a leave
  still on its way: a party replaced by creating or joining another ended `Removed`; a party call answered `party.not_found`
  ended `Removed` (a disband answered so now ends `Disbanded`); and a session that could not be read to learn why it ended
  ended `dropped`.
- **Protokite Playtest: pushing the recordings earlier launches left no longer keeps its work in memory for the rest of the
  launch.** The step that uploads them one at a time held itself, so it, the list of recordings, the request headers and
  the completion were never freed.
- **Protokite Playtest: a recording keeps the game's shape when the window changes shape.** After a resize to another
  shape (a window made taller, or turned) the picture was stretched to the video's size; it is now recorded at the largest
  size of its new shape that fits, between black bars; a side within 16 pixels of the video's still fills it, so rounding
  never draws thin lines.

### Documentation

- **README names a known engine issue on UE 5.4 and 5.5**: a Development package ends with an access violation
  (`0xC0000005`) about 60 seconds after it quits. It is the engine's, fixed by Epic in 5.6, and happens whether or not
  any Flock feature ran.

## [1.27.0] - 2026-10-13

### Added

- **Sessions.** One player hosts a game the others join by code.
  - `FFlockMultiplayerProvider::HostSession(MaxPlayers, Data, bSameVersionOnly, OnComplete)` makes a session with the
    signed-in player as its host and only player. Leave the size unset for the game's own player count from the dashboard (8 unless changed there); it takes 2 to 16. The
    data is the game's own, such as a map, kept with its keys as written. With `bSameVersionOnly` (the server's default,
    so it is sent only when false) only players on the host's Game Version can join.
  - `JoinSession(JoinCode, OnComplete)` takes a seat by code, in any letter case. `GetSession(SessionId, OnComplete)` reads
    a session the player holds or once held a seat in; one the player is no longer seated in comes back ended and is not
    held. `GetMySession(OnComplete)` finds the session the player is seated in without its id, or null when seated nowhere.
  - Each completes with a `UFlockMultiplayerSession` (Blueprint type): its id, join code, host and whether the player hosts,
    the players seated now (longest seated first), size, data (`FFlockJsonData`), the host's published connection
    (`HasConnection`, `GetConnection`: a mode and its values) and why it ended. Its calls are `Leave`, `End`, `MakeHost`,
    `PublishConnection`, `RequestJoinToken` and `VerifyJoinToken`.
  - Its events, assignable from Blueprint: `OnPlayersChanged`, `OnHostChanged`, `OnConnectionChanged` (also raised when
    hosting moves and the old host's address is cleared) and `OnEnded`, raised once with a reason string:
    `FlockMultiplayerSessionEndReasons` names the server's (`ended_by_host`, `host_left`, `expired`, `empty`) and the
    SDK's (`left`, `dropped`, `moved_to_another_session`, `signed_out`); the server can add more.
  - **Heartbeats.** While a session is held the SDK keeps the seat with a heartbeat at the interval the server gives (20 s
    at the defaults). Every session route answers the whole session, so each heartbeat's answer brings the session up to
    date and raises its events with no read by the game. Only the newest reading changes the session: an older one, or one
    whose host or connection epoch is lower than the one held, changes nothing.
  - A call that finds the player out of the session reads it once to learn why and ends it with that reason (`dropped`
    when it cannot be read). Hosting or joining another session ends the held one at once as `moved_to_another_session`.
  - The game instance shutting down (the game quitting, or Play In Editor stopping) gives the seat up without waiting for
    the answer; the session ends as `left`, raising nothing. `ShutdownSdk` keeps the seat.
  - Host, end and make host are never sent twice after a lost answer. Every session call is sent only for the sign-in it was
    made under.

### Changed

- The session error hints name the calls that fix them (`HostSession`, `JoinSession`, `GetSession`, `RequestJoinToken`).

### Fixed

- A party that another player removed the player from, or that ended some other way, while this game's leave or disband of
  an earlier party was still on its way, ended as `Left` or `Disbanded`. Only the party the game itself left or disbanded
  ends that way now; any other ends as `Removed`.

## [1.26.0] - 2026-10-13

### Added

- **Parties.** Players group up by invite code before they search for a match together.
  - `FFlockMultiplayerProvider::CreateParty(MaxSize, Settings, OnComplete)` makes a party with the signed-in player as its
    leader and only member. Leave the size unset for the server's default (4); it takes 2 to 64 players. The settings are
    the game's own data on the party, such as a mode, kept with their keys exactly as written.
  - `JoinParty(InviteCode, OnComplete)` joins by code, in any letter case. Joining the party the player is already in returns
    it.
  - `GetMyParty(OnComplete)` reads the player's party, or null when the player is in none. A party lasts until the player
    leaves it, across launches, so call it after signing in. It returns the same object while the party lasts.
  - Each completes with a `UFlockParty` (Blueprint type): its id, invite code, leader, members in the order they joined, size,
    settings (`FFlockJsonData`), and why it ended (`EFlockPartyEndReason`). Its calls are `Leave`, `Kick`, `MakeLeader`,
    `Update` and `Disband`, each completing with the party as it stands afterwards, and each call returns a handle that
    cancels the request on its way.
  - Its events, assignable from Blueprint: `OnMembersChanged`, `OnLeaderChanged` and `OnEnded`, raised once with
    `Left`, `Disbanded`, `Removed` (the server no longer lists the player in it) or `SignedOut` (the player signed out or
    another signed in; raised on the next frame, while the party reads as ended at once). Flock shutting down ends it
    without raising anything.
  - The SDK reads a held party again every **Party Refresh Seconds** (Project Settings > Flock > Multiplayer, 10 by
    default, 0 turns it off), after each change this game makes, and on `GetMyParty`. Only the newest reading changes the
    party: a reading sent before another one landed, or before a change this game made finished, changes nothing.
  - The game's own leave reads as `Left` even when a refresh sees it first or the server says the player is already out.
    A change answered `party.not_found` ends the party as `Removed`. A refresh never takes up another party; only a call
    the game makes does.
  - Create, kick, make leader and disband are never sent twice after a lost answer. Every party call is sent only for the
    sign-in it was made under.

### Changed

- The party error hints name the calls that fix them (`GetMyParty`, `Leave`, `MakeLeader`, `Disband`).

## [1.25.0] - 2026-10-13

### Added

- **Multiplayer setup.** `UFlockSubsystem::GetMultiplayerProvider()` returns `FFlockMultiplayerProvider`, built when the
  SDK starts and torn down when it shuts down. It is where the multiplayer features will live; this release gives it two
  things.
  - **`FindQueueId(QueueName, OnComplete)`** finds a matchmaking queue's id by its exact name, letter case included. It
    reads the game's queue list (`GET /v1/matchmaking/queues?limit=200`, API key only) once a launch, shares that read
    with every caller asking at the same time, and reads again only for a name the list it kept does not have. Of two
    queues with one name the older is used, with one warning. A missing name fails `Validation`, coded
    `matchmaking.queue_not_found`, and its hint names a queue that differs only in letter case, or says when the list was
    full (200 queues) and a newer queue may be missing from it. Cancelling the handle ends that caller's wait at once, and
    a lookup still waiting when Flock shuts down is answered `Cancelled`.
  - **Repeating calls** (`FFlockRepeatingCalls`), which the multiplayer features use for search checks, heartbeats and
    party refreshes. One request at a time each, every 0.75 to 1.25 times its interval, sent from the core ticker (which
    keeps ticking while a desktop game is minimized or covered), for the sign-in it started under. A server's
    `Retry-After` is honoured. No answer, a timeout, a 5xx, 408, 429 or an unreadable answer keeps it going; any other
    failure stops it and is reported once. The SDK's own features start them; a game does not.

### Changed

- **A 403 is never answered with a token refresh.** A 401 still refreshes the token and sends the call once more. A 403
  means the player is not allowed (not the host, not the party leader), which a new token does not change, so the 403 is
  the answer. Until now both refreshed.
- **A call made for a player stays that player's.** The calls that act as the signed-in player through their token send
  every attempt -- retries, and the replay after a token refresh -- only while that sign-in lasts. After a sign-out or
  another sign-in they fail `Cancelled` and send nothing, so a retry never goes out as the next player; one made while
  nobody was signed in stops the same way once someone signs in. Each attempt reads the token as it is then. These are
  the inbox, unread count, summary, mark read and mark all read, scheduling, cancelling and listing schedules, device
  token register and unregister, your leaderboard rank and the standings around you, sending and checking the
  verification email, revoking the token, listing, linking and unlinking accounts, and a purchase. The calls that do
  other work first keep the sign-in they were asked for under: a purchase (it reads its item first, and records its
  analytics transactions only for that sign-in), scheduling by template name (it looks the template up first) and
  cancelling every schedule (it reads the list first, and a sign-in change stops the walk with the rest still tracked).
  Game-wide reads keep retrying as before.
- **A token refresh answered after its sign-in ended changes nothing.** It is not saved, raises no event and signs nobody
  out, and a player who signed in meanwhile gets a refresh of their own when one is needed.
- **A cancelled call is no longer answered from the offline cache.** It fails `Cancelled`, as it does without a cache.
- `FFlockAuthSession` gains `GetSignInNumber()` (a sign-in and a sign-out move it, a refresh does not), `GetBaseHeaders()`
  and a `RefreshAccessToken(ForSignInNumber, OnComplete)` overload. `FFlockProviderBase` gains `ForTheCurrentSignInOnly`
  and `ForSignInOnly`, which wrap an operation in the rule above.

### Fixed

- **A game module calling `FFlockStructuredData::GetDataAs<T>` or `FFlockJsonData::GetAs<T>` no longer fails to link.**
  Both converted the data inline, in the game's own module, which links neither `Json` nor `JsonUtilities` through Flock
  (measured in an editor build, with Flock listed private and public), so the editor build failed with an unresolved
  `FJsonObjectConverter::JsonObjectToUStruct`. The conversion now runs inside Flock, through the new
  `GetDataAsStruct(StructType, OutStruct)` and `GetAsStruct(StructType, OutStruct)`, which also take a struct type known
  at run time. A game module needs only `"Flock"`; code that builds its own requests on Flock's HTTP layer adds `"Json"`
  and `"JsonUtilities"` (README, Installation).

## [1.24.0] - 2026-10-13

### Added

- **Matchmaking, party and session error codes.** `EFlockErrorCode` gains 29 members, each with a hint: 9 `matchmaking.`,
  12 `multiplayer.` and 8 `party.` codes, so a refusal from those services reads as its own code instead of `Unknown`.
  Four of them (`MatchmakingNotPartyLeader`, `MatchmakingPlayerNotEligible`, `MultiplayerNotHost`, `PartyNotPartyLeader`)
  arrive as HTTP 403 and mean "not allowed", not "signed out"; their hints never send the player to sign in. A 404 from
  these services usually means "not yours, or no longer live". Members are named after their wire codes, so
  `matchmaking.not_party_leader` and `party.not_party_leader` are two members. No multiplayer call ships yet.

### Changed

- `EFlockErrorCode` members are grouped by namespace, so the new groups sit between existing ones and every member after
  `LogEventGameNotFound` has a new number. Blueprint pins and C++ refer to members by name and are unaffected; anything
  that stored the raw number needs it re-mapped.

## [1.23.0] - 2026-09-24

### Changed

- **The Protokite Playtest plugin's Protokite API URL now defaults to production, `https://api-protokite.qwacks.com`.** A
  studio no longer types it: turning on **Enable Playtesting** is enough. A project that already set the URL keeps its
  own value; set `http://localhost:8020` (or your own address) to point at a local Protokite. The default is
  `UProtokitePlaytestSettings::DefaultProtokiteApiUrl`.

## [1.22.0] - 2026-09-24

### Added

- **Unreal Engine 5.4 is supported.** The range is now 5.4 to 5.8. A 5.4 project can install the SDK and the
  Protokite playtest plugin without changing either, and nothing a game calls changed -- this widens the floor,
  it does not move any API.
  - **5.4 is verified to a lower bar than the rest of the range, and says so in README.md.** It has been built
    and linked in a real 5.4 project -- all four modules, editor target, no errors and no warnings -- but
    `Tooling/Build-AllEngines.ps1` has not been run on it, so there are no test counts for 5.4 the way there are
    for 5.5 to 5.8. Run the sweep before a release states 5.4 the way it states the others.
- Two engine differences needed a guard, and both live in `Source/Flock/Public/Misc/FlockEngineCompat.h` with the
  rest of the version claim:
  - **`UUserDefinedStruct`'s header has no portable spelling.** It moved from `Engine/` to `StructUtils/` in 5.5,
    and the old path is not a fallback: from 5.6 the `Engine/` header forwards only under
    `UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_5`, which a module on the latest include order does not have. The
    seven codegen and codegen-test files now include `FLOCK_USER_DEFINED_STRUCT_HEADER` instead of either path.
  - **`FAutomationTestBase::TestEqualSensitive` arrived in 5.5.** Below that the SDK supplies its own, so the
    same test source reads letter case the same way on every engine in the range. Test-only: nothing a game
    compiles depends on it.

### Changed

- The compat header no longer claims nothing in the SDK is version-conditional, and its list of version-sensitive
  APIs gains `UUserDefinedStruct`'s header, `TestEqualSensitive` and `EAutomationTestFlags` (a struct-scoped enum
  before 5.5, an enum class from it -- the spellings the tests use compile as both, and nothing stores the type).
- The note about `FJsonObject::Values` now says what the code actually does: ten places loop over it directly and
  eight name the pair `TPair<FString, ...>`, which is what would break first if a later engine interns the key.
- The Protokite playtest build rules said the engine ships libvpx on 5.5 to 5.8. **5.4 ships it too**, with a
  Win64 library, so video recording works there as well.

### Fixed

- **A success with no body is a success on every route that has nothing to read.** The HTTP client refused any
  2xx with an empty body as "Empty response from server", so a 204 on a log event, a gameplay event, a transaction
  or a session end read as a failure. A spooled session end answered that way was dropped with a warning that it
  had been rejected, and a transaction reported failing though the server had recorded it. The six analytics sends
  now use `PostJsonAcceptingNoContent` / `PatchJsonAcceptingNoContent`: a 2xx with no body or with any JSON body
  is a success and nothing in it is read, while a 200 that is not JSON (a captive portal's page) still fails.
  Reads keep refusing an empty body, since a read exists for what the body carries.
- **A session end answered by something other than the server is kept.** A 2xx whose body could not be read was
  treated as a final refusal and the spooled end deleted, though the server never saw it. It now stays spooled
  and goes out on the next trigger, the same as a batch of events already did.
- **The Protokite playtest's session end uses the same verb.** Its own special case for a 204, which existed
  only because the SDK's client did not accept one, is gone.

## [1.21.0] - 2026-09-22

### Changed

- **The playtest plugin is now called Protokite Playtest, in its name as well as its wording.** It ships as
  `ProtokitePlaytest-<version>.zip` and installs to `Plugins/ProtokitePlaytest/`, its two modules are
  `ProtokitePlaytest` and `ProtokitePlaytestEditor`, and its types, headers, log category, console commands,
  Blueprint nodes and settings carry the same name. Only the plugin changed: the Flock SDK is untouched, and a
  project that does not install the playtest plugin has nothing to do.
- **What to change in a project that already uses it**, all of it mechanical:
  - Replace the plugin folder, and change the `.uproject` entry from `FlockPlaytest` to `ProtokitePlaytest`.
  - In `DefaultGame.ini`, rename the section `[/Script/FlockPlaytest.FlockPlaytestSettings]` to
    `[/Script/ProtokitePlaytest.ProtokitePlaytestSettings]`, and `FlockPlaytestLocalSettings` likewise in
    `EditorPerProjectUserSettings`. **A settings section left under the old name is read by nothing**, so the
    build falls back to the shipped defaults, which means playtesting off.
  - C++: `FlockPlaytest*` headers and types are `ProtokitePlaytest*`, `FFlockProtokiteClient` is
    `FProtokiteClient`, `FFlockRunningSteamAccount` is `FProtokitePlaytestRunningSteamAccount`,
    `EFlockDeviceIdFileResult` is `EProtokitePlaytestDeviceIdFileResult`, `FLOCKPLAYTEST_API` is
    `PROTOKITEPLAYTEST_API` and `WITH_FLOCK_PLAYTEST_VIDEO` is `WITH_PROTOKITE_PLAYTEST_VIDEO`.
  - Blueprint: the nodes read **Protokite ...** rather than **Flock ...** and sit under the **Protokite | Playtest**
    category. A graph that called one has to be repointed at the renamed node.
  - Console commands are `ProtokitePlaytest.<command>`, and the log category is `LogProtokitePlaytest`.
  - Files the plugin saves move from `Saved/FlockPlaytest/` to `Saved/ProtokitePlaytest/`: the device id, the
    player's consent answer, recordings waiting to upload and feedback forms waiting to send. **Nothing is
    migrated** -- a machine that had answered the consent question is asked again, and recordings an earlier
    launch left behind are no longer found. Copy the folder across to keep them.
  - Automation tests are named `Protokite.Playtest.*`, so a run that filtered on `Flock.` now needs
    `Automation RunTests Flock.+Protokite.`.

- **The two things a game reports are no longer one drawer with one prefix.** Analytics answers what players
  did and is read on the Game Metrics dashboards; log events answer what went wrong and are read under
  Diagnostics. Both had lived under a single `Flock|Analytics` Blueprint category, and the diagnostics calls
  were named for the other surface, so the node closest to hand for "record a level completion" was the one
  that files it where nobody building a retention chart will look.
  - **`Flock Log Diagnostic Event`, `Flock Log Diagnostic Error` and `Flock Log Diagnostic Exception`**
    (`LogDiagnosticEvent`, `LogDiagnosticError`, `LogDiagnosticException`) replace `Flock Log Event`,
    `Flock Log Error` and `Flock Log Exception` (`LogAnalyticsEvent`, `LogAnalyticsError`,
    `LogAnalyticsException`). **The old names still work**, forward to the new ones and are marked deprecated,
    so existing graphs and code keep running and the editor names their replacement.
  - **The diagnostics calls, the metadata builders and the exception-capture coverage read now sit under
    *Flock | Diagnostics***; *Flock | Analytics* keeps Track Event, Record Screen View, the sessions, consent
    and flush. A test fails a call whose Blueprint category disagrees with the surface its doc comment
    declares, so the split cannot quietly close again.
- **Event properties have nodes of their own**: **Make Flock Event Properties** and **Flock Event Property
  (Integer / Float / String / Boolean / String Array)**. They build the same struct the game-commands
  *Set Command …* nodes build and are pinned against the C++ setters, so nothing can drift — a graph
  recording gameplay simply no longer has to reach for a node named for a different surface to say what
  happened. Which container each call takes is now a row in the comparison table on both guide pages:
  properties keep their type and can be charted, a diagnostic entry's Extra Data is a map of strings.

## [1.20.0] - 2026-09-21

### Added

- **A playtest build asks its player what it may collect, and collects nothing until they answer.** The question is
  drawn over the game once this build's playtest is loaded, and offers four answers: the screen and play data, the
  screen only, play data only, or nothing at all. It is the playtest's own question and its words say so -- a game's
  own privacy or analytics choices are asked separately, and neither answer moves the other. The answer is kept on the
  player's machine and used by every later launch.
- **Choosing nothing leaves the build behaving exactly as one with Enable Playtesting off**: nothing is recorded,
  nothing is sent, and no Protokite session is started for the launch.
- **Each half of the answer is honoured feature by feature.** Video recording follows the screen half, and the
  performance windows, level loads, the game's own playtest events and the exceptions a playtest asks for follow the
  play-data half. A feature name this build does not know needs the answer that allows everything.
- **The session start carries two extra parameters**, in the `extra_debug` it already sends: `playtest_consent`, the
  answer the launch collected under, and `playtest_consent_asked`, whether the player was asked at all. A session with
  no recording then reads as a player who asked for none rather than a build that went wrong.
- **Blueprint nodes for it**: Flock Get Playtest Consent, Flock Get Players Consent Answer, Flock Describe Playtest
  Consent, Flock Set Playtest Consent (for a game that asks in its own screens), Flock Ask For Playtest Consent (a
  "change what this playtest collects" entry), and Flock Is Consent Question Open.
- **Two new statuses**, Waiting For Player Consent and Player Refused Playtest, each with the sentence that explains it.
- **Ask The Player For Playtest Consent** (Project Settings > Plugins > Flock Playtest Settings), on by default. Turn
  it off where players are asked another way, for an internal test, or for an automated run with nobody there to
  answer; the build then collects what the playtest turns on and says so in what each session sends. An answer a player
  has already given is still honoured in such a build.
- **`FlockPlaytest.AnswerConsent <video_and_play_data|video_only|play_data_only|nothing|not_answered>`**, a
  Development-build console command, for a run with nobody at the keyboard.
- **A setup finding when Play starts** naming the playtest's own consent question, beside the existing one for the
  Flock SDK's analytics consent -- each saying whose consent it is, since a build can wait on both.

### Changed

- **A recording is deleted, not uploaded, when the player takes the screen back after it started.** Keeping it would
  send it a launch later, since the session it belongs to is saved beside it.
- **Nothing an earlier launch left goes out while this launch's question is still on screen**, and none of it is pushed
  while the player's answer is nothing -- it waits instead, so a player who changes their mind has it sent in the same
  launch. Feedback forms kept from an earlier launch are still sent whatever the answer: a form is something the player
  filled in and sent themselves.
- **An answer that could not be saved takes the older answer with it**, so the next launch asks again rather than
  collecting under an answer the player has replaced. Forgetting an answer removes its temporary files too.
- **The playtest self-test skips, rather than fails, when nobody has answered the consent question**, naming the
  console command that answers it.

## [1.19.0] - 2026-09-18

### Added

- **Blueprint nodes for the whole playtest.** Its status, and the sentence that explains it; whether a feature is on,
  with a node for each feature name so none has to be typed; the launch's session; sending the recording; the built-in
  feedback form; and **a feedback form of your own**, with nodes that read the playtest's questions, record answers,
  check them the way the server will, and send them. Every node is safe with playtesting off.
- **Setup findings when Play starts.** An empty or unusable Protokite API URL, the Flock SDK's analytics turned off, a
  Game Version that is not a playtest's, and the settings a playtest session waits on are each listed in the Play
  message log with a link to the page that fixes it. Nothing is said while Enable Playtesting is off.
- **A warning while building a Shipping game** that has Enable Playtesting on or carries a playtest's Game Version.
  The build still succeeds. It is given by a build that reads the plugin's build rules, such as a clean build; an
  unchanged rebuild skips them and says nothing.
- **A warning when a playtest asks for exceptions** and the Flock SDK's Analytics Capture Exceptions is off.
- **`Flock.RaiseTestException [error|blueprint] [times]`**, a Development-build console command that raises a fault the
  exception capture reports, to check exceptions reach the dashboard.
- **A playtest setup guide**, from installing the plugin to a session on Protokite's Sessions page.
- **The Qwacks icon on the feedback form**, beside its title. A packaged game carries the image with it.
- **`FlockPlaytest.SelfTest [closed playtest's Game Version ID]`**, a Development-build console command that checks a
  playtest build against the real Protokite and logs a line per step and a count. Each check sits beside a request
  Protokite must refuse, and a refusal passes only with its own HTTP status and, for a form, the question it names: the
  config (and a wrong key, a missing key, a version with no playtest), the session (and a start naming no player, and a
  closed playtest when one is named), an exception reported once with its repeat counted, a playtest event (and the
  plugin's own event name refused), the feedback form (and a missing answer, an option not on the list, an unknown
  session), the recording's upload (and a link for an unknown session) and the session's end (and an end for an unknown
  session). It ends the launch's session as its last step, and ends at once any session a refusal should have prevented.
  On a build with no video encoder, which is every platform but 64-bit Windows, the recording step is skipped, saying why.
- **On Recording Upload Finished**, a Blueprint event raised once when the launch's recording finishes: uploaded, or not
  uploaded with the reason, including when the upload could not begin. It is not raised while the game is closing.
- **`EndPlaytestSession` takes an optional completion** in C++, which hears whether Protokite took the end and why not.

### Changed

- **Sending your own form's answers says what happened.** It returns whether the answers were sent or kept, and says
  why in the log when they were neither: no published form, or nobody to attribute them to. It used to return
  nothing, and said nothing when the playtest had no form.

### Fixed

- **A crash is reported once.** The engine raises its crash events up to three times for one crash, and each became
  its own "Unhandled system error" beside the error that caused it. A crash is now one report, named by the error the
  engine recorded, and a Fatal log line counts as that report.
- **A picked option is checked letter for letter**, as the server checks it. "crash" for the option "Crash" passed
  the form's own check and was then refused by the server, which dropped the answers.
- **A feedback form with two list questions** no longer risks its first list pointing at memory the second one moved.
- The Flock panel's **Open Settings** opens the Flock SDK's own settings page.
- A form whose question ids differ only in letter case is named in the log when it loads, because their answers
  cannot be kept apart: rename one of them.
- **The playtest plugin compiles when a unity build puts its form files together.** Two of them defined the same
  helpers, which compiled only while the build kept them apart.
- **The guide says a game that stops its recording gets it uploaded**, as every finished playtest recording is. It
  said the recording was kept.
- **Offline snapshots keep names exactly as declared.** The cache wrote every member name, at every depth, with its first
  letter lower-cased. Snapshots written before this still load.
- **The SDK's automation tests leave the project's saved files alone.** A test's Flock SDK kept its files in the
  project's own `Saved/Flock` folder, like any launch of the game. A test run that was cut short left a test player's
  session there, and the next launch of the game sent it to the Flock server, which refused it. A test run also took
  over the analytics that ended launches of the game had kept, and deleted the saved sign-in, the consent decision and
  other versions' offline data. Each test now keeps every file in a folder of its own, deleted when the test ends.

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
