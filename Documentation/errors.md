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

Each segment is dropped when there is nothing to say. A failure that never reached the server has no
status, no code and no hint, so it composes to just its message — `Request timeout`.

The **raw response body is deliberately not in there**. It is unbounded payload, and it is what makes
an error-tracker group every failure into its own bucket. Read `Error.Body` when you want it, or
`Error.ToString()`, which is the same log-friendly text plus the body.

## What is on the error

| Field | What it holds |
|-------|---------------|
| `Type` | `EFlockErrorType` — Auth, Validation, Network, Connection, Timeout, Serialization, Cancelled |
| `Message` | Terse, stable text (`Validation failed (HTTP 422)`). Kept short on purpose so error trackers bucket by kind |
| `ServerMessage` | The server's own wording of the failure, when the body carried one |
| `Code` / `ErrorCode` | The backend's `detail.code` string, raw and as a typed `EFlockErrorCode` |
| `Hint` | The SDK's next step for this code — the `Fix:` line above |
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
is still on `Error.Code`, so a newer backend code is never lost, only untyped.

### Groups, not single codes

`Is Already Registered` (`UFlockErrorLibrary::IsAlreadyRegistered`) groups the five identity codes
where a register call reports the identity already exists, because that is one caller concept with one
remedy. A taken **display name** is deliberately outside the group — different fix. There is no
predicate wrapping a single code; compare `ErrorCode` directly for those.

## Hints

`Hint` is filled from a table keyed on `EFlockErrorCode`, never on message text, and it is stamped the
moment the error is built — so any failure with a recognised code carries its remedy no matter which
layer produced it.

One code is genuinely ambiguous and gets refined by the credential that hit it:
`player.invalid_login_credentials` means *"this device has no account, register it"* for a device login
and *"wrong password"* for email. The HTTP layer cannot tell which, so the auth provider refines the
hint on the way out.

Hints name real calls only. Facebook and Discord have no registration route, so their hint points at
`Flock Link Facebook` / `Flock Link Discord` and never at a register node that does not exist.

## Validation failures name the field

The backend reports two different `detail` shapes, and both are parsed:

- The game routes send a coded object — `{"detail":{"code":"...","message":"..."}}` — which fills
  `Code` and `ServerMessage`.
- The request-validation layer sends an **array of field errors** instead, which carries no code at
  all. Those render into `ServerMessage` as the field path and the reason:

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
- **`EFlockErrorType::Auth` after a refresh** — the session is gone. Send the player back to sign-in.
- **Money mutations** (a purchase, a consume, a funds grant) — an ambiguous failure is surfaced rather
  than re-sent, because a timed-out request may already have committed. Re-send only on a deliberate
  player action, never automatically.

## Where failures show up in the log

Every failing call is logged as a warning by the HTTP layer with the server's error document (first 512
characters), and the provider logs the operation that failed. Neither needs debug logs turned on — see
[Logging & debugging](logging.md).
