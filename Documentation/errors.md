# Errors

Every SDK call answers with a `TFlockResult<T>` rather than throwing — Unreal builds with exceptions
off. On failure the result carries an `FFlockError`, and that error is written to be **read by a
developer at 2am**: it names the call that failed, the server's own reason, the machine-readable code,
and the next step to take.

## The one line you want

`To String (Flock Error)` in Blueprint, `FFlockError::ToDisplayText()` in C++, composes all four:

```
Device login failed: Invalid credentials [player.invalid_login_credentials, HTTP 401]
Fix: This device is not registered yet. Call Flock Register With Device once to create the account, then Flock Login With Device on later launches.
```

Every failure says what is wrong and how to fix it. A segment is dropped only when there is nothing to
say: a failure that never reached the server has no status and no code, so it reads as its cause and
its fix alone:

```
The server did not answer in time
Fix: Check the device's network connection, then try again; the SDK retries a read by itself. A purchase or other change that timed out may still have been made, so read the result before sending it again.
```

The **raw response body is deliberately not in there**. It is unbounded payload, and it is what makes
an error-tracker group every failure into its own bucket. Read `Error.Body` when you want it, or
`Error.ToString()`, which is the same log-friendly text plus the body.

## What is on the error

| Field | What it holds |
|-------|---------------|
| `Type` | `EFlockErrorType` — Auth, Validation, Network, Connection, Timeout, Serialization, Cancelled |
| `Message` | What is wrong, in one sentence: the server's own reason when it gave one, otherwise the SDK's (`The server found nothing at this address`). Never the fix and never the status |
| `ServerMessage` | The server's own wording of the failure, when the body carried one |
| `Code` / `ErrorCode` | The backend's `detail.code` string, raw and as a typed `EFlockErrorCode` |
| `Hint` | How to fix it — the `Fix:` line above. Never empty |
| `Operation` | Short label of the call that failed (`Device login`, `Purchase shop item`) |
| `StatusCode` | HTTP status when the failure came from a response; `0` for transport and client-side failures |
| `Body` | The raw response body |
| `bHasRetryAfter` / `RetryAfterSeconds` | The server's Retry-After hint, when it sent one |

## Branch on the code, never on the text

`ErrorCode` is the typed view of the backend's coded-error contract. Message wording is the server's to
change; the code is a contract.

```cpp
Shop->Purchase(ItemId, [](TFlockResult<FFlockPurchaseResult> Result)
{
    if (Result.bSuccess)
    {
        return;
    }
    if (Result.Error.ErrorCode == EFlockErrorCode::ShopInsufficientFunds)
    {
        ShowTopUpScreen();
        return;
    }
    UE_LOG(LogTemp, Warning, TEXT("%s"), *Result.Error.ToDisplayText());
});
```

In Blueprint the same branch is `Error Code` off the broken-out struct, compared against the
`EFlockErrorCode` literal — no string typing involved.

`EFlockErrorCode::Unknown` means the body had no code, or one this SDK version predates. The raw string
is still on `Error.Code`, so a newer backend code is never lost, only untyped, and the fix names it.

### Groups, not single codes

`Is Already Registered` (`UFlockErrorLibrary::IsAlreadyRegistered`) groups the five identity codes
where a register call reports the identity already exists, because that is one caller concept with one
remedy. A taken **display name** is deliberately outside the group — different fix. There is no
predicate wrapping a single code; compare `ErrorCode` directly for those.

## Hints

`Hint` is filled from a table keyed on `EFlockErrorCode`, never on message text, and it is stamped the
moment the error is built — so any failure with a recognised code carries its remedy no matter which
layer produced it. When the code has no advice of its own, the fix is the one for the kind of failure:

| Failure | Fix |
|---------|-----|
| No answer in time | Check the network; a change that timed out may still have been made, so read before re-sending |
| No connection | Check the device is online and that API URL is the server's address alone |
| An answer with no Flock code | Something other than Flock's game API answered (a proxy, a captive portal, another server): check API URL |
| An unreadable success | The same: a captive portal or another server answered |
| A server failure (5xx) | Not caused by the call: try again later |
| A code newer than this SDK | The server's reason is all there is; the fix names the code |
| A failure the SDK finds itself | Its own fix (an empty argument names what to pass; nobody signed in says to sign in) |

Every failure Flock's game API sends carries a code. `request.not_found` with a bare `Not Found` is the server saying no
route (with any other reason it names a record the route did not find)
answers at that address: API URL under Project Settings > Flock SDK must be the server's address alone,
such as `https://api.example.com`, with no `/v1` or other path after it.

One code is genuinely ambiguous and gets refined by the credential that hit it:
`player.invalid_login_credentials` means *"this device has no account, register it"* for a device login
and *"wrong password"* for email. The HTTP layer cannot tell which, so the auth provider refines the
hint on the way out.

Hints name real calls only. Facebook and Discord have no registration route, so their hint points at
`Flock Link Facebook` / `Flock Link Discord` and never at a register node that does not exist.

## Validation failures name the field

The backend reports two different `detail` shapes, and both are parsed:

- The game routes send a coded object — `{"detail":{"code":"...","message":"..."}}` — which fills
  `Code` and `ServerMessage`. A request the server refuses before any route runs (a member missing from
  the body) comes coded `request.validation_failed`, its message naming the field
  (`body.shop_item_id: Field required`).
- An older server's request-validation layer sends an **array of field errors** instead, which carries no
  code at all. Those render into `ServerMessage` as the field path and the reason:

```
Update player data failed: body.player_data: Input should be a valid dictionary [HTTP 422]
```

At most three fields are spelled out; the rest collapse into a `(+2 more)` tail so one bad request
cannot flood a log line.

## Which failures are worth retrying yourself

Mostly none — the SDK already retries transient failures with backoff, honours `Retry-After`, and never
retries Auth, Validation, Serialization or Cancelled. What is left for you:

- **`EFlockErrorType::Connection`** — the request never reached the server. Reads backed by the offline
  cache serve their last good copy instead; a write that cannot be queued fails here.
- **`EFlockErrorType::Auth` after a refresh** — the session is gone. Send the player back to sign-in. Check
  `StatusCode` and `ErrorCode` first: the SDK refreshes only a 401, so a 403 reaches you as it came. A 403 carrying
  `MatchmakingNotPartyLeader`, `MatchmakingPlayerNotEligible`, `MultiplayerNotHost` or `PartyNotPartyLeader` means the
  player is not allowed to do that, and signing in again changes nothing.
- **`EFlockErrorType::Cancelled`** — you cancelled the call, or it was made for a player who has since signed out
  or been replaced by another sign-in, so its retry was not sent. Nothing to retry: make the call again for the
  player now signed in, if it still applies. A cancelled call is never answered from the offline cache.
- **Money mutations** (a purchase, a consume, a funds grant) — an ambiguous failure is surfaced rather
  than re-sent, because a timed-out request may already have committed. Re-send only on a deliberate
  player action, never automatically.

## Where failures show up in the log

Every failing call is logged as a warning by the HTTP layer with the server's error document (first 512
characters), and the provider logs the operation that failed with the same text `ToDisplayText()` gives,
its `Fix:` line included. Neither needs debug logs turned on — see
[Logging & debugging](logging.md).
