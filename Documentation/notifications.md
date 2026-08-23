# Notifications

Three things share one provider, and they are independent of each other:

- **The inbox** — messages the backend has already delivered to this player. Needs no push setup at all.
- **Scheduling** — reminders your game asks the backend to deliver later. Server-side, so they fire whether
  or not the game is running.
- **Push device tokens** — registering a device so the backend can reach it outside the game.

Reach them from `UFlockSubsystem::GetNotificationProvider()`, or use the Blueprint nodes under
**Flock | Notifications**.

Every call except the two template reads requires a signed-in player and fails with `EFlockErrorType::Auth`
without one — an inbox and a device token both belong to a player.

## The inbox

```cpp
FFlockNotificationProvider* Notifications = UFlockSubsystem::Get(this)->GetNotificationProvider();

Notifications->GetNotifications([](TFlockResult<FFlockNotificationPage> Result)
{
    if (!Result.bSuccess) { return; }
    // Result.Value.Items is this page; Result.Value.Total is the player's whole inbox.
});
```

`GetNotifications(bUnreadOnly, Page, Limit, ...)` takes the filter and paging explicitly; the short overload
is everything, first page of 50.

For a bell icon, prefer `GetSummary` — it returns the unread count *and* the newest few items in one call:

```cpp
Notifications->GetSummary([](TFlockResult<FFlockNotificationSummary> Result)
{
    // Result.Value.UnreadCount, Result.Value.Items
});
```

`GetUnreadCount` is the cheapest call if you only need the badge number.

Marking read:

```cpp
Notifications->MarkRead(NotificationId, nullptr);   // one row; returns the updated row
Notifications->MarkAllRead(nullptr);                // Updated is how many actually flipped
```

Read state comes from `FFlockNotification::IsRead()` (`Is Read` in Blueprint), never from comparing
`ReadAt` yourself — it is nullable on the wire and an absent value arrives as an empty string.

`Type` and `Severity` are plain strings rather than enums, because the API declares both unconstrained; an
enum here would invent a closed set the server never promised. `Data` is an `FFlockJsonData` handle with the
sender's keys kept verbatim — read it with the JSON Data library.

**Offline.** Inbox reads are snapshot-backed, so a notifications screen shows the last-known messages when
the network is down rather than an error. Mark-read calls are the opposite: they are **never** queued
offline and fail with `Connection` instead, because a read receipt replayed an hour later marks messages the
player never saw.

**`ClearCache()` clears for one player.** It drops the signed-in player's cached inbox, counts and summary,
and nothing else. Another account on the same device keeps its own — including its pending reminders, which
matters because a scheduled notification still fires server-side and its id is the only handle on it. The
seen-watermark and the pending-schedule list are state rather than cache and are kept for the signing-out
player too; the template catalog is game-scoped and is kept for everyone. Called with nobody signed in it
does nothing at all.

### Reacting to arrivals

Two events on the hub save you diffing pages yourself:

```cpp
UFlockEvents* Events = UFlockSubsystem::Get(this)->GetEvents();
Events->OnNotificationReceived.AddDynamic(this, &AMyActor::HandleNotification);  // UFUNCTION()
Events->OnUnreadCountChanged.AddDynamic(this, &AMyActor::HandleUnreadCount);     // UFUNCTION()
```

`OnUnreadCountChanged` carries the count the **server** last reported — it fires on `GetUnreadCount`,
`GetSummary`, and `MarkAllRead` (which reports `0`, since that call has exactly one possible outcome). It
never fires from a background poll, because the SDK does not run one.

`OnNotificationReceived` fires once per notification the SDK has not surfaced before, **oldest first**. It
rides the inbox and summary reads and adds no traffic of its own.

Things worth knowing about it:

- **"Received" means first seen by a read**, not the instant the server created the row. There is no
  realtime channel; a push wakes the OS, not your `UObject`.
- **The first read for a player is silent.** A player who already has mail would otherwise get their whole
  history as a burst of events the first time your game asks.
- **It survives a sign-out.** The watermark behind it is per-player state rather than cache, so signing out
  and back in does not re-announce everything, and a shared device never applies one account's cutoff to
  another's inbox.
- A row whose `created_at` the SDK cannot parse is skipped rather than announced — with no comparable
  timestamp it would raise on every fetch, and a duplicate is worse than a miss.

## Scheduling

```cpp
Notifications->ScheduleByTemplateName(TEXT("energy_refilled"),
    FDateTime::UtcNow() + FTimespan::FromHours(4),
    FFlockCommandData().Set(TEXT("player"), TEXT("Ada")),
    { EFlockNotificationChannel::Push },
    [](TFlockResult<FFlockScheduledNotification> Result) {});
```

The first argument is the template's **name** — what you see on the dashboard — not its ID. The ID is
resolved through the by-name route and memoized for the session, so scheduling repeatedly does not
re-resolve. An unknown name fails `Validation` before anything is scheduled.

`Variables` fills the template's `{placeholders}`; keys are the template's own and are sent verbatim.
`Channels` restricts delivery. Leave either empty and it is **omitted from the request** rather than sent
blank, so the template's own defaults apply — an empty channel list would otherwise read as "deliver
nowhere".

Cancel with the id from the scheduled row:

```cpp
Notifications->CancelScheduled(Scheduled.Id, nullptr);
```

Delivery state is read from the timestamps, not the `Status` string: `IsPending()`, `IsDelivered()`,
`IsCanceled()`. A status value added server-side later cannot silently break those.

**Money-adjacent safety.** `ScheduleByTemplateName` is **not idempotent** and is never re-sent after an
ambiguous failure — a retry could leave the player with two of the same reminder. `CancelScheduled` *is*
idempotent and does retry.

`ScheduleByTemplateId` exists for a caller that already holds an id. It is named rather than overloaded,
because two `FString` overloads would be indistinguishable at a call site.

### What you scheduled

`/v1` has no route to list or read a schedule back, so the SDK keeps its own record of what it scheduled:

```cpp
for (const FFlockPendingSchedule& Entry : Notifications->GetPendingSchedules())
{
    // Entry.Id is what CancelScheduled takes; Entry.TemplateName is what you scheduled it by.
}

Notifications->CancelAllScheduled([](TFlockResult<int32> Result)
{
    // Result.Value is how many the server actually cancelled.
});
```

`GetPendingSchedules` is synchronous — there is no network call to make. What that buys, and what it costs:

- It knows only what **this install** scheduled. A reminder set on the player's other device is invisible
  here, because nothing can be asked.
- Delivery is inferred from the **clock**: once an entry's `DeliverAt` passes it stops being pending and is
  dropped as the list is read.
- The list is **per player** and survives sign-out, so a second player on a shared device neither sees nor
  can cancel the first player's reminders — and signing back in does not lose them.

`CancelAllScheduled` drops entries the server no longer recognises — delivered, already cancelled, or
unknown — instead of failing the whole batch, since they are not pending either way. A transient failure
stops the run and reports the error, leaving the remaining entries tracked so a later call can retry them.

## The template catalog

```cpp
Notifications->GetTemplates([](TFlockResult<TArray<FFlockNotificationTemplate>> Result) {});
Notifications->GetTemplateByName(TEXT("energy_refilled"), [](TFlockResult<FFlockNotificationTemplate> Result) {});
```

These two are the odd ones out in this provider: they are **game-scoped, not player-scoped**. They declare
no `Authorization` header, so they work signed out, they are cached per game version rather than per player,
they survive a logout, and they are memoized in process — a catalog does not mutate under the player the way
an inbox does.

`GetTemplateByName` takes an optional locale. A locale-specific fetch is not memoized under the bare name,
since it is a different record.

## Push device tokens

**The SDK registers a token; it does not fetch one.** Get the token from your push plugin — Firebase Cloud
Messaging's `On Token Received`, or OneSignal — and pass the string in:

```cpp
Notifications->RegisterDeviceToken(Token, [](TFlockResult<FFlockDeviceToken> Result) {});
```

The platform comes from the running build. On desktop, console and in the editor this fails `Validation`
rather than guessing one: the backend accepts `android`, `ios` and `web` only, and a token filed under the
wrong platform is *accepted* and then never delivers — a failure that surfaces weeks later with nothing in
the logs to point at.

Check first rather than letting it fail:

```cpp
EFlockDevicePlatform Platform;
if (FFlockNotificationProvider::GetCurrentDevicePlatform(Platform))
{
    // Push is available here — safe to offer an "enable notifications" toggle.
}
```

There is an explicit-platform overload for a token that did not come from the running build — a web token
from an embedded view, say. `UnregisterDeviceToken(Token, ...)` stops delivery; `Deactivated` false means
there was nothing to deactivate. Registering **is** idempotent (the row is keyed by token), so it retries
safely — the opposite of scheduling.

Re-register whenever the OS issues a new token.

### Where the token comes from, per platform

Taking the token as a string is where every comparable backend SDK draws the line — PlayFab, Nakama and
Beamable all do the same. But how you *get* one differs sharply by platform:

- **iOS — nothing extra needed, today.** Unreal surfaces the APNs token itself and Flock's backend speaks
  APNs directly, so there is no Firebase in this path at all. Bind
  `FCoreDelegates::ApplicationRegisteredForRemoteNotificationsDelegate`, hex-encode the bytes, and pass the
  string in. Worked example: [iOS today](push-setup-android.md#ios-today-no-plugin-required).
- **Android — a third-party plugin, for now.** Unreal's own `GoogleCloudMessaging` plugin does supply a
  Java registration path, but it is built on GCM and `InstanceID`, which Google decommissioned in 2019. So
  a current Firebase plugin is the practical route today.

**Both of these are being closed.** A one-call iOS wrapper and a first-party Android UPL shipping the
modern `firebase-messaging` binding are planned for the SDK, so push will not depend on a paid marketplace
plugin. The `RegisterDeviceToken` API you call does not change either way — only where the string comes
from.

### What has to be true for a push to arrive

1. Provider credentials configured on the dashboard — the Firebase service-account JSON for Android and
   web, or an APNs `.p8` key with its Key ID, Team ID and Bundle ID for iOS.
2. A push plugin in the project to mint the token, and `google-services.json` where it expects it.
3. The Firebase Android package name matching **Project Settings → Platforms → Android → Android Package
   Name** exactly. A mismatch fails silently.
4. A device build. Push cannot be exercised in the editor, in PIE, or on Windows.

If registration succeeds and nothing arrives, check (1) and (3) first — registration succeeding only proves
the SDK half.

Full walkthrough: **[Testing push notifications on Android](push-setup-android.md)**.

## Blueprint

Every call has a node under **Flock | Notifications**: `Flock Get Notifications`,
`Flock Get Unread Notification Count`, `Flock Get Notification Summary`, `Flock Mark Notification Read`,
`Flock Mark All Notifications Read`, `Flock Get Notification Templates`,
`Flock Get Notification Template By Name`, `Flock Schedule Notification`,
`Flock Cancel Scheduled Notification`, `Flock Register Device Token`,
`Flock Register Device Token For Platform` and `Flock Unregister Device Token`.

Pure helpers: `Is Read`, `Is Pending`, `Is Delivered`, `Is Canceled`,
`Flock Get Current Device Platform`, `Device Platform To String` and `Notification Channel To String`.

The two events need no node of their own — drag off `Flock Get Events` and *Assign* `On Unread Count
Changed` or `On Notification Received`.

Build a `Variables` bag with the chainable `Set Command …` nodes — it is the same flat typed value bag the
game-command calls use.
