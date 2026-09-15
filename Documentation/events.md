# SDK events

`GetEvents()` returns the SDK event hub (`UFlockEvents`): lifecycle (`OnInitialized`,
`OnInitializationFailed`, `OnShutdown`), auth (`OnAuthenticated`, `OnTokenRefreshed`, `OnAuthExpired`,
`OnLoggedOut`, `OnSessionRestored`, `OnAccountLinked`, `OnAccountUnlinked`), session
(`OnSessionStarted`, `OnSessionRegistered`, `OnSessionEnded`, `OnSessionPaused`, `OnSessionResumed`), notifications
(`OnUnreadCountChanged`, `OnNotificationReceived`), and consent (`OnConsentChanged`). All are
Blueprint-assignable and raised on the game thread.

## Blueprint

`Flock Get Events` (self-resolving, no Target pin needed) returns the hub; drag off it and bind any event
with an *Assign* node. Bind in `BeginPlay` or on construction.

## C++

```cpp
if (UFlockSubsystem* Sdk = UFlockSubsystem::Get(this))
{
    FFlockInitializedCallback OnReady;
    OnReady.BindDynamic(this, &AMyActor::HandleFlockReady);  // must be a UFUNCTION()
    Sdk->GetEvents()->CallOrRegister_OnInitialized(OnReady);
}
```

## Things worth knowing

- **Auto-init fires before you can bind.** Initialization completes during GameInstance startup, so a
  plain `OnInitialized` binding made in `BeginPlay` misses it. Use `CallOrRegister_OnInitialized` (or
  `..._OnInitializationFailed`) — it fires immediately when init already happened, otherwise on the
  next init. One-shot.
- **Subscriptions survive `ShutdownSdk()`.** They stay bound across re-initialization and are released
  with the GameInstance (dynamic delegates hold weak references, so destroyed subscribers are skipped).
- **A session has two ids.** `OnSessionStarted` carries the local id, the moment the session begins.
  `OnSessionRegistered` follows once the server has answered, with the local id and the server's id, which is
  the one every other record of the session is filed under. It fires once per session, never for a session
  that ended before the server answered, and it is not replayed: if you bind after a session registered, read
  `GetAnalyticsSessionId()` instead. With **Analytics Heartbeat Interval** at 0, a start call that failed is not
  retried while the session runs, so it does not fire for that session.
- **The notification events are fetch-derived, not pushed.** There is no realtime channel and the SDK
  never polls, so `OnNotificationReceived` means *first seen by a read* — it rides the inbox and summary
  calls your game already makes and adds no traffic of its own. The first read for a player is silent, so
  an existing inbox never arrives as a burst on launch. See [Notifications](notifications.md).

---

[← Back to the README](../README.md)
