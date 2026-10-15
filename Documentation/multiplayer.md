# Multiplayer

Flock's multiplayer is player-hosted: players group up in **parties**, find each other through **matchmaking** or a
join code, and one of them hosts the game session. This guide covers what ships today: **parties**, **sessions** and
**matchmaking**. Connecting the players' games to each other follows in a later release.

Everything here goes through `FFlockMultiplayerProvider`, from `UFlockSubsystem::GetMultiplayerProvider()`. Make its
calls from the game thread. Every party, session and matchmaking call needs a signed-in player and fails with `Auth` when
there is none, without a request.

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
            ShowJoinCode(Session->GetJoinCode());
            Session->PublishConnection(TEXT("direct"),
                FFlockCommandData().Set(TEXT("address"), MyAddress).Set(TEXT("port"), 7777), nullptr);
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
