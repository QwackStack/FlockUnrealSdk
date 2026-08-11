# Leaderboards

A **leaderboard** ranks players by one of their player-data fields. The board declares which template and
field it projects over, how repeated writes fold together, and which direction wins.

**There is no submit call, and none is needed.** Because a board is a projection, a player moves up it by
writing the field it ranks — with a game command, exactly as any other data write. If you are looking for
`SubmitScore`, it is [`UpdatePlayerDataField`](game-commands.md).

Boards are addressed by **name** — the name you gave the board on the dashboard. The SDK resolves the id
internally and caches it, so no read ever takes one.

## Blueprint

The nodes live under *Flock | Leaderboard*:

| Node | Returns |
|---|---|
| `Flock Get Leaderboard` | the board's configuration |
| `Flock Get Leaderboard Standings` | a page of ranked entries (`Page`, `Limit`) |
| `Flock Get My Rank` | the signed-in player's placement |
| `Flock Get Standings Around Me` | the `Neighbours` entries either side of them |

Reading a board and its standings works signed out. **My Rank** and **Standings Around Me** need a
signed-in player and fail immediately when there is none.

The pure helpers are on the same menu:

| Node | Does |
|---|---|
| `Make Current Window` | the board's live window — all-time, this week, or this season |
| `Make Season Window` | one finished season, from its id |
| `Make Period Window` | a raw period key, e.g. `2026-W31` |
| `Format Score` | renders a score the way the board measures it |
| `Is Higher Better` | true when a bigger number wins |

Leave the **Window** pin unconnected for the board's live window, which is what most screens want.

### Showing a player's rank

`Flock Get My Rank` returns a **Player Rank** with a **Ranked** boolean. A player who has not scored yet
is a *successful* read with `Ranked` false — not an error. Branch on `Ranked` before showing `Rank` or
`Score`; `Format Score` already returns an empty string for an unranked player, so a text binding needs no
branch of its own.

### Formatting scores

Do not print `Score` directly. A board can measure an integer, a float, or a **duration in seconds**, and
`Format Score` is what turns `83.25` into `1:23.250` on a time-trial board. Feed it the board from
`Flock Get Leaderboard` and the score from a standings row or a player rank.

## C++

Everything lives on the leaderboard provider:

```cpp
UFlockSubsystem* Sdk = UFlockSubsystem::Get(this);
FFlockLeaderboardProvider* Boards = Sdk->GetLeaderboardProvider();

// The board's configuration — needed for FormatScore and IsHigherBetter.
Boards->GetByName(TEXT("HighScoreTest"),
    [Boards](TFlockResult<FFlockLeaderboard> Board)
    {
        if (!Board.bSuccess)
        {
            return;
        }

        // Top of the board's live window.
        Boards->GetStandings(TEXT("HighScoreTest"),
            [Board](TFlockResult<FFlockStandings> Standings)
            {
                if (!Standings.bSuccess)
                {
                    return;
                }
                // Total is the board's full entry count, not Items.Num() — page from it.
                for (const FFlockStandingEntry& Entry : Standings.Value.Items)
                {
                    const FString Text = Board.Value.FormatScore(Entry.Score);
                    UE_LOG(LogTemp, Log, TEXT("#%d %s %s"), Entry.Rank, *Entry.PlayerName, *Text);
                }
            });
    });
```

The signed-in player's own placement, and the players around them:

```cpp
Boards->GetMyRank(TEXT("HighScoreTest"),
    [](TFlockResult<FFlockPlayerRank> Rank)
    {
        if (Rank.bSuccess && !Rank.Value.Ranked)
        {
            // A valid answer: this player has no entry on the board yet.
            return;
        }
    });

Boards->GetAroundMe(TEXT("HighScoreTest"),
    [](TFlockResult<FFlockStandings> Around) { /* five either side by default */ });
```

Both take an explicit window and country on their longer overloads:

```cpp
Boards->GetStandings(TEXT("HighScoreTest"), FFlockLeaderboardWindow::Season(TEXT("s7")),
    TEXT("SA"), /*Page*/ 1, /*Limit*/ 25, [](TFlockResult<FFlockStandings> Result) {});
```

`ResolveId` exists for logging and deep links. Nothing in the read API consumes the id it returns.

## Submitting a score

Write the field the board ranks:

```cpp
// The board's source template, then the field it projects over.
Sdk->GetPlayerProvider()->GetMyDataByTemplate(TEXT("gameplay"),
    [Sdk](TFlockResult<FFlockPlayerData> Row)
    {
        if (!Row.bSuccess)
        {
            return;
        }
        Sdk->GetCommandProvider()->UpdatePlayerDataField(Row.Value.Id, TEXT("Score"),
            FFlockCommandValue(9001), [](TFlockResult<FFlockPlayerData> Written) {});
    });
```

The write key is the field name the **template declares**, not the Pascal-cased name a read gives back —
see [Game commands](game-commands.md). Whether the new score appears on the board immediately or on a
delay is the backend's business; re-read the rank to find out, and call `ClearCache()` first if you need
to be certain the read is not served from the snapshot.

## Board configuration

`FFlockLeaderboard` carries what a UI needs to render the board correctly:

| Member | Means |
|---|---|
| `ValueType` | `Integer`, `Float`, or `DurationSeconds` (scores are in seconds) |
| `Direction` | `Higher` or `Lower` — use `IsHigherBetter()` |
| `Aggregation` | `Best`, `Latest`, or `Sum` — how repeated writes fold |
| `WindowType` | `Never`, `Weekly`, or `Seasonal` — how the board buckets over time |
| `Scope` | `Global` or `Country` |

`WindowType` describes how the board buckets; it is **not** what you pass when reading. Reading takes an
`FFlockLeaderboardWindow`, and leaving it at `Current()` asks for whichever window the board is serving
now.

The field a board ranks is deliberately not exposed — the server does not send it to clients.

## Caching

Board configurations, standings, and player placements are cached to disk under the game version and
served when the network is down, so a leaderboard screen shows the last-known board rather than an error.
An authoritative failure — a board that no longer exists — still surfaces.

Placements are cached **per player**, so one player's rank is never shown to the next player on a shared
device, and the whole leaderboard cache is dropped on sign-out. `ClearCache()` drops it on demand.

A board name your game does not have fails rather than returning an empty board, because an empty
leaderboard and a typo look identical in a UI.
