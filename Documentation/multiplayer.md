# Multiplayer

Flock's multiplayer is player-hosted: players group up in **parties**, find each other through **matchmaking** or a
join code, and one of them hosts the game session. This guide covers what ships today: **parties**. Matchmaking and
sessions follow in later releases.

Everything here goes through `FFlockMultiplayerProvider`, from `UFlockSubsystem::GetMultiplayerProvider()`. Make its
calls from the game thread. Every party call needs a signed-in player and fails with `Auth` when there is none, without a
request.

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
