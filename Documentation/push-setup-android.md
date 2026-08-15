# Testing push notifications on Android

Everything you need to get a real push banner onto a real handset, in order. Budget an hour the first
time; most of it is waiting on Firebase and a device build.

Push cannot be tested in the editor, in Play In Editor, or in a Windows build — Unreal's desktop
implementation is a stub that reports "not registered" and does nothing. **A device build is not optional.**

## The chain, so you know what you are building

1. **Firebase** issues your app a *registration token* on the device.
2. A **push plugin** in your Unreal project hands that token to your game.
3. Your game passes it to **`RegisterDeviceToken`**, and Flock stores it against the signed-in player.
4. The **Flock backend** sends to that token using your Firebase **service-account** credentials.

Steps 1–2 are outside this plugin **for now**. Unreal does ship a first-party Android push path — the
`GoogleCloudMessaging` engine plugin — but it targets Google Cloud Messaging, which Google decommissioned
in 2019 (see the note in step 3), so a current Firebase plugin is the practical route today. Step 4 needs
credentials on the dashboard; skip it and tokens register perfectly while nothing is ever delivered.

> **Built-in token acquisition is planned.** Needing a paid marketplace plugin to use push is a poor fit
> for a free-tier SDK, and we intend to close it:
>
> - **iOS needs nothing extra even today.** Unreal already delivers the APNs token, and Flock's backend
>   speaks APNs directly — no Firebase anywhere in that path. Ten lines of your own code get you there
>   right now; see [iOS today](#ios-today-no-plugin-required) below. A one-call wrapper is coming.
> - **Android** will get a first-party path in the SDK: a UPL shipping the modern
>   `firebase-messaging` binding, replacing the engine's decommissioned GCM one. Until that lands, a
>   third-party plugin is the way.
>
> Nothing you set up below is wasted — the dashboard credentials, the template, the Firebase project and
> the package name are all required either way. Only step 3 changes.

## 1. Create the Firebase project

1. Go to [console.firebase.google.com](https://console.firebase.google.com) → **Add a project**.
2. Inside the project, **Add app → Android**.
3. It asks for an **Android package name**. This **must exactly match** your Unreal package name — in
   **Project Settings → Platforms → Android → APK Packaging → Android Package Name**, typically
   `com.YourCompany.YourProject`. A mismatch here is the single most common cause of "the token registers
   but nothing arrives", and it fails silently.
4. Finish the wizard.

## 2. Download the two files you need

They are different files, from different pages, and they go to different places. Mixing them up wastes an
afternoon.

| File | Where to get it | Where it goes | What it is |
| --- | --- | --- | --- |
| `google-services.json` | Firebase → **Project Settings → General → Your apps → Download** | Your **Unreal project** (see step 3) | Client config. Lets the device obtain a token. |
| Service-account JSON | Firebase → **Project Settings → Service accounts → Generate new private key** | The **Flock dashboard** (see step 5) | Server credential. Lets Flock's backend send. |

> The service-account key is a **secret** — it grants send rights on your Firebase project. Do not commit
> it, and do not ship it in the game. It belongs only in the dashboard field, which stores it encrypted and
> write-only.

## 3. Install a push plugin in the Unreal project

The plugin's job is to obtain the FCM registration token. **It must give you a real FCM registration
token** — if you use a vendor SDK other than Firebase, check that it exposes the underlying FCM token
rather than its own subscriber/player ID, because Flock's backend sends through FCM and a vendor ID will
not resolve.

Using **Firebase Features** (Fab / Epic marketplace), which is the common choice:

1. Download it from **Fab**, start the editor, enable the **Firebase** plugin under **Plugins**, close the
   editor.
2. Copy `google-services.json` into **`<YourProject>/Services/`** — the project's `Services` folder, not
   `Config` and not the plugin folder. Create the folder if it does not exist.
3. If you are calling it from C++, add `"FirebaseFeatures"` to `PrivateDependencyModuleNames` in
   `YourProject.Build.cs`, then regenerate project files.

Sanity check: start the editor and watch the Output Log. If you see
`Failed to create Firebase Application. Make sure the google-services.json file exists and is valid`, the
file is missing or in the wrong place. No message means it loaded.

> **Leave the Google Cloud Messaging Sender ID blank** in Project Settings → Platforms → Android.
>
> That field is not dead — it gates Unreal's own `GoogleCloudMessaging` plugin (enabled by default,
> Android-only), which injects a Java registration service and broadcasts the engine's remote-notification
> delegates. So Unreal *does* ship a first-party Android push path.
>
> But that path is built on `com.google.android.gms:play-services-gcm` and `InstanceID.getToken`, and
> **Google shut GCM down on 29 May 2019** and deprecated the Instance ID API. Whether it still yields a
> usable token against current Play Services is doubtful, and it is not something this guide has tested.
>
> Leaving the field blank disables that plugin, which is what you want here: it also stops two registration
> paths competing for the same `FCoreDelegates` broadcasts. Use the Firebase plugin's own token callback
> instead.

## 4. Hook the token to Flock

One binding, once, after the player signs in. In C++:

```cpp
#include "FirebaseFeatures/Public/Messaging/FirebaseMessaging.h"   // plugin header
#include "Providers/FlockNotificationProvider.h"
#include "FlockSubsystem.h"

FFirebaseMessagingLibrary::OnTokenReceived().AddLambda([this](const FString& Token)
{
    if (UFlockSubsystem* Sdk = UFlockSubsystem::Get(this))
    {
        if (FFlockNotificationProvider* Notifications = Sdk->GetNotificationProvider())
        {
            Notifications->RegisterDeviceToken(Token, [](TFlockResult<FFlockDeviceToken> Result)
            {
                UE_LOG(LogTemp, Log, TEXT("Flock push registration: %s"),
                    Result.bSuccess ? TEXT("ok") : *Result.Error.Message);
            });
        }
    }
});
```

In Blueprint: bind the plugin's **On Token Received** event, drag the token string into
**Flock Register Device Token**.

The platform is taken from the running build, so on an Android device it resolves to `android`
automatically. Bind the delegate **before** the token can arrive — it fires once at startup — and
re-register whenever the OS issues a new one.

`Flock Register Device Token` requires a **signed-in player**. If your token arrives before sign-in, cache
it and register after `OnAuthenticated`.

## 5. Configure the Flock dashboard

Two things, both on the dashboard:

**Credentials** — **Settings → Push Notifications → Firebase Cloud Messaging (FCM) → Configure**. Paste the
**full contents** of the service-account JSON from step 2. It should read **Configured** afterwards.
Credentials are per game, encrypted, and write-only — replaceable, never readable back.

**A template** — **Marketing → Templates**. Give it a name (that name is what
`ScheduleByTemplateName` takes), a title and body, locale, and tick **push** among its channels. Any
`{placeholder}` in the title or body is filled from the `Variables` you pass at schedule time.

## 6. Android 13+ notification permission

From Android 13 (API 33), `POST_NOTIFICATIONS` is a **runtime permission**. If it is not granted, the token
registers and delivery is silently dropped. Check that your push plugin requests it, or request it yourself
at a sensible moment. This is a platform requirement, not a Flock one, and it is easy to miss because
nothing errors.

## 7. Build, deploy, and verify

1. Package for Android (**Platforms → Android → Package Project**) and install on a device. An emulator
   works only if it has Google Play services.
2. Launch the game, sign in, and check the log for the registration line.
3. Send one: **Marketing → Campaigns**, pick your template, target the player, send now. Or have the game
   call `ScheduleByTemplateName` a minute out and background the app.
4. The banner should appear within seconds.

### If nothing arrives

Work down this list — it is ordered by how often each is the cause.

| Symptom | Likely cause |
| --- | --- |
| Registration succeeded, no banner ever | FCM shows **Not configured** on the dashboard, or the package name in Firebase does not match Unreal's |
| No token in the log at all | `google-services.json` missing or in the wrong folder — check the editor's Output Log for the Firebase error |
| Token arrives, `RegisterDeviceToken` fails with an auth error | No player signed in yet — register after `OnAuthenticated` |
| Token arrives, register fails with a validation error | You are running on desktop or in the editor, where push is unavailable by design |
| Works in one build, not another | Android 13+ notification permission not granted |

**Registration succeeding proves only the SDK half.** The client reports success because it genuinely
succeeded — the token reached Flock. If no banner appears, suspect the dashboard credentials and the
package name before the client.

## What is already verified without a device

`Flock.SelfTest` covers the registration and unregistration calls on any platform by registering a
synthetic token under the `web` platform and removing it again. That proves the request, response, auth and
parse — everything this SDK owns. It cannot prove a banner arrives, which is what the steps above are for.

## iOS today: no plugin required

iOS needs **no third-party plugin at all**, and never did — Unreal surfaces the APNs token itself, and
Flock's backend speaks APNs directly. There is no Firebase anywhere in the iOS path.

Dashboard side: **Settings → Push Notifications → APNs** takes the `.p8` signing key, its **Key ID**, your
**Team ID** and the app's **Bundle ID**. Tick **Use APNs sandbox** for development builds — a sandbox token
will not accept production credentials, or the reverse.

Project side: tick **Enable Remote Notifications Support** in **Project Settings → Platforms → iOS**.

Then bind the engine's own delegate and hand the token to Flock. The engine gives you raw APNs bytes, so
hex-encode them:

```cpp
#include "Misc/CoreDelegates.h"

FCoreDelegates::ApplicationRegisteredForRemoteNotificationsDelegate.AddLambda(
    [this](TArray<uint8> TokenBytes)
    {
        FString Token;
        for (uint8 Byte : TokenBytes)
        {
            Token += FString::Printf(TEXT("%02x"), Byte);
        }

        if (UFlockSubsystem* Sdk = UFlockSubsystem::Get(this))
        {
            if (FFlockNotificationProvider* Notifications = Sdk->GetNotificationProvider())
            {
                Notifications->RegisterDeviceToken(Token, [](TFlockResult<FFlockDeviceToken>) {});
            }
        }
    });

FPlatformMisc::RegisterForRemoteNotifications();   // triggers the callback above
```

Bind before calling `RegisterForRemoteNotifications`, and register after sign-in (the call needs a player).

A built-in wrapper for this is coming so you will not have to write it — but it works today, on the free
tier, with no purchase.

> The delegate does not fire under `-unattended`, which is why the automation suite cannot cover this path
> and `Flock.SelfTest` registers a synthetic `web` token instead.
