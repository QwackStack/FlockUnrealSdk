// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Flock.h"
#include "FlockSubsystem.h"
#include "HAL/IConsoleManager.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformMisc.h"
#include "Http/FlockError.h"
#include "Models/FlockAuthModels.h"
#include "Providers/FlockAuthProvider.h"

/**
 * Console commands for driving a game that has no interface of its own: a playtest build, or the test harness.
 *
 * `Flock.LoginWithDevice [device id]` -- signs a player in from the console.
 *
 * A device login is the one that needs no account and no password, which is what a playtest actually uses. It is also
 * what the rest of the chain waits on: no analytics session starts until a player is signed in, and the playtest
 * session waits in turn for that session to reach the server, so a recording made without one has nowhere to go.
 *
 * Development builds only, like the self-test command beside it. Shipping compiles console commands out anyway.
 */
namespace FlockDevelopmentCommands
{
	/** The SDK of the game that is actually running. The self-test builds a game instance of its own; this must not. */
	UFlockSubsystem* FindRunningFlockSubsystem()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType != EWorldType::Game && Context.WorldType != EWorldType::PIE)
			{
				continue;
			}
			if (UGameInstance* GameInstance = Context.OwningGameInstance)
			{
				if (UFlockSubsystem* Sdk = GameInstance->GetSubsystem<UFlockSubsystem>())
				{
					return Sdk;
				}
			}
		}
		return nullptr;
	}

	void SignInWithDevice(const TArray<FString>& Arguments)
	{
		UFlockSubsystem* Sdk = FindRunningFlockSubsystem();
		if (Sdk == nullptr || !Sdk->IsInitialized())
		{
			UE_LOG(LogFlock, Warning, TEXT("Flock.LoginWithDevice: no running game whose Flock SDK has started."));
			return;
		}
		FFlockAuthProvider* Auth = Sdk->GetAuthProvider();
		if (Auth == nullptr)
		{
			UE_LOG(LogFlock, Warning, TEXT("Flock.LoginWithDevice: the SDK has no auth provider."));
			return;
		}

		// A device id of this machine's own, so two machines are two players; an argument overrides it when a
		// particular player is wanted, or when one is being signed in twice from one machine.
		FString DeviceId = Arguments.Num() > 0 ? Arguments[0] : FPlatformMisc::GetLoginId();
		if (DeviceId.IsEmpty())
		{
			DeviceId = TEXT("flock-console-device");
		}
		UE_LOG(LogFlock, Log, TEXT("Flock.LoginWithDevice: signing in with the device id %s."), *DeviceId);

		// Signing in exactly once is the whole point of this order, and getting it wrong is not obvious: registering
		// first and then signing in authenticates twice, and the second sign-in ends the session the first one
		// started -- before that session's registration comes back. A playtest session waits for a Flock session that
		// is still running when its server id arrives, and never one registered from the spool, so a double sign-in
		// quietly stops a playtest from ever starting. Measured 2026-09-16, which is why it is spelled out here.
		// The completions hold a weak subsystem rather than the provider, because a shutdown between two calls would
		// otherwise leave them calling into a provider that is gone.
		const TWeakObjectPtr<UFlockSubsystem> WeakSdk(Sdk);
		Auth->LoginWithDevice(DeviceId, [WeakSdk, DeviceId](TFlockResult<FFlockPlayerLoginResponse> SignedIn)
		{
			if (SignedIn.IsSuccess())
			{
				UE_LOG(LogFlock, Log, TEXT("Flock.LoginWithDevice: signed in. An analytics session starts from here, "
					"and a playtest session once that session reaches the server."));
				return;
			}

			// The device has never been seen before, which is what a first run looks like. Register it, and sign in
			// only if registering did not already authenticate.
			UFlockSubsystem* LiveSdk = WeakSdk.Get();
			FFlockAuthProvider* LiveAuth = LiveSdk != nullptr ? LiveSdk->GetAuthProvider() : nullptr;
			if (LiveAuth == nullptr)
			{
				UE_LOG(LogFlock, Warning, TEXT("Flock.LoginWithDevice: the sign-in failed and the SDK is gone: %s"),
					*SignedIn.Error.ToDisplayText());
				return;
			}
			UE_LOG(LogFlock, Log, TEXT("Flock.LoginWithDevice: no player for that device yet, registering it."));
			LiveAuth->RegisterWithDevice(DeviceId, FString(),
				[WeakSdk, DeviceId](TFlockResult<FFlockRegisterResult> Registered)
			{
				UFlockSubsystem* RegisteredSdk = WeakSdk.Get();
				if (!Registered.IsSuccess() || RegisteredSdk == nullptr)
				{
					UE_LOG(LogFlock, Warning, TEXT("Flock.LoginWithDevice: the device could not be registered: %s"),
						*Registered.Error.ToDisplayText());
					return;
				}
				if (RegisteredSdk->IsAuthenticated())
				{
					UE_LOG(LogFlock, Log, TEXT("Flock.LoginWithDevice: registered and signed in as %s."),
						*RegisteredSdk->GetPlayerId());
					return;
				}
				FFlockAuthProvider* RegisteredAuth = RegisteredSdk->GetAuthProvider();
				if (RegisteredAuth == nullptr)
				{
					return;
				}
				RegisteredAuth->LoginWithDevice(DeviceId, [](TFlockResult<FFlockPlayerLoginResponse> SignedInAfter)
				{
					if (!SignedInAfter.IsSuccess())
					{
						UE_LOG(LogFlock, Warning, TEXT("Flock.LoginWithDevice: the sign-in failed: %s"),
							*SignedInAfter.Error.ToDisplayText());
						return;
					}
					UE_LOG(LogFlock, Log, TEXT("Flock.LoginWithDevice: signed in."));
				});
			});
		});
	}

	FAutoConsoleCommand GFlockLoginWithDeviceCommand(
		TEXT("Flock.LoginWithDevice"),
		TEXT("Signs a player in with a device id, so a running game starts an analytics session without a sign-in "
			"screen of its own. Takes the device id to use, or picks one for this machine (development builds only)."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&SignInWithDevice));

	/**
	 * `Flock.QuitAfterSeconds [seconds]` -- asks the game to close, the way a player closing it does, after a wait.
	 *
	 * It exists because nothing else could drive a shutdown to look at. A run started with -unattended has no window,
	 * so the operating system has nothing to ask to close; `-ExecCmds` all run on the first frame, which is before a
	 * session has reached the server; and the automation runner's own `Quit` ends the process with TerminateProcess,
	 * so none of the teardown a session end is sent from ever runs. The wait is what makes the difference: it leaves a
	 * real session running at the moment the game is asked to close, which is the only state worth measuring.
	 */
	void QuitAfterSeconds(const TArray<FString>& Arguments)
	{
		float SecondsToWait = Arguments.Num() > 0 ? FCString::Atof(*Arguments[0]) : 10.f;
		if (!(SecondsToWait > 0.f))
		{
			SecondsToWait = 10.f;
		}
		UE_LOG(LogFlock, Log, TEXT("Flock.QuitAfterSeconds: asking the game to close in %.0f seconds."), SecondsToWait);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			UE_LOG(LogFlock, Log, TEXT("Flock.QuitAfterSeconds: asking the game to close now."));
			// The same request the Quit console command makes, and never a forced one: a forced exit is the thing
			// being avoided, since it takes the process down before any of the teardown has run.
			FPlatformMisc::RequestExit(false);
			return false;
		}), SecondsToWait);
	}

	FAutoConsoleCommand GFlockQuitAfterSecondsCommand(
		TEXT("Flock.QuitAfterSeconds"),
		TEXT("Asks the game to close after a wait, the way a player closing it does, so shutdown can be driven from a "
			"harness with a session still running. Takes the seconds to wait (development builds only)."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&QuitAfterSeconds));
}

#endif // !UE_BUILD_SHIPPING
