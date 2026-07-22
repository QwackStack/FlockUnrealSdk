// Copyright 2022, Qwacks. All Rights Reserved.

// A dev-only console command that drives the Flock SDK surface and narrates each step to the log,
// so you can watch boot/init, authentication, and analytics behavior against your configured Flock
// backend.
// It initializes from Project Settings > Flock SDK (API URL, key, and the resolved Game Version),
// so it needs valid settings and a reachable backend to get past init. Run from the editor or
// in-game console: `Flock.SelfTest` (also works via -ExecCmds in a development build).
//
// This is a demonstration harness, not a unit test — it exercises a transient subsystem instance
// through the injectable IFlockLogger so the breadcrumbs are visible end to end. Guards and edge
// cases live in the automation tests; this just proves the surface is wired and nothing crashes.

#if !UE_BUILD_SHIPPING

#include "FlockSubsystem.h"
#include "Analytics/FlockLogSink.h"
#include "Analytics/FlockMetadata.h"
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

	/**
	 * Drives the analytics session lifecycle against the configured backend, then hands off to
	 * Teardown. Runs signed in, because a session needs a player id — which is why it is chained off
	 * the auth sweep rather than run alongside it.
	 *
	 * This is the part that only a real backend can prove: session start is the one analytics route
	 * with a typed response, and session end is the only PATCH. Both pass against test fakes whatever
	 * the wire shape is.
	 */
	void RunAnalyticsSweep(FFlockAnalyticsProvider& AnalyticsRef, const FString& PlayerId,
		const TSharedRef<IFlockLogger>& Logger, TFunction<void()> Teardown)
	{
		FFlockAnalyticsProvider* Analytics = &AnalyticsRef;

		// Signing in auto-starts a session when Auto Start Session is on, so one may already be in
		// flight here — that wiring is itself worth seeing.
		Logger->LogInfo(FString::Printf(
			TEXT("Self-test: analytics signed in — HasConsent=%s SessionAutoStarted=%s ServerSessionId='%s'"),
			Analytics->HasConsent() ? TEXT("true") : TEXT("false"),
			Analytics->HasActiveSession() ? TEXT("true") : TEXT("false"),
			*Analytics->GetCurrentSessionId()));

		// Close whatever sign-in opened, so the id narrated below is unambiguously this sweep's.
		Analytics->EndSession(EFlockSessionEndReason::Manual,
			[Analytics, PlayerId, Logger, Teardown](TFlockResult<FFlockAnalyticsAck>)
			{
				Analytics->StartSession(PlayerId,
					[Analytics, Logger, Teardown](TFlockResult<FString> StartResult)
					{
						if (!StartResult.bSuccess)
						{
							Logger->LogInfo(FString::Printf(
								TEXT("Self-test: analytics start session -> failed (%s); skipping the rest of the sweep."),
								*StartResult.Error.Message));
							Teardown();
							return;
						}
						Logger->LogInfo(FString::Printf(
							TEXT("Self-test: analytics start session -> server session id %s"), *StartResult.Value));

						// Screen views only count against a live session, which is exactly what the
						// signed-out probe earlier cannot exercise.
						Analytics->RecordScreenView(TEXT("MainMenu"));
						Analytics->RecordScreenView(TEXT("Shop"));
						Analytics->LogEvent(TEXT("self-test event (signed in)"),
							FFlockMetadata().Add(TEXT("source"), TEXT("Flock.SelfTest")).Add(TEXT("sweep"), 2));

						FFlockLogDetails ErrorDetails;
						ErrorDetails.LogicalExpression = TEXT("selfTest == true");
						ErrorDetails.ErrorCode = TEXT("SELF_TEST");
						Analytics->LogError(TEXT("self-test logic error"), ErrorDetails);

						// No stack trace argument: the SDK walks one. This used to pass a hand-written
						// "at SelfTest()" placeholder, which is exactly the failure a required
						// stack-trace parameter invites.
						Analytics->LogException(TEXT("self-test exception"));

						const FFlockSessionSnapshot Snapshot = Analytics->GetCurrentSnapshot();
						Logger->LogInfo(FString::Printf(
							TEXT("Self-test: analytics snapshot — screens=%d firstSession=%s pendingSpooled=%d"),
							Snapshot.ScreensViewed,
							Snapshot.IsFirstSession ? TEXT("true") : TEXT("false"),
							Analytics->GetPendingEventCount()));

						Analytics->Flush([Analytics, Logger, Teardown](TFlockResult<FFlockAnalyticsAck> FlushResult)
						{
							Logger->LogInfo(FlushResult.bSuccess
								? FString::Printf(TEXT("Self-test: analytics flush -> spool drained (pending=%d)"),
									Analytics->GetPendingEventCount())
								: FString::Printf(TEXT("Self-test: analytics flush -> failed (%s); entries stay spooled."),
									*FlushResult.Error.Message));

							Analytics->EndSession(EFlockSessionEndReason::Manual,
								[Analytics, Logger, Teardown](TFlockResult<FFlockAnalyticsAck> EndResult)
								{
									Logger->LogInfo(EndResult.bSuccess
										? FString(TEXT("Self-test: analytics end session -> closed out on the backend."))
										: FString::Printf(TEXT("Self-test: analytics end session -> failed (%s)"),
											*EndResult.Error.Message));

									// Consent round trip: withdrawing must stop collection and drop the
									// queue; restoring must reopen the session (the opt-in path).
									Analytics->LogEvent(TEXT("self-test entry before revoke"));
									const int32 PendingBefore = Analytics->GetPendingEventCount();
									Analytics->SetConsent(false);
									Logger->LogInfo(FString::Printf(
										TEXT("Self-test: analytics consent withdrawn — pending %d -> %d, HasConsent=%s"),
										PendingBefore, Analytics->GetPendingEventCount(),
										Analytics->HasConsent() ? TEXT("true") : TEXT("false")));

									Analytics->SetConsent(true);
									Logger->LogInfo(FString::Printf(
										TEXT("Self-test: analytics consent restored — HasConsent=%s SessionReopened=%s"),
										Analytics->HasConsent() ? TEXT("true") : TEXT("false"),
										Analytics->HasActiveSession() ? TEXT("true") : TEXT("false")));

									// Leave no residue on this machine: the spool, the persisted consent
									// decision, and any crash marker all go.
									Analytics->EraseLocalData();
									Logger->LogInfo(TEXT("Self-test: analytics local data erased (spool + consent decision + crash marker)."));

									Teardown();
								});
						});
					});
			});
	}

	// Runs the auth surface against the configured backend. The register -> login flow is chained (login
	// fires on register success OR "already registered") and hands off to the analytics sweep — which
	// calls Teardown when it finishes; the other calls are independent one-shots that narrate their own
	// result. Every completion captures shared refs / the raw providers (kept alive by the caller's
	// AddToRoot until Teardown), never `this`, so a late arrival stays safe.
	void RunAuthSweep(FFlockAuthProvider& AuthRef, FFlockAnalyticsProvider* Analytics,
		const TSharedRef<IFlockLogger>& Logger, TFunction<void()> Teardown)
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
			[Auth, Analytics, Logger, Teardown, NarrateAction](TFlockResult<FFlockRegisterResult> Result)
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
					[Auth, Analytics, Logger, Teardown, NarrateAction](TFlockResult<FFlockPlayerLoginResponse> LoginResult)
					{
						if (!LoginResult.bSuccess)
						{
							Logger->LogInfo(FString::Printf(
								TEXT("Self-test: email login -> failed (%s); skipping the signed-in flows."), *LoginResult.Error.Message));
							Teardown();
							return;
						}

						Logger->LogInfo(FString::Printf(TEXT("Self-test: email login -> signed in as %s"), *LoginResult.Value.PlayerId));
						const FString PlayerId = LoginResult.Value.PlayerId;

						// Signed in from here: the bearer rides along automatically.
						Auth->SendEmailVerification(
							[Auth, Analytics, PlayerId, Logger, Teardown, NarrateAction](TFlockResult<FFlockAuthActionResponse> SendResult)
							{
								NarrateAction(TEXT("send email verification (signed in)"), SendResult);

								// The real code arrives by email, so this placeholder is expected to be
								// rejected — a code error here still proves the authenticated round trip.
								Auth->VerifyEmail(DemoCode,
									[Analytics, PlayerId, Logger, Teardown, NarrateAction](TFlockResult<FFlockAuthActionResponse> VerifyResult)
									{
										NarrateAction(TEXT("verify email (signed in, placeholder code)"), VerifyResult);

										// Analytics needs a signed-in player, so it runs here and owns
										// the teardown from this point.
										if (Analytics != nullptr)
										{
											RunAnalyticsSweep(*Analytics, PlayerId, Logger, Teardown);
											return;
										}
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

		// ── Analytics (signed out) ──
		// A session needs a player id, so none can open yet. What can be shown here is that the log
		// API and the disk spool work before sign-in — an entry recorded now is not lost, and the
		// sweep after sign-in delivers it. Screen views are deliberately NOT called here: they only
		// count against a live session, so they would be a silent no-op.
		FFlockAnalyticsProvider* Analytics = Sdk->GetAnalyticsProvider();
		if (Analytics != nullptr)
		{
			Logger->LogInfo(FString::Printf(
				TEXT("Self-test: analytics signed out — HasConsent=%s HasActiveSession=%s PendingSpooled=%d"),
				Analytics->HasConsent() ? TEXT("true") : TEXT("false"),
				Analytics->HasActiveSession() ? TEXT("true") : TEXT("false"),
				Analytics->GetPendingEventCount()));

			Analytics->LogEvent(TEXT("self-test event (signed out)"),
				FFlockMetadata().Add(TEXT("source"), TEXT("Flock.SelfTest")).Add(TEXT("signedIn"), false));
			Logger->LogInfo(FString::Printf(
				TEXT("Self-test: analytics spooled an entry while signed out -> pending=%d (delivered by the sweep after sign-in)."),
				Analytics->GetPendingEventCount()));

			// Automatic capture demo. Nothing below calls the SDK: the log sink picks this up off
			// GLog, walks the callstack, and the next tick turns it into a spooled exception. The
			// SDK's own categories are excluded, so an SDK error could not demonstrate this.
			UE_LOG(LogTemp, Error, TEXT("Flock self-test: simulated unhandled error (automatic capture demo)"));

			// Peek (not dequeue) so the entry still drains to the backend on the next tick. This is
			// the exact payload that will be sent, printed here so a capture problem is visible
			// locally instead of only in the dashboard.
			if (const FFlockLogSink* Sink = Analytics->GetLogSinkForTesting())
			{
				FFlockCapturedLog Captured;
				if (Sink->Peek(Captured))
				{
					TArray<FString> Lines;
					Captured.StackTrace.ParseIntoArrayLines(Lines);
					Logger->LogInfo(FString::Printf(
						TEXT("Self-test: sink captured '%s' [%s] with %d callstack frames:"),
						*Captured.Message, *Captured.Category.ToString(), Lines.Num()));
					for (int32 Index = 0; Index < FMath::Min(Lines.Num(), 8); ++Index)
					{
						Logger->LogInfo(FString::Printf(TEXT("Self-test:   #%d %s"), Index, *Lines[Index]));
					}
					if (Lines.Num() > 8)
					{
						Logger->LogInfo(FString::Printf(TEXT("Self-test:   ... %d more frames"), Lines.Num() - 8));
					}
				}
				else
				{
					Logger->LogError(TEXT("Self-test: the engine error was NOT captured by the sink."));
				}
			}

			if (GIsEditor)
			{
				Logger->LogInfo(TEXT("Self-test: analytics crash reporting is off in the editor "
					"(stopping Play-In-Editor is not an app death), so no app_termination check runs here."));
			}
		}
		else
		{
			Logger->LogInfo(TEXT("Self-test: analytics is off in Project Settings > Flock SDK; skipping the analytics sweep."));
		}

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

		RunAuthSweep(*Auth, Analytics, Logger, Teardown);
		Logger->LogInfo(TEXT("Self-test: auth sweep dispatched; the analytics session sweep runs once it signs in, "
			"and teardown follows that."));
	}

	FAutoConsoleCommand GFlockSelfTestCommand(
		TEXT("Flock.SelfTest"),
		TEXT("Drives the Flock SDK surface (boot/init + auth) and narrates each step to the log (development builds only)."),
		FConsoleCommandDelegate::CreateStatic(&RunFlockSelfTest));
}

#endif // !UE_BUILD_SHIPPING
