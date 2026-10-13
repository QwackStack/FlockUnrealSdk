# Multiplayer

Flock's multiplayer is player-hosted: players group up in **parties**, find each other through **matchmaking** or a
join code, and one of them hosts the game session. This guide covers what ships today: **parties** and **sessions**.
Matchmaking, and connecting the players' games to each other, follow in later releases.

Everything here goes through `FFlockMultiplayerProvider`, from `UFlockSubsystem::GetMultiplayerProvider()`. Make its
calls from the game thread. Every party and session call needs a signed-in player and fails with `Auth` when there is
none, without a request.

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
already taken still answers `Cancelled`, with the player now in the party: `GetMyParty` returns it.

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
