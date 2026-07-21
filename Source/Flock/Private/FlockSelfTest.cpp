// Copyright 2022, Qwacks. All Rights Reserved.

// A dev-only console command that drives the Flock SDK surface and narrates each step to the log,
// so you can watch boot/init and authentication behavior against your configured Flock backend.
// It initializes from Project Settings > Flock SDK (API URL, key, and the resolved Game Version),
// so it needs valid settings and a reachable backend to get past init. Run from the editor or
// in-game console: `Flock.SelfTest` (also works via -ExecCmds in a development build).
//
// This is a demonstration harness, not a unit test — it exercises a transient subsystem instance
// through the injectable IFlockLogger so the breadcrumbs are visible end to end. Guards and edge
// cases live in the automation tests; this just proves the surface is wired and nothing crashes.

#if !UE_BUILD_SHIPPING

#include "FlockSubsystem.h"
#include "FlockLogger.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"
#include "Http/FlockResult.h"
#include "Models/FlockAuthModels.h"
#include "Providers/FlockAuthProvider.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	// Demo credentials for the auth sweep. Mirror the canonical SDK test values with a "UE" suffix so
	// a player registered from this SDK stays distinct from one made elsewhere. The sweep runs against
	// your configured backend: a first run registers this player and signs in; later runs report
	// "already registered" and log in.
	const TCHAR* const DemoEmail = TEXT("pUE@x.com");
	const TCHAR* const DemoPassword = TEXT("pwUE");
	const TCHAR* const DemoName = TEXT("PlayerUE");
	const TCHAR* const DemoCode = TEXT("123456UE");
	const TCHAR* const DemoNewPassword = TEXT("new-pwUE");

	// Runs the auth surface against the configured backend. The register -> login flow is chained (login
	// fires on register success OR "already registered") and calls Teardown when it finishes; the other
	// calls are independent one-shots that narrate their own result. Every completion captures shared
	// refs / the raw provider (kept alive by the caller's AddToRoot until Teardown), never `this`, so a
	// late arrival stays safe.
	void RunAuthSweep(FFlockAuthProvider& AuthRef, const TSharedRef<IFlockLogger>& Logger, TFunction<void()> Teardown)
	{
		FFlockAuthProvider* Auth = &AuthRef;

		Logger->LogInfo(FString::Printf(
			TEXT("Self-test: auth sweep with demo creds email=%s name=%s against the configured backend."),
			DemoEmail, DemoName));

		// Shared narrator for the account flows that return {success}.
		auto NarrateAction = [Logger](const TCHAR* Label, TFlockResult<FFlockAuthActionResponse> Result)
		{
			Logger->LogInfo(Result.bSuccess
				? FString::Printf(TEXT("Self-test: %s -> success=%s"), Label, Result.Value.Success ? TEXT("true") : TEXT("false"))
				: FString::Printf(TEXT("Self-test: %s -> failed (%s)"), Label, *Result.Error.Message));
		};

		// Independent one-shots that work signed out. The reset/revoke/refresh calls fail fast on their
		// local guards here (nobody is signed in yet) — that is the guard behavior, not a server error.
		// The email-verification pair is bearer-only server-side, so it runs later, inside the chain.
		Auth->IsNameAvailable(DemoName, [Logger](TFlockResult<FFlockNameAvailableResponse> Result)
		{
			Logger->LogInfo(Result.bSuccess
				? FString::Printf(TEXT("Self-test: name '%s' available=%s"), DemoName, Result.Value.Available ? TEXT("true") : TEXT("false"))
				: FString::Printf(TEXT("Self-test: name availability -> failed (%s)"), *Result.Error.Message));
		});
		Auth->ForgotPassword(DemoEmail, [NarrateAction](TFlockResult<FFlockAuthActionResponse> Result)
		{
			NarrateAction(TEXT("forgot password"), Result);
		});
		Auth->ResetPassword(DemoEmail, DemoCode, DemoNewPassword, [NarrateAction](TFlockResult<FFlockAuthActionResponse> Result)
		{
			NarrateAction(TEXT("reset password"), Result);
		});
		Auth->RevokeToken([Logger](TFlockResult<FFlockTokenRevokeResponse> Result)
		{
			Logger->LogInfo(Result.bSuccess
				? FString::Printf(TEXT("Self-test: token revoke -> revoked=%s"), Result.Value.Revoked ? TEXT("true") : TEXT("false"))
				: FString::Printf(TEXT("Self-test: token revoke -> failed (%s)"), *Result.Error.Message));
		});
		Auth->RefreshToken([Logger](bool bRefreshed)
		{
			Logger->LogInfo(FString::Printf(TEXT("Self-test: token refresh -> %s"), bRefreshed ? TEXT("refreshed") : TEXT("no token to refresh")));
		});

		// Register -> login -> the bearer-only email flows -> teardown. The verification pair carries no
		// sign-in guard (the bearer just rides along when present), so it has to run here, after the
		// chain authenticates, to get a real answer instead of a server 401. Teardown runs on whichever
		// branch ends the chain, so the subsystem outlives every async round trip.
		Auth->RegisterWithEmail(DemoEmail, DemoPassword, DemoName,
			[Auth, Logger, Teardown, NarrateAction](TFlockResult<FFlockRegisterResult> Result)
			{
				if (!Result.bSuccess)
				{
					Logger->LogInfo(FString::Printf(TEXT("Self-test: email register -> failed (%s); skipping login."), *Result.Error.Message));
					Teardown();
					return;
				}

				Logger->LogInfo(Result.Value.bAlreadyRegistered
					? TEXT("Self-test: email register -> already registered; logging in.")
					: TEXT("Self-test: email register -> registered + signed in; logging in to confirm."));

				Auth->LoginWithEmail(DemoEmail, DemoPassword,
					[Auth, Logger, Teardown, NarrateAction](TFlockResult<FFlockPlayerLoginResponse> LoginResult)
					{
						if (!LoginResult.bSuccess)
						{
							Logger->LogInfo(FString::Printf(
								TEXT("Self-test: email login -> failed (%s); skipping the signed-in flows."), *LoginResult.Error.Message));
							Teardown();
							return;
						}

						Logger->LogInfo(FString::Printf(TEXT("Self-test: email login -> signed in as %s"), *LoginResult.Value.PlayerId));

						// Signed in from here: the bearer rides along automatically.
						Auth->SendEmailVerification(
							[Auth, Logger, Teardown, NarrateAction](TFlockResult<FFlockAuthActionResponse> SendResult)
							{
								NarrateAction(TEXT("send email verification (signed in)"), SendResult);

								// The real code arrives by email, so this placeholder is expected to be
								// rejected — a code error here still proves the authenticated round trip.
								Auth->VerifyEmail(DemoCode,
									[Logger, Teardown, NarrateAction](TFlockResult<FFlockAuthActionResponse> VerifyResult)
									{
										NarrateAction(TEXT("verify email (signed in, placeholder code)"), VerifyResult);
										Teardown();
									});
							});
					});
			});
	}

	void RunFlockSelfTest()
	{
		// Verbose logger so every breadcrumb prints, regardless of project settings.
		const TSharedRef<IFlockLogger> Logger = MakeShared<FFlockUnrealLogger>(/*bVerbose*/ true);
		Logger->LogInfo(TEXT("Self-test: starting."));

		// UFlockSubsystem is a UGameInstanceSubsystem (ClassWithin=UGameInstance), so its Outer must be a
		// UGameInstance — creating it directly under the transient package trips a "created in invalid
		// Outer" ensure. A throwaway transient GameInstance is a valid Outer for this driver.
		//
		// Both are rooted for the run: the register -> login chain finishes asynchronously, and teardown
		// must not race a GC that would drop the subsystem (and its provider) mid-chain.
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		UFlockSubsystem* Sdk = NewObject<UFlockSubsystem>(GameInstance);
		GameInstance->AddToRoot();
		Sdk->AddToRoot();
		Sdk->SetLogger(Logger);

		Logger->LogInfo(TEXT("Self-test: initializing from Project Settings > Flock SDK."));
		Sdk->InitializeFromSettings();
		if (!Sdk->IsInitialized())
		{
			Logger->LogError(FString::Printf(
				TEXT("Self-test: init failed (%s). Fill in Project Settings > Flock SDK and resolve the Game Version, then retry."),
				*Sdk->GetInitializationError()));
			Sdk->RemoveFromRoot();
			GameInstance->RemoveFromRoot();
			return;
		}
		Logger->LogInfo(FString::Printf(
			TEXT("Self-test: IsInitialized=true GameId=%s GameVersionId=%s VersionedUrl=%s"),
			*Sdk->GetGameId(), *Sdk->GetGameVersionId(), *Sdk->GetVersionedApiUrl()));

		Logger->LogInfo(FString::Printf(TEXT("Self-test: IsAuthenticated=%s PlayerId='%s' IsRestoringSession=%s"),
			Sdk->IsAuthenticated() ? TEXT("true") : TEXT("false"),
			*Sdk->GetPlayerId(),
			Sdk->IsRestoringSession() ? TEXT("true") : TEXT("false")));

		Logger->LogInfo(TEXT("Self-test: Logout to start from a clean signed-out state (safe when already signed out)."));
		Sdk->Logout();

		// Shut down + un-root once the register -> login chain ends. Safe to call while the one-shots are
		// still in flight: their completions hold their own shared refs and never touch the subsystem.
		TFunction<void()> Teardown = [Logger, GameInstance, Sdk]()
		{
			Logger->LogInfo(TEXT("Self-test: shutting down."));
			Sdk->ShutdownSdk();
			Sdk->RemoveFromRoot();
			GameInstance->RemoveFromRoot();
			Logger->LogInfo(TEXT("Self-test: complete."));
		};

		FFlockAuthProvider* Auth = Sdk->GetAuthProvider();
		if (!Auth)
		{
			Teardown();
			return;
		}

		RunAuthSweep(*Auth, Logger, Teardown);
		Logger->LogInfo(TEXT("Self-test: auth sweep dispatched; teardown runs when the register -> login chain finishes."));
	}

	FAutoConsoleCommand GFlockSelfTestCommand(
		TEXT("Flock.SelfTest"),
		TEXT("Drives the Flock SDK surface (boot/init + auth) and narrates each step to the log (development builds only)."),
		FConsoleCommandDelegate::CreateStatic(&RunFlockSelfTest));
}

#endif // !UE_BUILD_SHIPPING
