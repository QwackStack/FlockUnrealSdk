# Multiplayer

Flock's multiplayer is player-hosted: players group up in **parties**, find each other through **matchmaking** or a
join code, and one of them hosts the game session. This guide covers what ships today: **parties**, **sessions**,
**matchmaking**, **connecting players** directly or through a connection of the game's own, and the host **letting in only
the session's players**. Flock's relay, for players who cannot reach each other directly, follows in a later release.

Everything here goes through `FFlockMultiplayerProvider`, from `UFlockSubsystem::GetMultiplayerProvider()`. Make its
calls from the game thread. Every party, session and matchmaking call needs a signed-in player and fails with `Auth` when
there is none, without a request. From Blueprint, every call is a node under *Flock | Multiplayer*, and a ready-made game
mode lets in only the session's players: see [Blueprint](#blueprint).

## Parties

A party is a group of players who will search for a match together. Its **leader** made it, or was handed the lead;
the others joined with its **invite code**, which a player reads out or pastes to a friend. A player is in one party at
a time per game.

**A party lasts until the player leaves it, across launches.** Quitting the game does not leave it, and nothing times it
out on the server. So call `GetMyParty` after the player signs in: it hands back the party the player is still in, or
null.

```cpp
#include "FlockSubsystem.h"
#include "Multiplayer/FlockParty.h"

UFlockSubsystem* Flock = GetGameInstance()->GetSubsystem<UFlockSubsystem>();
FFlockMultiplayerProvider* Multiplayer = Flock->GetMultiplayerProvider();

// Make a party of up to 4 (the server's default when the size is left unset), with a setting of the game's own.
Multiplayer->CreateParty(4, FFlockCommandData().Set(TEXT("Mode"), TEXT("duo")),
    [this](TFlockResult<UFlockParty*> Result)
    {
        if (Result.IsSuccess())
        {
            Party = Result.Value;                  // a UPROPERTY, so the party outlives its end if you need it to
            ShowInviteCode(Party->GetInviteCode());
        }
    });

// A friend joins with the code, in any letter case.
Multiplayer->JoinParty(TypedCode, [this](TFlockResult<UFlockParty*> Result) { /* ... */ });

// After signing in: the party the player is still in, or null.
Multiplayer->GetMyParty([this](TFlockResult<UFlockParty*> Result) { /* Result.Value may be null */ });
```

| Call | Does |
|---|---|
| `CreateParty(MaxSize, Settings, OnComplete)` | Makes a party with the player as its leader and only member. Size 2 to 64; unset is 4. |
| `JoinParty(InviteCode, OnComplete)` | Joins by code. Joining the party the player is already in returns it. |
| `GetMyParty(OnComplete)` | The player's party, read now, or null. The same object each time while it lasts. |

Each call returns an `FFlockRequestHandle`; cancelling it stops the request on its way. A create or join the server had
already taken can still answer `Cancelled`, with the player now in the party: `GetMyParty` returns it. A cancel that lands
once the SDK holds the party (while it reads the player's search for a match, each call's last step) hands the party over
instead.

### The party object

`UFlockParty` is a Blueprint type; its state reads from Blueprint and C++ alike.

| Read | Gives |
|---|---|
| `GetId()`, `GetInviteCode()` | the party, and the code others join with |
| `GetLeaderPlayerId()`, `IsLeader()` | who leads, and whether it is the signed-in player |
| `GetMembers()` | the players, in the order they joined; when the leader leaves, the first of the others leads |
| `GetMaxSize()` | how many players it takes |
| `GetSettings()` | the game's own data, an `FFlockJsonData` read by dotted path (`TryGetString`, `TryGetInt`, ...) with its keys as written |
| `GetEndReason()`, `HasEnded()` | why it stopped being the player's, or `None` |

Its calls, each completing with the party as it stands afterwards:

| Call | Who | Does |
|---|---|---|
| `Leave(OnComplete)` | anyone | Leaves. If the server says the player is already out, that counts as leaving. The last one out disbands it. |
| `Kick(PlayerId, OnComplete)` | leader | Removes another player. The leader cannot remove themselves. |
| `MakeLeader(PlayerId, OnComplete)` | leader | Hands the lead to another player in the party. |
| `Update(MaxSize, Settings, OnComplete)` | leader | Changes the size, the settings or both; leave one unset to keep it. New settings replace all of the old ones, and the size cannot go below the players already in it. |
| `Disband(OnComplete)` | leader | Ends the party for everyone in it. |

A call on a party that has ended is refused (`Validation`) without a request. The server decides who may do what: a
member who does not lead gets `PartyNotPartyLeader`, a 403 that means "not allowed", never "signed out".

### Keeping it current

The SDK reads a held party again every **Party Refresh Seconds** (Project Settings > Flock > Multiplayer; 10 by
default, about 6 requests a minute per player; 0 turns it off), after each change the game makes, and on every
`GetMyParty`. Only the newest reading changes the party: a reading sent before another one landed, or before one of
the game's own changes finished, changes nothing, so the party never steps back to an older state.

| Event | Raised when |
|---|---|
| `OnMembersChanged` | someone joined, left or was removed |
| `OnLeaderChanged` | another player leads now |
| `OnEnded(Reason)` | the party stopped being the player's, once |

Each is raised with the party already up to date, and is assignable from Blueprint. The end's reasons:

| `EFlockPartyEndReason` | Means |
|---|---|
| `Left` | the player left it, with `Leave`, even if a refresh saw it first |
| `Disbanded` | the player disbanded it |
| `Removed` | the server no longer lists the player in it: the leader removed them, it was disbanded, or they left it on another device |
| `SignedOut` | the player signed out or another player signed in. The party reads as ended at once, and `OnEnded` is raised on the next frame. The player is still in it on the server, so the next sign-in's `GetMyParty` returns it again. |

When Flock shuts down, a held party ends as `SignedOut` without raising anything. A refresh never takes up another party
the player is in: only a call the game makes does.

### Lost answers and sign-ins

Create, kick, make leader and disband are never sent twice after an answer that was lost: a second try could make a
second party or be refused for a change that already happened. Join, leave, update and the reads are tried again. Every
party request is sent only for the sign-in it was made under, so a retry never goes out as the next player.

### Common refusals

| Code | Means |
|---|---|
| `PartyAlreadyInParty` | the player is in a party already; `GetMyParty` returns it |
| `PartyInvalidInviteCode` | no open party has that code |
| `PartyFull` | no free places, or a size below the players already in it |
| `PartyNotPartyLeader` | only the leader can do that |
| `PartyCannotKickLeader` | the leader cannot remove themselves |
| `PartyTargetNotAMember` | that player is not in the party |
| `PartyNotFound` | the party is gone for this player; the SDK ends it as `Removed` |

Every refusal carries a hint naming the fix; see [Errors](errors.md).

## Sessions

A session is one game the players play together. Its **host** is the player whose game the others connect to; the host
made it, or was handed hosting when the host before them left. Other players take a seat with its **join code**. A player
holds one seat at a time per game: hosting or joining another session gives the old seat up.

```cpp
#include "FlockSubsystem.h"
#include "Multiplayer/FlockMultiplayerSession.h"

FFlockMultiplayerProvider* Multiplayer = GetGameInstance()->GetSubsystem<UFlockSubsystem>()->GetMultiplayerProvider();

// Host a session of up to 4 with data of the game's own; only players on this build's Game Version can join.
Multiplayer->HostSession(4, FFlockCommandData().Set(TEXT("map"), TEXT("dust")), /*bSameVersionOnly*/ true,
    [this](TFlockResult<UFlockMultiplayerSession*> Result)
    {
        if (Result.IsSuccess())
        {
            Session = Result.Value;                // a UPROPERTY, so the session outlives its end if you need it to
            ShowJoinCode(Session->GetJoinCode());  // then bring the players in: see Connecting players
        }
    });

// A friend joins with the code, in any letter case.
Multiplayer->JoinSession(TypedCode, [this](TFlockResult<UFlockMultiplayerSession*> Result) { /* ... */ });

// After a restart: the session the player is still seated in, or null.
Multiplayer->GetMySession([this](TFlockResult<UFlockMultiplayerSession*> Result) { /* Result.Value may be null */ });
```

| Call | Does |
|---|---|
| `HostSession(MaxPlayers, Data, bSameVersionOnly, OnComplete)` | Makes a session with the player as its host and only player. 2 to 16 players; unset takes the game's own player count from the dashboard (8 unless changed there). With `bSameVersionOnly` (the server's default) only players on the host's Game Version can join. |
| `JoinSession(JoinCode, OnComplete)` | Takes a seat by code. Joining the session the player is seated in returns it. |
| `GetSession(SessionId, OnComplete)` | Reads a session the player holds or once held a seat in. The held session comes back as the same object; one the player is no longer seated in comes back ended, and is not held. |
| `GetMySession(OnComplete)` | The session the player is seated in, found without its id, or null when seated nowhere. |

Each call returns an `FFlockRequestHandle`; cancelling it stops the request on its way.

### The session object

`UFlockMultiplayerSession` is a Blueprint type; its state reads from Blueprint and C++ alike.

| Read | Gives |
|---|---|
| `GetId()`, `GetJoinCode()` | the session, and the code others join with |
| `GetHostPlayerId()`, `IsHost()` | who hosts, and whether it is the signed-in player |
| `GetPlayers()` | the players seated now, longest seated first; when the host leaves, the first of the others hosts |
| `GetMaxPlayers()` | how many players it takes |
| `GetData()` | the game's own data, set when it was hosted, an `FFlockJsonData` with its keys as written |
| `HasConnection()`, `GetConnection()` | how to reach the host, as the host published it: a `Mode` and its `Values` |
| `GetEndReason()`, `HasEnded()` | why it stopped being the player's, or empty while it lasts |

Its calls, each completing with the session as it stands afterwards:

| Call | Who | Does |
|---|---|---|
| `Leave(OnComplete)` | anyone | Gives the seat up. If the server says the player is already out, that counts as leaving. When the host leaves, the longest seated of the others hosts; the last one out closes the session. |
| `End(OnComplete)` | host | Ends the session for every player. |
| `MakeHost(PlayerId, OnComplete)` | host | Hands hosting to another seated player, who then publishes their own connection. |
| `PublishConnection(Mode, Values, OnComplete)` | host | Says how to reach the host: a mode (32 characters at most) and the values it needs, under 4 KB in all. Replaces what was published before. |
| `RequestJoinToken(OnComplete)` | anyone | A token proving the player holds a seat, to send the host when connecting; good for 60 s. |
| `VerifyJoinToken(Token, OnComplete)` | host | Checks a token a connecting player sent and answers that player's id; refused with `MultiplayerInvalidJoinToken` when it is not good for this session. |

A call on a session that has ended is refused (`Validation`) without a request. The server decides who may do what: a
player who does not host gets `MultiplayerNotHost`, a 403 that means "not allowed", never "signed out".

Bringing the players' games together adds `TravelToSession`, `PublishDirectConnection`, `WaitForConnection` and
`FindDirectAddress`: see [Connecting players](#connecting-players).

### Keeping the seat, and keeping it current

While the player holds a session, the SDK keeps the seat with a **heartbeat** at the interval the server gives for the
game's timeouts (every 20 s at the defaults). Every session route answers the whole session, so each heartbeat's answer,
and each call's, brings the session up to date: the game needs no reads of its own to see players come and go. Only the
newest reading changes the session; one sent before another landed, or before one of the game's own changes finished,
changes nothing.

The server gives a seat up when it stops hearing from the player (90 s by default; 60 s for the host, after which hosting
moves on). Heartbeats run from the core ticker, which keeps ticking while a desktop game is minimized or covered; a game
that is paused past the timeouts loses its seat, and the first heartbeat back ends the session as `dropped`. A heartbeat
the server refuses for another reason stops with one warning; the next `GetSession` or `GetMySession` starts them again.

| Event | Raised when |
|---|---|
| `OnPlayersChanged` | a player took or gave up a seat |
| `OnHostChanged` | another player hosts now |
| `OnConnectionChanged` | the host published how to reach it, or published again, or hosting moved and the old address was cleared |
| `OnEnded(Reason)` | the session stopped being the player's, once |

Each is raised with the session already up to date, and is assignable from Blueprint. **When hosting moves, the old host's
address is cleared** until the new host publishes theirs, so `HasConnection()` is false meanwhile.

The end's reasons are strings, because the server can add more; compare with `FlockMultiplayerSessionEndReasons` and keep
a default:

| Reason | Means |
|---|---|
| `ended_by_host` | the host ended it |
| `host_left` | the host left and nobody was left to take over |
| `expired` | it reached the game's longest session age |
| `empty` | every player left |
| `left` | the player left it, with `Leave` or from another device; the game's own leave is `left` even when it closed the session |
| `dropped` | the server stopped hearing from the player and gave the seat up |
| `moved_to_another_session` | the player hosted or joined another session |
| `signed_out` | the player signed out or another player signed in. The session reads as ended at once, and `OnEnded` is raised on the next frame. |

A call that finds the player out of the session (`MultiplayerSessionNotFound`, `MultiplayerNotAParticipant`) reads the
session once to learn why, ends it with that reason, then fails with its own code.

### Quitting

When the game quits (its game instance shuts down, Play In Editor stopping included), the SDK gives the seat up on the way
out without waiting for the answer, so the others see the player go at once rather than when the server stops hearing from
them. The session ends as `left`, raising nothing. A game that shuts Flock down itself (`ShutdownSdk`) keeps the seat: the
session ends here as `signed_out` without raising anything, and after initializing again, within the game's timeouts,
`GetMySession` finds it.

### Lost answers and sign-ins

Host, end and make host are never sent twice after an answer that was lost: a second host would make a second session, and
a second end or hand-over would be refused. Join, leave, publish, the reads, heartbeats and the token calls are tried again.
Every session request is sent only for the sign-in it was made under, so a retry never goes out as the next player.

### Common refusals

| Code | Means |
|---|---|
| `MultiplayerInvalidJoinCode` | no open session has that code |
| `MultiplayerSessionFull` | no free seats |
| `MultiplayerVersionMismatch` | the session keeps to another Game Version |
| `MultiplayerNotHost` | only the host can do that |
| `MultiplayerTargetNotAParticipant` | that player holds no seat in it |
| `MultiplayerInvalidJoinToken` | a wrong, expired or other session's join token |
| `MultiplayerSessionNotFound` | the session is over, or the player holds no seat in it; the SDK ends it with the reason a reading gives |

## Matchmaking

Each mode of your game is a **queue** with a smallest and largest number of players, made in the Flock dashboard. A
player, or a whole party, searches in a queue by its name (letter for letter, case included); once the queue holds enough
players, Flock makes a match and seats the players in a session. A party is matched as one group and never split. Players
are matched in the order they search, and the longest waiting hosts. A search that ends without a match is an outcome,
not a failure.

```cpp
#include "FlockSubsystem.h"
#include "Multiplayer/FlockParty.h"

FFlockMultiplayerProvider* Multiplayer = GetGameInstance()->GetSubsystem<UFlockSubsystem>()->GetMultiplayerProvider();

Searching = Multiplayer->FindMatch(TEXT("coop-4"), FFlockMatchmakingOptions(),
    [this](TFlockResult<FFlockMatchmakingResult> Result)
    {
        if (!Result.IsSuccess())
        {
            // Cancelled when the game cancelled the handle (the search is cancelled on the server too);
            // MatchmakingQueueNotFound when no queue has that name.
            ShowLobby();
            return;
        }
        if (Result.Value.IsMatched() && Result.Value.Session != nullptr)
        {
            Play(Result.Value.Session);            // held like any other session: heartbeats, events, Leave
        }
        else if (Result.Value.Outcome == FlockMatchmakingOutcomes::Expired)
        {
            ShowMessage(TEXT("Nobody found in 5 minutes. Search again?"));
        }
        else
        {
            ShowMessage(TEXT("The search stopped."));
        }
    });

// The player gives up: the search is cancelled on the server, and the call completes as Cancelled.
Searching.Cancel();
```

Search as a party with the party's own `FindMatch`. The leader's game starts the search; another member's game hears it
through the party, so handle the result in one place. `OnSearchEnded` is raised on every member's game, the leader's
included, and before the `FindMatch` call waiting for that search completes.

```cpp
Party->OnSearchStarted.AddDynamic(this, &AMyLobby::ShowSearching);
Party->OnSearchEnded.AddDynamic(this, &AMyLobby::HandleSearchEnded);   // void HandleSearchEnded(const FFlockMatchmakingResult& Result)

if (Party->IsLeader())
{
    Party->FindMatch(TEXT("coop-4"), FFlockMatchmakingOptions(), nullptr);
}
```

| Call | Does |
|---|---|
| `FindMatch(QueueName, Options, OnComplete)` on the provider | Searches alone. A search of the player's own left from an earlier launch is cancelled first and this one takes its place. |
| `FindMatch(QueueName, Options, OnComplete)` on a party | The leader's game searches as the party. Another member's game waits for the leader's search (it may pass an empty name) and completes with the same match; it makes no search of its own. The wait ends (`Validation`) if the party ends first, or if the player comes to lead it: the new leader then searches with a queue name. |
| `IsSearching()` on a party | True while the party searches, as this game knows: its own `FindMatch`, or the leader's search seen by the party's refresh. |

Each `FindMatch` returns an `FFlockRequestHandle`. Cancelling it cancels the search on the server (a party's, for every
member) and completes the call as `Cancelled`; a match that landed while the cancel was on its way gives its seat up
first. `FFlockMatchmakingOptions::Attributes` passes your own data with a search, keys as written; matching does not use
it yet.

| Event on the party | Raised when |
|---|---|
| `OnSearchStarted` | the party started searching: the leader's game from its call, another member's from the party's refresh (**Party Refresh Seconds**) or its own `FindMatch` |
| `OnSearchEnded(Result)` | the party's search ended, with the same result the call gets |

A member's game learns of the leader's search from the party's refresh, so with Party Refresh Seconds at 0 it hears of it
only through its own `FindMatch`. A search from before the game first held the party is not told; the next one is.

### How a search ends

The SDK checks a search every 3 seconds (2.25 to 3.75, so players do not ask in step). `FFlockMatchmakingResult` holds how
it ended:

| `Outcome` | When |
|---|---|
| `FlockMatchmakingOutcomes::Matched` | `Session` is where the players play, held like any other session; `PlayerIds` lists them, the longest waiting first. `Session` is null when reading it failed (`GetMySession` finds it) or when "Create a session when a match forms" is off in the dashboard, which logs one warning a match; your game then makes the session itself. |
| `FlockMatchmakingOutcomes::Expired` | nobody was found within 5 minutes |
| `FlockMatchmakingOutcomes::PartyChanged` | a player joined or left the party during the search, so the server stopped it; the leader searches again |
| `FlockMatchmakingOutcomes::Cancelled` | another game stopped it: a party member's, or the player's own on another device |

The outcomes are strings, because the server can add more: compare with `IsMatched()` or the constants, and treat any
other as ended without a match. A party the player has left raises nothing more, though a call waiting on its search still
completes with how it ended.

### One search at a time

A player searches once at a time per game, in any queue. A second `FindMatch` while one runs here is refused
(`Validation`) without a request, and so is a second call waiting for the same party search. A search alone while the
player's party searches is refused `MatchmakingAlreadyQueued` and leaves the party's search alone: one member's cancel
would stop it for everyone.

### Sign-ins, quitting and shutting down

Every matchmaking request is sent only for the sign-in it was made under. When the player signs out or another player
signs in, the search ends as `Cancelled` on the next frame and nothing more is sent for it; it stays on the server until it
is matched or expires.

When the game quits (its game instance shuts down, Play In Editor stopping included), the search is cancelled on the way out
without waiting for the answer, so nobody who has gone is matched, and the call completes as `Cancelled`. A search still being
made is cancelled once its ticket lands, and a match already landing gives its seat up, while Flock waits for the requests on
their way. A game that shuts
Flock down itself (`ShutdownSdk`) ends the search here as `Cancelled` with no event and sends nothing; the next `FindMatch`
after initializing again replaces it.

### Common refusals

| Code | Means |
|---|---|
| `MatchmakingQueueNotFound` | no queue by that name in this game. The list of queues is read once a launch; a queue deleted and made again under the same name is found by its new id |
| `MatchmakingAlreadyQueued` | a player in the search already searches: the party's search, or a member searching alone |
| `MatchmakingPartyTooLarge` | the party has more players than the queue takes |
| `MatchmakingNotPartyLeader` | only the leader's game can queue the party; leadership moved before the search went out |
| `MatchmakingPlayerNotEligible` | a player is outside the queue's allowed segments |
| `MatchmakingVersionMismatch` | on a "Same game version only" queue, a member's game is on another Game Version, or has not read the party yet: each member's version comes from their game's last party read, so have every member's game call `GetMyParty` once it is running |

Every refusal carries a hint naming the fix; see [Errors](errors.md).

## Connecting players

Once the players hold a session (hosted, joined by code or matched), their games connect to the host's. There are two ways:

- **Direct**, over the engine's own networking: `TravelToSession` does it all. The host opens its level listening for
  players and publishes how to reach it; every other player waits for that and travels to the host with a Flock join token,
  which the host checks before letting the player in (its game mode calls Flock's check: see
  [Letting in only the session's players](#letting-in-only-the-sessions-players)).
- **A connection of the game's own** (Steam, EOS, anything else): the host publishes it with `PublishConnection(Mode,
  Values)`, and the players read it back from the session's `GetConnection()`.

```cpp
#include "FlockSubsystem.h"
#include "Multiplayer/FlockMultiplayerSession.h"

// Every player's game, host included, once it holds the session (a UPROPERTY on the game instance here).
FFlockTravelToSessionOptions Options;
Options.HostLevel = TEXT("/Game/Maps/Arena");   // the level the host opens; players travel to whatever the host has open
const TWeakObjectPtr<UMyGameInstance> Game = this;
Session->TravelToSession(this, Options, [Game](TFlockResult<FFlockTravelToSessionResult> Result)
{
    // Runs in the level the game is in now: the level the call was made from is gone.
    if (!Result.IsSuccess() || !Game.IsValid())
    {
        return;                                  // a mistake, a publish the server refused, or a cancel
    }
    const FString& Outcome = Result.Value.Outcome;
    if (Outcome == FlockTravelToSessionOutcomes::StartedAsHost || Outcome == FlockTravelToSessionOutcomes::Connected)
    {
        Game->ShowMatchStarting();
    }
    else
    {
        Game->ShowCouldNotJoin(Outcome);         // compare with FlockTravelToSessionOutcomes and keep a default
    }
});
```

Pass anything in the game's world as the first argument, such as an actor or the game instance. The completion runs once
the game is in the level it stays in, so capture what outlives a level change (the game instance, weak pointers), never an
actor of the level the call was made from.

### What Travel To Session does

| Who | Does |
|---|---|
| host | Opens `HostLevel` (empty: the level it is in, again) with the engine's `?listen`. Once the level listens, publishes a direct connection on the port its net driver bound: when the port asked for is taken (another game on the same machine), the engine takes the next free one, and that is the one published. Completes `started_as_host`. |
| player | Waits for the host to publish (`WaitForHostSeconds`, 60 by default), reading the session every 2 s. Asks Flock for a join token (it lasts 60 s, so only once the host is known) and travels to the host's LAN address when this device shares the host's public address, and to the published address otherwise, with the token in the travel URL (`?FlockJoinToken=`). Completes `connected` once the host has let the player in: on a LAN, a few seconds after the host published (the next read of the session, then about two seconds of the engine's own connecting), and `refused` when the host turned the player away. |

How it ended is the result's `Outcome`, a string because more can come; compare with `FlockTravelToSessionOutcomes`:

| Outcome | Means |
|---|---|
| `started_as_host` | this game hosts: its level listens and how to reach it is published |
| `connected` | this game is in the host's level, let in by the host |
| `host_never_published` | the host did not publish within `WaitForHostSeconds`; this game traveled nowhere |
| `session_ended` | the session ended while this game waited |
| `could_not_connect` | connecting failed, and the engine has taken this game back to its default level; or the host published no address and port this game can use. Logged as a warning naming the address. Outside a LAN the host's port must be forwarded to it |
| `could_not_start` | the host's level did not open listening for players on an IP address, so nothing was published |
| `not_direct` | the host published a connection of the game's own; the result's `Connection` holds it |
| `refused` | the host turned this player away, and the engine has taken this game back to its default level; the result's `RefusedReason` says why (see below). Logged as a warning naming the fix |

It fails, rather than ending with an outcome, only for a mistake (no world, an ended session, this game already connected,
hosting or opening a level, a travel already under way), for a publish the server refused (the host stops listening first:
nobody can reach a host that did not say where it is), for a join token Flock would not give (nothing travels without one;
the failure is Flock's, with its fix), and for a cancel. **Cancelling the handle** gives up a player's join token still
being asked for or connection still being made (the game stays in its level) and stops a host listening, at once or as soon
as its level has opened.

A connection the engine cannot make is the engine's to give up: it waits out its own connection timeout (about 20 s for an
address nobody answers), then loads the game's default level. Travel To Session connects over the engine's IP networking:
a game whose net driver uses Steam's sockets (OnlineSubsystemSteam's `bUseSteamNetworking`, on unless set to false) cannot
listen on an IP address, and ends `could_not_start`; such a game publishes a Steam connection of its own instead.

### Letting in only the session's players

A host that travels to its session with `TravelToSession` lets in only players whose Flock join token checks out, one
connection a player, and knows which Flock player each connection is. The engine asks the game mode about each joining
player in `PreLoginAsync`, so the host's game mode calls Flock's check there, in place of the engine's own:

```cpp
// MyGameMode.h
virtual void PreLoginAsync(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId,
    const FOnPreLoginCompleteDelegate& OnComplete) override;

// MyGameMode.cpp
#include "Multiplayer/FlockJoinChecks.h"

void AMyGameMode::PreLoginAsync(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId,
    const FOnPreLoginCompleteDelegate& OnComplete)
{
    // Flock's check, then the engine's own (PreLogin, such as a full game), then the game's own below.
    const TWeakObjectPtr<AMyGameMode> Mode = this;
    FlockJoinChecks::CheckJoiningPlayer(this, Options, Address, UniqueId, OnComplete,
        [Mode](const FFlockJoiningPlayer& Player, FString& OutRefusedReason)
        {
            if (Mode.IsValid() && Mode->IsBanned(Player.PlayerId))
            {
                OutRefusedReason = TEXT("banned");   // the joining player's RefusedReason
                return false;
            }
            return true;
        });
}
```

Or base the game mode on **`AFlockGameModeBase`** (on `AGameModeBase`) or **`AFlockGameMode`** (on `AGameMode`, for
match states), as a Blueprint or in C++: their `PreLoginAsync` does exactly the above, and the game's own check is their
**Approve Joining Player** (`ApproveJoiningPlayer`), overridable in Blueprint, which lets everyone in until overridden.

The game's own check is optional; it runs only for a player Flock has let in, and gets `FFlockJoiningPlayer`: the
`PlayerId` Flock verified, the travel URL's `Options` without Flock's token, the `Address` and the `UniqueId` the engine
gave. A refusal with no reason is told as `refused_by_the_game`. `OnComplete` is always called, and the game thread never
waits: the answer comes once Flock has answered.

Who a connection is, on the host: `Session->PlayerForConnection(PlayerController)`, such as in `PostLogin`. It answers the
Flock player Flock verified for a joined player, this device's own player for its own controller, and an empty string for a
controller whose connection is gone or was not let in by Flock's check, and while the game does not host that session
through `TravelToSession`. Name players by it, never by what a player sends.

A joining player is turned away for each of these reasons, its `RefusedReason`:

| Reason | Means |
|---|---|
| `no_join_token` | the player's travel URL carried no token: it did not come through `TravelToSession`. Flock is not asked |
| `join_token_invalid` | Flock said the token is not good for this session: made up, another session's, expired (tokens last 60 s), or its player gave up their seat. Flock gives one answer for all four |
| `already_connected` | the same Flock player is already let in through another connection. A token checks out any number of times within its 60 s, so the host itself lets one connection a player in; a connection that is gone, or that was turned away, holds no place |
| `flock_unreachable` | Flock did not answer within 8 s, or its answer could not be read, or the host was over its checks a minute (60 by default; the server answers `RequestRateLimited`). Nobody is let in unchecked; the host's log says why |
| `session_ended` | the session has ended |
| `not_host` | the device reached no longer hosts the session |
| `refused_by_the_game` | the game's own check turned the player away without a reason of its own |
| `join_check_missing` | the host's game mode does not call `CheckJoiningPlayer`, so nobody is let in. The host's log says so once, at Error |

`RefusedReason` can also be the game's own reason (`banned` above) or the engine's (such as `Server full.`); compare with
`FlockTravelToSessionRefusedReasons` and keep a default. A player turned away by a host that is not checking through Flock
(its words carry no Flock reason), or by a host closing its connections, ends `could_not_connect`.

How long a check may take: the engine never drops a player its game mode has not answered (measured: still waiting after
130 s), so Flock's check gives Flock 8 s and then turns the player away with a reason. An answer that comes after the player
left, or after the host stopped hosting, lets nobody in and is remembered nowhere; when the host's level stops listening or
Flock shuts down, every player still being checked is turned away (`flock_unreachable`) rather than left waiting. Flock shut
down while the hosted level stays open (listening, or opening) turns every joining player away (`flock_unreachable`, told once
as a warning), whatever the game mode does, until that level closes or the game opens another: nobody can be checked, so
nobody is let in. Leave the hosted level before shutting Flock down.

While the game does not host through `TravelToSession`, `CheckJoiningPlayer` is the engine's own `PreLoginAsync` and the
game's check is not run. Split-screen guests joining through a player's connection carry no token of their own, so a host
checking players turns them away (`no_join_token`). The token travels in the clear unless the game encrypts its
connections; it is bound to one player and one session for 60 s, and the host lets in one connection a player.

### The pieces

`TravelToSession` is built from three session calls a game can also use alone, with engine travel of its own:

| Call | Who | Does |
|---|---|---|
| `PublishDirectConnection(Port, OnComplete)` | host | Publishes how to reach this device on `Port`, the port its game listens on. Refused (`Validation`) before any request for a port outside 1 to 65535. |
| `WaitForConnection(TimeoutSeconds, OnComplete)` | player | Completes with the session once the host has published (`HasConnection()`), once the session has ended (`HasEnded()`), or with neither when the time runs out. Reads the session every 2 s meanwhile; a host change clears the address without ending the wait. Completes at once when the host has already published. |
| `FindDirectAddress(OnComplete)` | player | The address and port to connect to (`FFlockDirectAddress`): the host's LAN address when this device shares its public address, the published address otherwise. `Address` is empty when the host published no dotted IPv4 address and whole port from 1 to 65535 (a port sent as text, "7777", counts). |

A direct connection holds `mode` `direct` and four values; the server checks none of them, so the SDK does:

| Value | Holds |
|---|---|
| `address` | the host's public address when it is known, its LAN address otherwise |
| `port` | the port, a number |
| `lan_address` | the address the host's game sends from toward the internet; left out when unknown |
| `public_address` | the address the host is seen from on the internet; left out when unknown |

The public address comes from one request to the STUN server Flock lists (`stun.l.google.com` unless the game's backend
lists another), asked from a dual-stack socket where the platform makes one, a plain IPv4 socket otherwise. Flock's list is
read once a launch, asked with no session so a direct connection is never counted as relay use; a list that cannot be read is
not kept, and is warned about. When no STUN server answers within 2 s, the connection offers the LAN address alone, which
only players on the same network can reach, with one warning per launch.

### A connection of the game's own

```cpp
// Host: say how to reach it on Steam. The mode is the game's own word, 32 characters at most.
Session->PublishConnection(TEXT("steam"), FFlockCommandData().Set(TEXT("steam_id"), MySteamId), nullptr);

// Players: TravelToSession ends not_direct holding it; or read it whenever OnConnectionChanged says it changed.
FString HostSteamId;
if (Session->HasConnection() && Session->GetConnection().Mode.Equals(TEXT("steam"), ESearchCase::CaseSensitive)
    && Session->GetConnection().Values.TryGetString(TEXT("steam_id"), HostSteamId))
{
    ConnectThroughSteam(HostSteamId);
}
```

### Outside a LAN

Players on other networks reach a direct host only when its port is forwarded to it on the host's router. Flock's relay,
which needs no forwarding, follows in a later release.

## Blueprint

Every call above is an async node under *Flock | Multiplayer*, named as the call is, and fires exactly one of its pins. The
party and session objects' state and events are already Blueprint (a party's **On Members Changed**, a session's **On Ended**
and the rest); the nodes add the calls.

| Node | Pins |
|---|---|
| Flock Create Party, Flock Join Party | On Success (the party) · On Failure (the error) |
| Flock Get My Party | On Party · On No Party · On Failure |
| Flock Leave Party, Kick From Party, Make Party Leader, Update Party, Disband Party | On Success (the party as it stands, ended after a leave or disband) · On Failure |
| Flock Host Session, Flock Join Session, Flock Get Session | On Success (the session) · On Failure |
| Flock Get My Session | On Session · On No Session · On Failure |
| Flock Leave Session, End Session, Make Session Host, Publish Connection, Publish Direct Connection | On Success (the session) · On Failure |
| Flock Find Match, Flock Party Find Match | On Matched (the result: its Session is where the players play) · On No Match (the result: its Outcome says how the search ended) · On Failure |
| Flock Request Join Token | On Success (the token) · On Failure |
| Flock Verify Join Token | On Success (the player the token was given to) · On Failure |
| Flock Find Direct Address | On Success (the address) · On Failure |
| Flock Wait For Connection | On Connection · On Session Ended · On Timed Out · On Failure |
| Flock Travel To Session | On Joined (started as host, or connected) · On Not Joined (the result's Outcome and Refused Reason say why) · On Failure (a mistake, such as an ended session) |

Max Size and Max Players left at 0 take the server's default (a party of 4) and the game's own count from the dashboard;
Update Party's New Max Size 0 keeps the size, and its Replace Settings off keeps the settings. A node that acts on a party or
a session and is given none fires On Failure and sends nothing. On No Match and On Not Joined carry outcomes the server adds
later too, so compare the result's Outcome with the nodes below and keep a default.

**Cancelling.** Find Match, Party Find Match, Wait For Connection and Travel To Session hand out the node itself on their
**Async Task** pin; its **Cancel** stops the search on the server, the wait, or the travel. A cancel the game asks for fires
no pin; Flock shutting down while a node waits fires On Failure. The game instance keeps every node until a pin has fired, so
a search left running by a graph that called the node again still answers.

**Call Travel To Session from the Game Instance Blueprint**, where the engine's own **Network Error** and **Travel Error**
events are. Its pins fire once the level has changed, so a widget or an actor of the current level is gone by then and never
sees how the travel ended: a refused player included, since the engine takes it back to its default level. Called from inside
the level, the node warns in the log, naming the caller.

**Spellings.** Every value a result can carry is a pure node, so a graph compares rather than types it: **Flock Travel Outcome**
(Started As Host, Connected, Host Never Published, Session Ended, Could Not Connect, Could Not Start, Not Direct, Refused),
**Flock Refused Reason** (No Join Token, Join Token Invalid, Already Connected, Flock Unreachable, Session Ended, Not Host,
Refused By The Game, Join Check Missing), **Flock Matchmaking Outcome** (Matched, Expired, Party Changed, Cancelled), **Flock
Session End Reason** (Ended By Host, Host Left, Expired, Empty, Left, Dropped, Moved To Another Session, Signed Out), **Flock
Connection Mode Direct** and **Flock Direct Connection Value** (Address, Port, LAN Address, Public Address), and **Is Matched**.

**Letting in only the session's players.** Make the game's game mode a Blueprint based on **Flock Game Mode Base** (or **Flock
Game Mode**, for match states) and set it in the project's Maps & Modes. To turn players away, override **Approve Joining
Player**: it gets the Joining Player (the Player Id Flock verified, the travel's Options, the Address) after Flock's check and
the engine's own, returns whether to let the player in, and its Refused Reason reaches the player's Travel To Session (empty
reads as Refused By The Game). Name players by **Player For Connection** on the session, such as in **On Post Login**.
