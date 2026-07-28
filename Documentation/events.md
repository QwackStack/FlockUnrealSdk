# SDK events

`GetEvents()` returns the SDK event hub (`UFlockEvents`): lifecycle (`OnInitialized`,
`OnInitializationFailed`, `OnShutdown`), auth (`OnAuthenticated`, `OnTokenRefreshed`, `OnAuthExpired`,
`OnLoggedOut`, `OnSessionRestored`), session (`OnSessionStarted`, `OnSessionEnded`, `OnSessionPaused`,
`OnSessionResumed`), and consent (`OnConsentChanged`). All are Blueprint-assignable and raised on the
game thread; auth/session/consent events are declared now and raised by their features as they land.

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

---

[← Back to the README](../README.md)
