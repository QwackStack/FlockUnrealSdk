// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "FlockEvents.h"
#include "FlockLogger.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockProviderBase.h"
#include "Misc/Base64.h"
#include "Models/FlockAuthModels.h"
#include "Providers/FlockAuthProvider.h"
#include "Tests/Support/FlockEventTestListener.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockRecordingLogger.h"

namespace FlockAuthProviderTestHelpers
{
	inline FString Base64Url(const FString& In)
	{
		FString Encoded = FBase64::Encode(In);
		Encoded.ReplaceInline(TEXT("+"), TEXT("-"));
		Encoded.ReplaceInline(TEXT("/"), TEXT("_"));
		Encoded.ReplaceInline(TEXT("="), TEXT(""));
		return Encoded;
	}

	inline FString MakeJwt(const FString& PlayerId, int64 ExpiryOffsetSeconds = 3600)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + ExpiryOffsetSeconds;
		const FString Payload = FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp);
		return FString::Printf(TEXT("h.%s.s"), *Base64Url(Payload));
	}

	/** Login-response body for the fake transport. The player auth routes are NOT enveloped — the model sits at the root. */
	inline FString LoginBody(const FString& PlayerId, const FString& AccessToken, const FString& RefreshToken = TEXT("r-1"))
	{
		return FString::Printf(TEXT("{\"player_id\":\"%s\",\"access_token\":\"%s\",\"refresh_token\":\"%s\"}"),
			*PlayerId, *AccessToken, *RefreshToken);
	}

	/** Exposes the protected Execute/SetAuthSession for direct base-class testing. */
	class FTestProvider : public FFlockProviderBase
	{
	public:
		using FFlockProviderBase::FFlockProviderBase;
		using FFlockProviderBase::Execute;
		using FFlockProviderBase::SetAuthSession;
	};

	// MaxRetries=0 keeps every path synchronous (Auth errors are never handler-retried anyway).
	inline FFlockRetryPolicy NoRetryPolicy()
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = 0;
		return Policy;
	}

	struct FBaseFixture
	{
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockAuthSession> Session;
		FTestProvider Provider;

		FBaseFixture()
			: Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>()))
			, Provider(Client, NoRetryPolicy(), MakeShared<FFlockNullLogger>())
		{
			Provider.SetAuthSession(Session);
		}

		/** GET /data through the base Execute, fetching auth headers per attempt. */
		FFlockRequestHandle RunDataCall(TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete, bool bAllowAuthRetry = true)
		{
			const TSharedRef<FFlockHttpClient> ClientRef = Client;
			const TSharedRef<FFlockAuthSession> SessionRef = Session;
			return Provider.Execute<FFlockPlayerLoginResponse>(
				[ClientRef, SessionRef](TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnAttempt)
				{
					return ClientRef->GetRaw<FFlockPlayerLoginResponse>(TEXT("http://x/v1/data"), SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
				},
				MoveTemp(OnComplete), TEXT("Data call"), /*bIdempotent*/ true, /*MaxRetriesOverride*/ -1, bAllowAuthRetry);
		}
	};

	struct FProviderFixture
	{
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockAuthSession> Session;
		UFlockEvents* Events = nullptr;
		UFlockEventTestListener* Listener = nullptr;
		TUniquePtr<FFlockAuthProvider> Provider;
		/** What the provider reported, so a test can assert on log level as well as outcome. */
		TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();

		FProviderFixture()
			: Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
		{
			Events = NewObject<UFlockEvents>();
			Listener = NewObject<UFlockEventTestListener>();
			Events->OnAuthenticated.AddDynamic(Listener, &UFlockEventTestListener::HandleAuthenticated);
			Events->OnTokenRefreshed.AddDynamic(Listener, &UFlockEventTestListener::HandleTokenRefreshed);
			Events->OnAuthExpired.AddDynamic(Listener, &UFlockEventTestListener::HandleAuthExpired);
			Events->OnLoggedOut.AddDynamic(Listener, &UFlockEventTestListener::HandleLoggedOut);
			Events->OnSessionRestored.AddDynamic(Listener, &UFlockEventTestListener::HandleSessionRestored);
			Session->SetEvents(Events);
			Provider = MakeUnique<FFlockAuthProvider>(Client, NoRetryPolicy(),
				Log, Session, Events, TEXT("http://x/v1"));
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockProviderBaseSilentRefreshTest, "Flock.Auth.ProviderBase.SilentRefresh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockProviderBaseSilentRefreshTest::RunTest(const FString& Parameters)
{
	using namespace FlockAuthProviderTestHelpers;
	FString Error;

	// 401 while authenticated -> refresh -> single replay succeeds with the new bearer.
	{
		FBaseFixture F;
		F.Session->SetTokens(MakeJwt(TEXT("p-1")), TEXT("r-1"), Error);
		const FString NewJwt = MakeJwt(TEXT("p-1"), 7200);
		F.Fake->OnSequence(TEXT("data"), { FFlockFakeTransport::Status(401, TEXT("")),
			FFlockFakeTransport::Ok(LoginBody(TEXT("p-1"), NewJwt)) });
		F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Ok(LoginBody(TEXT("p-1"), NewJwt, TEXT("r-2"))));

		bool bSuccess = false;
		F.RunDataCall([&](TFlockResult<FFlockPlayerLoginResponse> R) { bSuccess = R.bSuccess; });

		TestTrue(TEXT("replay succeeded"), bSuccess);
		TestEqual(TEXT("two data attempts"), F.Fake->CountTo(TEXT("data")), 2);
		TestEqual(TEXT("one refresh"), F.Fake->CountTo(TEXT("token/refresh")), 1);
		TestEqual(TEXT("replay used new bearer"), F.Fake->Requests.Last().Headers[TEXT("Authorization")],
			FString::Printf(TEXT("Bearer %s"), *NewJwt));
	}
	// Refresh fails -> original Auth error surfaces, no replay.
	{
		FBaseFixture F;
		F.Session->SetTokens(MakeJwt(TEXT("p-1")), TEXT("r-1"), Error);
		F.Fake->On(TEXT("data"), FFlockFakeTransport::Status(401, TEXT("")));
		F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Status(401, TEXT("")));

		EFlockErrorType Type = EFlockErrorType::None;
		F.RunDataCall([&](TFlockResult<FFlockPlayerLoginResponse> R) { Type = R.Error.Type; });

		TestEqual(TEXT("original auth error"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Auth));
		TestEqual(TEXT("one data attempt"), F.Fake->CountTo(TEXT("data")), 1);
		TestFalse(TEXT("session cleared by refresh failure"), F.Session->IsAuthenticated());
	}
	// bAllowAuthRetry=false -> no refresh attempt.
	{
		FBaseFixture F;
		F.Session->SetTokens(MakeJwt(TEXT("p-1")), TEXT("r-1"), Error);
		F.Fake->On(TEXT("data"), FFlockFakeTransport::Status(401, TEXT("")));
		EFlockErrorType Type = EFlockErrorType::None;
		F.RunDataCall([&](TFlockResult<FFlockPlayerLoginResponse> R) { Type = R.Error.Type; }, /*bAllowAuthRetry*/ false);
		TestEqual(TEXT("auth error through"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Auth));
		TestEqual(TEXT("no refresh"), F.Fake->CountTo(TEXT("token/refresh")), 0);
	}
	// Unauthenticated session -> no refresh attempt.
	{
		FBaseFixture F;
		F.Fake->On(TEXT("data"), FFlockFakeTransport::Status(401, TEXT("")));
		EFlockErrorType Type = EFlockErrorType::None;
		F.RunDataCall([&](TFlockResult<FFlockPlayerLoginResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("auth error through"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Auth));
		TestEqual(TEXT("no refresh"), F.Fake->CountTo(TEXT("token/refresh")), 0);
	}
	// Non-auth failures never trigger refresh.
	{
		FBaseFixture F;
		F.Session->SetTokens(MakeJwt(TEXT("p-1")), TEXT("r-1"), Error);
		F.Fake->On(TEXT("data"), FFlockFakeTransport::Status(500, TEXT("")));
		EFlockErrorType Type = EFlockErrorType::None;
		F.RunDataCall([&](TFlockResult<FFlockPlayerLoginResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("network error"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Network));
		TestEqual(TEXT("no refresh"), F.Fake->CountTo(TEXT("token/refresh")), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAuthProviderLoginTest, "Flock.Auth.Provider.Login",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAuthProviderLoginTest::RunTest(const FString& Parameters)
{
	using namespace FlockAuthProviderTestHelpers;

	// Email login: URL, wire body, tokens adopted, method recorded, OnAuthenticated raised.
	{
		FProviderFixture F;
		const FString Jwt = MakeJwt(TEXT("p-1"));
		F.Fake->On(TEXT("player/login"), FFlockFakeTransport::Ok(LoginBody(TEXT("p-1"), Jwt)));

		bool bSuccess = false;
		FString PlayerId;
		F.Provider->LoginWithEmail(TEXT("a@b.c"), TEXT("pw"),
			[&](TFlockResult<FFlockPlayerLoginResponse> R) { bSuccess = R.bSuccess; PlayerId = R.Value.PlayerId; });

		TestTrue(TEXT("success"), bSuccess);
		TestEqual(TEXT("player id"), PlayerId, FString(TEXT("p-1")));
		const FFlockHttpRequest& Request = F.Fake->Requests.Last();
		TestEqual(TEXT("url"), Request.Url, FString(TEXT("http://x/v1/player/login")));
		TestEqual(TEXT("method"), Request.Method, FString(TEXT("POST")));
		TestTrue(TEXT("login_type"), Request.JsonBody.Contains(TEXT("\"login_type\":\"email\"")));
		TestTrue(TEXT("email"), Request.JsonBody.Contains(TEXT("\"email\":\"a@b.c\"")));
		TestFalse(TEXT("unused ids dropped"), Request.JsonBody.Contains(TEXT("device_id")));
		TestTrue(TEXT("api key header"), Request.Headers.Contains(TEXT("X-Flock-API-Key")));
		TestTrue(TEXT("session authenticated"), F.Session->IsAuthenticated());
		TestEqual(TEXT("session token"), F.Session->GetAccessToken(), Jwt);
		TestEqual(TEXT("method recorded"), static_cast<int32>(F.Session->GetAuthMethod().GetValue()), static_cast<int32>(EFlockAuthMethod::Email));
		TestEqual(TEXT("authenticated event"), F.Listener->AuthenticatedCount, 1);
		TestEqual(TEXT("event player id"), F.Listener->LastAuthInfo.PlayerId, FString(TEXT("p-1")));
		TestEqual(TEXT("event method"), static_cast<int32>(F.Listener->LastAuthInfo.Method), static_cast<int32>(EFlockAuthMethod::Email));
		TestTrue(TEXT("tokens persisted"), F.Store->bHasTokens);
	}
	// Each remaining login route: correct endpoint + key body field + method enum.
	{
		struct FCase
		{
			TFunction<void(FProviderFixture&)> Call;
			const TCHAR* UrlSuffix;
			const TCHAR* BodyFragment;
			EFlockAuthMethod Method;
		};
		const TArray<FCase> Cases = {
			{ [](FProviderFixture& F) { F.Provider->LoginWithDevice(TEXT("dev-1"), nullptr); },
				TEXT("/player/login/device"), TEXT("\"device_id\":\"dev-1\""), EFlockAuthMethod::Device },
			{ [](FProviderFixture& F) { F.Provider->LoginWithGoogle(TEXT("g-tok"), nullptr); },
				TEXT("/player/login/google"), TEXT("\"id_token\":\"g-tok\""), EFlockAuthMethod::Google },
			{ [](FProviderFixture& F) { F.Provider->LoginWithApple(TEXT("a-tok"), nullptr); },
				TEXT("/player/login/apple"), TEXT("\"identity_token\":\"a-tok\""), EFlockAuthMethod::Apple },
			{ [](FProviderFixture& F) { F.Provider->LoginWithSteam(TEXT("s-tik"), nullptr); },
				TEXT("/player/login/steam"), TEXT("\"session_ticket\":\"s-tik\""), EFlockAuthMethod::Steam },
			{ [](FProviderFixture& F) { F.Provider->LoginWithFacebook(TEXT("fb-1"), nullptr); },
				TEXT("/player/login"), TEXT("\"facebook_id\":\"fb-1\""), EFlockAuthMethod::Facebook },
			{ [](FProviderFixture& F) { F.Provider->LoginWithDiscord(TEXT("dc-1"), nullptr); },
				TEXT("/player/login"), TEXT("\"discord_id\":\"dc-1\""), EFlockAuthMethod::Discord },
		};
		for (const FCase& Case : Cases)
		{
			FProviderFixture F;
			F.Fake->On(TEXT("player/login"), FFlockFakeTransport::Ok(LoginBody(TEXT("p-1"), MakeJwt(TEXT("p-1")))));
			Case.Call(F);
			TestTrue(FString::Printf(TEXT("%s url"), Case.UrlSuffix), F.Fake->Requests.Last().Url.EndsWith(Case.UrlSuffix));
			TestTrue(FString::Printf(TEXT("%s body"), Case.BodyFragment), F.Fake->Requests.Last().JsonBody.Contains(Case.BodyFragment));
			TestEqual(TEXT("method recorded"), static_cast<int32>(F.Session->GetAuthMethod().GetValue()), static_cast<int32>(Case.Method));
		}
		// Device login also reports the platform.
		{
			FProviderFixture F;
			F.Fake->On(TEXT("player/login"), FFlockFakeTransport::Ok(LoginBody(TEXT("p-1"), MakeJwt(TEXT("p-1")))));
			F.Provider->LoginWithDevice(TEXT("dev-1"), nullptr);
			TestTrue(TEXT("device_type sent"), F.Fake->Requests.Last().JsonBody.Contains(TEXT("\"device_type\"")));
		}
		// Facebook/Discord ride the generic route with a login_type discriminator.
		{
			FProviderFixture F;
			F.Fake->On(TEXT("player/login"), FFlockFakeTransport::Ok(LoginBody(TEXT("p-1"), MakeJwt(TEXT("p-1")))));
			F.Provider->LoginWithFacebook(TEXT("fb-1"), nullptr);
			TestTrue(TEXT("facebook login_type"), F.Fake->Requests.Last().JsonBody.Contains(TEXT("\"login_type\":\"facebook\"")));
		}
	}
	// Response without an access token -> Auth error, still signed out, no event.
	{
		FProviderFixture F;
		F.Fake->On(TEXT("player/login"), FFlockFakeTransport::Ok(TEXT("{\"player_id\":\"p-1\"}")));
		EFlockErrorType Type = EFlockErrorType::None;
		F.Provider->LoginWithEmail(TEXT("a@b.c"), TEXT("pw"),
			[&](TFlockResult<FFlockPlayerLoginResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("auth error"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Auth));
		TestFalse(TEXT("signed out"), F.Session->IsAuthenticated());
		TestEqual(TEXT("no event"), F.Listener->AuthenticatedCount, 0);
	}
	// Unparseable JWT in the response -> Auth error, state untouched.
	{
		FProviderFixture F;
		F.Fake->On(TEXT("player/login"), FFlockFakeTransport::Ok(LoginBody(TEXT("p-1"), TEXT("not-a-jwt"))));
		EFlockErrorType Type = EFlockErrorType::None;
		F.Provider->LoginWithEmail(TEXT("a@b.c"), TEXT("pw"),
			[&](TFlockResult<FFlockPlayerLoginResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("auth error"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Auth));
		TestFalse(TEXT("signed out"), F.Session->IsAuthenticated());
	}
	// Credential failure passes through with no silent refresh on login routes.
	{
		FProviderFixture F;
		FString ReloginError;
		F.Session->SetTokens(MakeJwt(TEXT("p-0")), TEXT("r-0"), ReloginError); // pre-authenticated re-login case
		F.Fake->On(TEXT("player/login"), FFlockFakeTransport::Status(401, TEXT("")));
		EFlockErrorType Type = EFlockErrorType::None;
		F.Provider->LoginWithEmail(TEXT("a@b.c"), TEXT("bad-pw"),
			[&](TFlockResult<FFlockPlayerLoginResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("auth error surfaces"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Auth));
		TestEqual(TEXT("no refresh triggered"), F.Fake->CountTo(TEXT("token/refresh")), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAuthProviderRegisterTest, "Flock.Auth.Provider.Register",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAuthProviderRegisterTest::RunTest(const FString& Parameters)
{
	using namespace FlockAuthProviderTestHelpers;

	// Email registration with a display name: URL, body, tokens adopted, event raised.
	{
		FProviderFixture F;
		const FString Jwt = MakeJwt(TEXT("p-1"));
		F.Fake->On(TEXT("player/register"), FFlockFakeTransport::Ok(LoginBody(TEXT("p-1"), Jwt)));

		bool bSuccess = false;
		bool bAlready = true;
		F.Provider->RegisterWithEmail(TEXT("a@b.c"), TEXT("pw"), TEXT("Duck"),
			[&](TFlockResult<FFlockRegisterResult> R) { bSuccess = R.bSuccess; bAlready = R.Value.bAlreadyRegistered; });

		TestTrue(TEXT("success"), bSuccess);
		TestFalse(TEXT("not already registered"), bAlready);
		const FFlockHttpRequest& Request = F.Fake->Requests.Last();
		TestEqual(TEXT("url"), Request.Url, FString(TEXT("http://x/v1/player/register")));
		TestTrue(TEXT("email"), Request.JsonBody.Contains(TEXT("\"email\":\"a@b.c\"")));
		TestTrue(TEXT("name"), Request.JsonBody.Contains(TEXT("\"name\":\"Duck\"")));
		TestTrue(TEXT("session authenticated"), F.Session->IsAuthenticated());
		TestEqual(TEXT("method"), static_cast<int32>(F.Session->GetAuthMethod().GetValue()), static_cast<int32>(EFlockAuthMethod::Email));
		TestEqual(TEXT("authenticated event"), F.Listener->AuthenticatedCount, 1);
	}
	// Empty display name is omitted from the wire.
	{
		FProviderFixture F;
		F.Fake->On(TEXT("player/register"), FFlockFakeTransport::Ok(LoginBody(TEXT("p-1"), MakeJwt(TEXT("p-1")))));
		F.Provider->RegisterWithEmail(TEXT("a@b.c"), TEXT("pw"), TEXT(""), nullptr);
		TestFalse(TEXT("no empty name"), F.Fake->Requests.Last().JsonBody.Contains(TEXT("\"name\"")));
	}
	// Already-registered short-circuit: success with the flag; session stays signed out; no event.
	{
		FProviderFixture F;
		F.Fake->On(TEXT("register/device"), FFlockFakeTransport::Coded(409, TEXT("player.device_already_registered")));

		bool bSuccess = false;
		bool bAlready = false;
		F.Provider->RegisterWithDevice(TEXT("dev-1"), TEXT(""),
			[&](TFlockResult<FFlockRegisterResult> R) { bSuccess = R.bSuccess; bAlready = R.Value.bAlreadyRegistered; });

		TestTrue(TEXT("success"), bSuccess);
		TestTrue(TEXT("already registered"), bAlready);
		TestFalse(TEXT("still signed out"), F.Session->IsAuthenticated());
		TestEqual(TEXT("no event"), F.Listener->AuthenticatedCount, 0);

		// The outcome is a success, so nothing about it may be reported as an error. Both the retry
		// handler and the provider base used to log one here, which made a normal first-run path look
		// broken in the log.
		TestEqual(TEXT("no error logged for an expected outcome"), F.Log->Errors.Num(), 0);
		TestTrue(TEXT("reported as a warning instead"),
			FFlockRecordingLogger::AnyContains(F.Log->Warnings, TEXT("already registered")));
		TestTrue(TEXT("and still leaves a debug breadcrumb"),
			FFlockRecordingLogger::AnyContains(F.Log->Debugs, TEXT("Operation failed")));
	}
	// A taken display name is NOT the already-registered case — it must fail.
	{
		FProviderFixture F;
		F.Fake->On(TEXT("player/register"), FFlockFakeTransport::Coded(409, TEXT("player.name_already_registered")));
		bool bSuccess = true;
		F.Provider->RegisterWithEmail(TEXT("a@b.c"), TEXT("pw"), TEXT("Taken"),
			[&](TFlockResult<FFlockRegisterResult> R) { bSuccess = R.bSuccess; });
		TestFalse(TEXT("taken name fails"), bSuccess);
		// The suppression is scoped to the declared outcome — a neighbouring coded error is still an error.
		TestTrue(TEXT("a taken name is still logged as an error"), F.Log->Errors.Num() > 0);
	}
	// Other failures pass through.
	{
		FProviderFixture F;
		F.Fake->On(TEXT("player/register"), FFlockFakeTransport::Status(500, TEXT("")));
		EFlockErrorType Type = EFlockErrorType::None;
		F.Provider->RegisterWithEmail(TEXT("a@b.c"), TEXT("pw"), TEXT(""),
			[&](TFlockResult<FFlockRegisterResult> R) { Type = R.Error.Type; });
		TestEqual(TEXT("network error"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Network));
		TestTrue(TEXT("a genuine failure is still logged as an error"), F.Log->Errors.Num() > 0);
	}
	// Route matrix for the remaining register endpoints.
	{
		struct FCase
		{
			TFunction<void(FProviderFixture&)> Call;
			const TCHAR* Fragment;
			const TCHAR* UrlSuffix;
			const TCHAR* BodyFragment;
			EFlockAuthMethod Method;
		};
		const TArray<FCase> Cases = {
			{ [](FProviderFixture& F) { F.Provider->RegisterWithDevice(TEXT("dev-1"), TEXT("N"), nullptr); },
				TEXT("register/device"), TEXT("/player/register/device"), TEXT("\"device_id\":\"dev-1\""), EFlockAuthMethod::Device },
			{ [](FProviderFixture& F) { F.Provider->RegisterWithGoogle(TEXT("g-tok"), TEXT("N"), nullptr); },
				TEXT("register/google"), TEXT("/player/register/google"), TEXT("\"id_token\":\"g-tok\""), EFlockAuthMethod::Google },
			{ [](FProviderFixture& F) { F.Provider->RegisterWithApple(TEXT("a-tok"), TEXT("N"), nullptr); },
				TEXT("register/apple"), TEXT("/player/register/apple"), TEXT("\"identity_token\":\"a-tok\""), EFlockAuthMethod::Apple },
			{ [](FProviderFixture& F) { F.Provider->RegisterWithSteam(TEXT("s-tik"), TEXT("N"), nullptr); },
				TEXT("register/steam"), TEXT("/player/register/steam"), TEXT("\"session_ticket\":\"s-tik\""), EFlockAuthMethod::Steam },
		};
		for (const FCase& Case : Cases)
		{
			FProviderFixture F;
			F.Fake->On(Case.Fragment, FFlockFakeTransport::Ok(LoginBody(TEXT("p-1"), MakeJwt(TEXT("p-1")))));
			Case.Call(F);
			TestTrue(FString::Printf(TEXT("%s url"), Case.UrlSuffix), F.Fake->Requests.Last().Url.EndsWith(Case.UrlSuffix));
			TestTrue(FString::Printf(TEXT("%s body"), Case.BodyFragment), F.Fake->Requests.Last().JsonBody.Contains(Case.BodyFragment));
			TestTrue(TEXT("name in body"), F.Fake->Requests.Last().JsonBody.Contains(TEXT("\"name\":\"N\"")));
			TestEqual(TEXT("method recorded"), static_cast<int32>(F.Session->GetAuthMethod().GetValue()), static_cast<int32>(Case.Method));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAuthProviderRestoreTest, "Flock.Auth.Provider.Restore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAuthProviderRestoreTest::RunTest(const FString& Parameters)
{
	using namespace FlockAuthProviderTestHelpers;

	// Nothing stored -> false; OnSessionRestored(false) still fires; no OnAuthenticated.
	{
		FProviderFixture F;
		bool bRestored = true;
		F.Provider->TryRestoreSession([&](bool bOk) { bRestored = bOk; });
		TestFalse(TEXT("nothing to restore"), bRestored);
		TestEqual(TEXT("restored event fired"), F.Listener->SessionRestoredCount, 1);
		TestFalse(TEXT("restored=false payload"), F.Listener->bLastSessionRestored);
		TestEqual(TEXT("no auth event"), F.Listener->AuthenticatedCount, 0);
		TestFalse(TEXT("flag cleared"), F.Provider->IsRestoringSession());
	}
	// Valid unexpired tokens -> true; original method re-adopted; OnAuthenticated carries SessionRestore.
	{
		FProviderFixture F;
		F.Store->bHasTokens = true;
		F.Store->Stored.AccessToken = MakeJwt(TEXT("p-9"));
		F.Store->Stored.RefreshToken = TEXT("r-9");
		F.Store->Stored.AuthMethod = EFlockAuthMethod::Email;

		bool bRestored = false;
		F.Provider->TryRestoreSession([&](bool bOk) { bRestored = bOk; });

		TestTrue(TEXT("restored"), bRestored);
		TestTrue(TEXT("authenticated"), F.Session->IsAuthenticated());
		TestEqual(TEXT("player id"), F.Session->GetPlayerId(), FString(TEXT("p-9")));
		TestEqual(TEXT("original method re-adopted"), static_cast<int32>(F.Session->GetAuthMethod().GetValue()),
			static_cast<int32>(EFlockAuthMethod::Email));
		TestEqual(TEXT("auth event"), F.Listener->AuthenticatedCount, 1);
		TestEqual(TEXT("event method is SessionRestore"), static_cast<int32>(F.Listener->LastAuthInfo.Method),
			static_cast<int32>(EFlockAuthMethod::SessionRestore));
		TestTrue(TEXT("restored payload"), F.Listener->bLastSessionRestored);
		TestEqual(TEXT("no network"), F.Fake->Requests.Num(), 0);
	}
	// Stored method missing -> falls back to SessionRestore for the gate.
	{
		FProviderFixture F;
		F.Store->bHasTokens = true;
		F.Store->Stored.AccessToken = MakeJwt(TEXT("p-9"));
		F.Store->Stored.RefreshToken = TEXT("r-9");
		F.Provider->TryRestoreSession(nullptr);
		TestEqual(TEXT("fallback method"), static_cast<int32>(F.Session->GetAuthMethod().GetValue()),
			static_cast<int32>(EFlockAuthMethod::SessionRestore));
	}
	// Expired token -> refresh succeeds -> restored.
	{
		FProviderFixture F;
		F.Store->bHasTokens = true;
		F.Store->Stored.AccessToken = MakeJwt(TEXT("p-9"), /*ExpiryOffsetSeconds*/ -60);
		F.Store->Stored.RefreshToken = TEXT("r-9");
		const FString NewJwt = MakeJwt(TEXT("p-9"), 7200);
		F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Ok(LoginBody(TEXT("p-9"), NewJwt, TEXT("r-10"))));

		bool bRestored = false;
		F.Provider->TryRestoreSession([&](bool bOk) { bRestored = bOk; });

		TestTrue(TEXT("restored after refresh"), bRestored);
		TestEqual(TEXT("one refresh POST"), F.Fake->CountTo(TEXT("token/refresh")), 1);
		TestEqual(TEXT("new token adopted"), F.Session->GetAccessToken(), NewJwt);
		TestTrue(TEXT("restored payload"), F.Listener->bLastSessionRestored);
	}
	// Expired token -> refresh rejected -> not restored, signed out, OnAuthExpired raised.
	{
		FProviderFixture F;
		F.Store->bHasTokens = true;
		F.Store->Stored.AccessToken = MakeJwt(TEXT("p-9"), -60);
		F.Store->Stored.RefreshToken = TEXT("r-9");
		F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Status(401, TEXT("")));

		bool bRestored = true;
		F.Provider->TryRestoreSession([&](bool bOk) { bRestored = bOk; });

		TestFalse(TEXT("not restored"), bRestored);
		TestFalse(TEXT("signed out"), F.Session->IsAuthenticated());
		TestEqual(TEXT("auth expired"), F.Listener->AuthExpiredCount, 1);
		TestFalse(TEXT("restored payload false"), F.Listener->bLastSessionRestored);
		TestEqual(TEXT("no auth event"), F.Listener->AuthenticatedCount, 0);
	}
	// Unparseable stored tokens -> cleared, not restored.
	{
		FProviderFixture F;
		F.Store->bHasTokens = true;
		F.Store->Stored.AccessToken = TEXT("not-a-jwt");
		F.Store->Stored.RefreshToken = TEXT("r-9");

		bool bRestored = true;
		F.Provider->TryRestoreSession([&](bool bOk) { bRestored = bOk; });

		TestFalse(TEXT("not restored"), bRestored);
		TestEqual(TEXT("store cleared"), F.Store->ClearCount, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAuthProviderLogoutTest, "Flock.Auth.Provider.Logout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAuthProviderLogoutTest::RunTest(const FString& Parameters)
{
	using namespace FlockAuthProviderTestHelpers;
	FString Error;

	// Signed in -> logout clears everything and raises OnLoggedOut once.
	{
		FProviderFixture F;
		F.Session->SetTokens(MakeJwt(TEXT("p-1")), TEXT("r-1"), Error);
		F.Session->SetAuthMethod(EFlockAuthMethod::Email);

		F.Provider->Logout();

		TestFalse(TEXT("signed out"), F.Session->IsAuthenticated());
		TestFalse(TEXT("method cleared"), F.Session->GetAuthMethod().IsSet());
		TestFalse(TEXT("store cleared"), F.Store->bHasTokens);
		TestEqual(TEXT("logged-out event"), F.Listener->LoggedOutCount, 1);

		// Second logout is a safe no-op with no second event.
		F.Provider->Logout();
		TestEqual(TEXT("no double event"), F.Listener->LoggedOutCount, 1);
	}
	// Signed out -> no event.
	{
		FProviderFixture F;
		F.Provider->Logout();
		TestEqual(TEXT("no event when signed out"), F.Listener->LoggedOutCount, 0);
	}
	// RefreshToken passthrough completes false with no refresh token.
	{
		FProviderFixture F;
		bool bResult = true;
		F.Provider->RefreshToken([&](bool bOk) { bResult = bOk; });
		TestFalse(TEXT("no refresh token"), bResult);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAuthProviderAccountTest, "Flock.Auth.Provider.Account",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAuthProviderAccountTest::RunTest(const FString& Parameters)
{
	using namespace FlockAuthProviderTestHelpers;
	FString Error;

	// ForgotPassword: URL, body, success passthrough; empty email fails locally.
	{
		FProviderFixture F;
		F.Fake->On(TEXT("password/forgot"), FFlockFakeTransport::Ok(TEXT("{\"success\":true}")));
		bool bSuccess = false;
		bool bServerSuccess = false;
		F.Provider->ForgotPassword(TEXT("a@b.c"),
			[&](TFlockResult<FFlockAuthActionResponse> R) { bSuccess = R.bSuccess; bServerSuccess = R.Value.Success; });
		TestTrue(TEXT("success"), bSuccess);
		TestTrue(TEXT("server success"), bServerSuccess);
		TestTrue(TEXT("url"), F.Fake->Requests.Last().Url.EndsWith(TEXT("/player/password/forgot")));
		TestTrue(TEXT("email in body"), F.Fake->Requests.Last().JsonBody.Contains(TEXT("\"email\":\"a@b.c\"")));

		EFlockErrorType GuardType = EFlockErrorType::None;
		F.Provider->ForgotPassword(TEXT(""), [&](TFlockResult<FFlockAuthActionResponse> R) { GuardType = R.Error.Type; });
		TestEqual(TEXT("empty email -> validation"), static_cast<int32>(GuardType), static_cast<int32>(EFlockErrorType::Validation));
		TestEqual(TEXT("guard sent nothing"), F.Fake->CountTo(TEXT("password/forgot")), 1);
	}
	// ResetPassword: gated on an email-method session (restored email sessions count).
	{
		FProviderFixture F;

		// Signed out -> Auth error, no request.
		EFlockErrorType Type = EFlockErrorType::None;
		F.Provider->ResetPassword(TEXT("a@b.c"), TEXT("123"), TEXT("np"),
			[&](TFlockResult<FFlockAuthActionResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("signed out -> auth"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Auth));

		// Signed in via device -> still gated.
		F.Session->SetTokens(MakeJwt(TEXT("p-1")), TEXT("r-1"), Error);
		F.Session->SetAuthMethod(EFlockAuthMethod::Device);
		Type = EFlockErrorType::None;
		F.Provider->ResetPassword(TEXT("a@b.c"), TEXT("123"), TEXT("np"),
			[&](TFlockResult<FFlockAuthActionResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("device method -> auth"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Auth));
		TestEqual(TEXT("no request yet"), F.Fake->CountTo(TEXT("password/reset")), 0);

		// Email method -> posts all three fields.
		F.Session->SetAuthMethod(EFlockAuthMethod::Email);
		F.Fake->On(TEXT("password/reset"), FFlockFakeTransport::Ok(TEXT("{\"success\":true}")));
		bool bSuccess = false;
		F.Provider->ResetPassword(TEXT("a@b.c"), TEXT("123"), TEXT("np"),
			[&](TFlockResult<FFlockAuthActionResponse> R) { bSuccess = R.bSuccess; });
		TestTrue(TEXT("reset posted"), bSuccess);
		const FString& Body = F.Fake->Requests.Last().JsonBody;
		TestTrue(TEXT("code"), Body.Contains(TEXT("\"code\":\"123\"")));
		TestTrue(TEXT("new_password"), Body.Contains(TEXT("\"new_password\":\"np\"")));

		// Empty args are guarded.
		Type = EFlockErrorType::None;
		F.Provider->ResetPassword(TEXT("a@b.c"), TEXT(""), TEXT("np"),
			[&](TFlockResult<FFlockAuthActionResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("empty code -> validation"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Validation));
	}
	// SendEmailVerification: empty JSON body; bearer rides along when signed in.
	{
		FProviderFixture F;
		F.Session->SetTokens(MakeJwt(TEXT("p-1")), TEXT("r-1"), Error);
		F.Fake->On(TEXT("email/send-verification"), FFlockFakeTransport::Ok(TEXT("{\"success\":true}")));
		bool bSuccess = false;
		F.Provider->SendEmailVerification([&](TFlockResult<FFlockAuthActionResponse> R) { bSuccess = R.bSuccess; });
		TestTrue(TEXT("sent"), bSuccess);
		TestEqual(TEXT("empty body"), F.Fake->Requests.Last().JsonBody, FString(TEXT("{}")));
		TestTrue(TEXT("bearer sent"), F.Fake->Requests.Last().Headers.Contains(TEXT("Authorization")));
	}
	// VerifyEmail: code guard + body.
	{
		FProviderFixture F;
		F.Fake->On(TEXT("email/verify"), FFlockFakeTransport::Ok(TEXT("{\"success\":true}")));
		bool bSuccess = false;
		F.Provider->VerifyEmail(TEXT("999"), [&](TFlockResult<FFlockAuthActionResponse> R) { bSuccess = R.bSuccess; });
		TestTrue(TEXT("verified"), bSuccess);
		TestTrue(TEXT("code in body"), F.Fake->Requests.Last().JsonBody.Contains(TEXT("\"code\":\"999\"")));

		EFlockErrorType Type = EFlockErrorType::None;
		F.Provider->VerifyEmail(TEXT(""), [&](TFlockResult<FFlockAuthActionResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("empty code -> validation"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Validation));
	}
	// RevokeToken: auth guard; unconfirmed revoke is an error; confirmed passes.
	{
		FProviderFixture F;

		EFlockErrorType Type = EFlockErrorType::None;
		F.Provider->RevokeToken([&](TFlockResult<FFlockTokenRevokeResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("signed out -> auth"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Auth));
		TestEqual(TEXT("no request"), F.Fake->CountTo(TEXT("token/revoke")), 0);

		F.Session->SetTokens(MakeJwt(TEXT("p-1")), TEXT("r-1"), Error);
		F.Fake->On(TEXT("token/revoke"), FFlockFakeTransport::Ok(TEXT("{\"revoked\":false}")));
		Type = EFlockErrorType::None;
		F.Provider->RevokeToken([&](TFlockResult<FFlockTokenRevokeResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("unconfirmed -> auth error"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Auth));
		TestTrue(TEXT("local session untouched"), F.Session->IsAuthenticated());

		F.Fake->On(TEXT("token/revoke"), FFlockFakeTransport::Ok(TEXT("{\"revoked\":true}")));
		bool bSuccess = false;
		F.Provider->RevokeToken([&](TFlockResult<FFlockTokenRevokeResponse> R) { bSuccess = R.bSuccess; });
		TestTrue(TEXT("confirmed revoke"), bSuccess);
		TestEqual(TEXT("empty body"), F.Fake->Requests.Last().JsonBody, FString(TEXT("{}")));
	}
	// IsNameAvailable: GET with the encoded name in the query; response passthrough; guard.
	{
		FProviderFixture F;
		F.Fake->On(TEXT("name-available"), FFlockFakeTransport::Ok(TEXT("{\"name\":\"Duck Duck\",\"available\":true}")));
		bool bAvailable = false;
		F.Provider->IsNameAvailable(TEXT("Duck Duck"),
			[&](TFlockResult<FFlockNameAvailableResponse> R) { bAvailable = R.Value.Available; });
		TestTrue(TEXT("available"), bAvailable);
		const FFlockHttpRequest& Request = F.Fake->Requests.Last();
		TestEqual(TEXT("GET"), Request.Method, FString(TEXT("GET")));
		TestTrue(TEXT("encoded query"), Request.Url.Contains(TEXT("name-available?name=Duck%20Duck")));

		EFlockErrorType Type = EFlockErrorType::None;
		F.Provider->IsNameAvailable(TEXT(""), [&](TFlockResult<FFlockNameAvailableResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("empty name -> validation"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Validation));
	}
	// Account routes keep silent refresh: a 401 while signed in refreshes and replays once.
	{
		FProviderFixture F;
		F.Session->SetTokens(MakeJwt(TEXT("p-1")), TEXT("r-1"), Error);
		const FString NewJwt = MakeJwt(TEXT("p-1"), 7200);
		F.Fake->OnSequence(TEXT("email/send-verification"), { FFlockFakeTransport::Status(401, TEXT("")),
			FFlockFakeTransport::Ok(TEXT("{\"success\":true}")) });
		F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Ok(LoginBody(TEXT("p-1"), NewJwt, TEXT("r-2"))));

		bool bSuccess = false;
		F.Provider->SendEmailVerification([&](TFlockResult<FFlockAuthActionResponse> R) { bSuccess = R.bSuccess; });
		TestTrue(TEXT("replay succeeded"), bSuccess);
		TestEqual(TEXT("two attempts"), F.Fake->CountTo(TEXT("email/send-verification")), 2);
		TestEqual(TEXT("one refresh"), F.Fake->CountTo(TEXT("token/refresh")), 1);
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
