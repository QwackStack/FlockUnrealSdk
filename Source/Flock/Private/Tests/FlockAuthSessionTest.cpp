// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "FlockEvents.h"
#include "FlockLogger.h"
#include "Http/FlockHttpClient.h"
#include "Misc/Base64.h"
#include "Tests/Support/FlockEventTestListener.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"

namespace FlockAuthSessionTestHelpers
{
	inline FString Base64Url(const FString& In)
	{
		FString Encoded = FBase64::Encode(In);
		Encoded.ReplaceInline(TEXT("+"), TEXT("-"));
		Encoded.ReplaceInline(TEXT("/"), TEXT("_"));
		Encoded.ReplaceInline(TEXT("="), TEXT(""));
		return Encoded;
	}

	/** JWT with the given player id, expiring OffsetSeconds from now (negative = already expired). */
	inline FString MakeJwt(const FString& PlayerId, int64 ExpiryOffsetSeconds = 3600)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + ExpiryOffsetSeconds;
		const FString Payload = FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp);
		return FString::Printf(TEXT("h.%s.s"), *Base64Url(Payload));
	}

	struct FSessionFixture
	{
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockAuthSession> Session;
		UFlockEvents* Events = nullptr;
		UFlockEventTestListener* Listener = nullptr;

		FSessionFixture()
			: Session(MakeShared<FFlockAuthSession>(
				MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()),
				Store, MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
		{
			Events = NewObject<UFlockEvents>();
			Listener = NewObject<UFlockEventTestListener>();
			Events->OnTokenRefreshed.AddDynamic(Listener, &UFlockEventTestListener::HandleTokenRefreshed);
			Events->OnAuthExpired.AddDynamic(Listener, &UFlockEventTestListener::HandleAuthExpired);
			Session->SetEvents(Events);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAuthSessionStateTest, "Flock.Auth.Session.State",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAuthSessionStateTest::RunTest(const FString& Parameters)
{
	using namespace FlockAuthSessionTestHelpers;
	FSessionFixture F;

	TestFalse(TEXT("starts signed out"), F.Session->IsAuthenticated());
	TestTrue(TEXT("player id empty"), F.Session->GetPlayerId().IsEmpty());

	FString Error;
	TestTrue(TEXT("set tokens"), F.Session->SetTokens(MakeJwt(TEXT("p-1")), TEXT("r-1"), Error));
	TestTrue(TEXT("authenticated"), F.Session->IsAuthenticated());
	TestFalse(TEXT("not expired"), F.Session->IsTokenExpired());
	TestEqual(TEXT("player id"), F.Session->GetPlayerId(), FString(TEXT("p-1")));
	TestTrue(TEXT("persisted"), F.Store->bHasTokens);
	TestEqual(TEXT("persisted access"), F.Store->Stored.AccessToken, F.Session->GetAccessToken());

	// Auth method persists alongside.
	F.Session->SetAuthMethod(EFlockAuthMethod::Email);
	TestTrue(TEXT("method stored"), F.Store->Stored.AuthMethod.IsSet());

	// Bearer header appears only when signed in.
	TMap<FString, FString> Headers = F.Session->GetAuthHeaders();
	TestTrue(TEXT("base header kept"), Headers.Contains(TEXT("X-Flock-API-Key")));
	TestTrue(TEXT("bearer added"), Headers.Contains(TEXT("Authorization")));
	TestTrue(TEXT("bearer format"), Headers[TEXT("Authorization")].StartsWith(TEXT("Bearer ")));

	// Unparseable JWT rejects and leaves state untouched.
	TestFalse(TEXT("bad jwt rejected"), F.Session->SetTokens(TEXT("not-a-jwt"), TEXT("r-2"), Error));
	TestFalse(TEXT("error message set"), Error.IsEmpty());
	TestEqual(TEXT("state untouched"), F.Session->GetPlayerId(), FString(TEXT("p-1")));

	// Expired token detection.
	TestTrue(TEXT("set expired"), F.Session->SetTokens(MakeJwt(TEXT("p-1"), -60), TEXT("r-1"), Error));
	TestTrue(TEXT("expired"), F.Session->IsTokenExpired());

	// Clear wipes memory + store + method.
	F.Session->ClearTokens();
	TestFalse(TEXT("signed out"), F.Session->IsAuthenticated());
	TestFalse(TEXT("store cleared"), F.Store->bHasTokens);
	TestFalse(TEXT("method cleared"), F.Session->GetAuthMethod().IsSet());
	TestFalse(TEXT("no bearer"), F.Session->GetAuthHeaders().Contains(TEXT("Authorization")));

	// Store failures are non-fatal.
	F.Store->bFailSave = true;
	TestTrue(TEXT("set with failing store"), F.Session->SetTokens(MakeJwt(TEXT("p-2")), TEXT("r"), Error));
	TestTrue(TEXT("still authenticated"), F.Session->IsAuthenticated());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAuthSessionRefreshTest, "Flock.Auth.Session.Refresh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAuthSessionRefreshTest::RunTest(const FString& Parameters)
{
	using namespace FlockAuthSessionTestHelpers;
	FString Error;

	// No refresh token -> completes false, no network.
	{
		FSessionFixture F;
		bool bResult = true;
		F.Session->RefreshAccessToken([&](bool bOk) { bResult = bOk; });
		TestFalse(TEXT("no refresh token -> false"), bResult);
		TestEqual(TEXT("no request"), F.Fake->CountTo(TEXT("token/refresh")), 0);
	}
	// Success: tokens update, OnTokenRefreshed raised, request carries player_id + refresh_token
	// with base headers (no bearer).
	{
		FSessionFixture F;
		F.Session->SetTokens(MakeJwt(TEXT("p-1")), TEXT("r-1"), Error);
		const FString NewJwt = MakeJwt(TEXT("p-1"), 7200);
		F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Ok(FString::Printf(
			TEXT("{\"player_id\":\"p-1\",\"access_token\":\"%s\",\"refresh_token\":\"r-2\"}"), *NewJwt)));

		bool bResult = false;
		F.Session->RefreshAccessToken([&](bool bOk) { bResult = bOk; });
		TestTrue(TEXT("refresh succeeds"), bResult);
		TestEqual(TEXT("token replaced"), F.Session->GetAccessToken(), NewJwt);
		TestEqual(TEXT("refresh token replaced"), F.Session->GetRefreshTokenValue(), FString(TEXT("r-2")));
		TestEqual(TEXT("event raised"), F.Listener->TokenRefreshedCount, 1);
		TestEqual(TEXT("no auth-expired"), F.Listener->AuthExpiredCount, 0);
		const FFlockHttpRequest& Request = F.Fake->Requests.Last();
		TestTrue(TEXT("body has refresh token"), Request.JsonBody.Contains(TEXT("\"refresh_token\":\"r-1\"")));
		TestTrue(TEXT("body has player id"), Request.JsonBody.Contains(TEXT("\"player_id\":\"p-1\"")));
		TestFalse(TEXT("no bearer on refresh"), Request.Headers.Contains(TEXT("Authorization")));
	}
	// Server rejection (401) -> tokens cleared, OnAuthExpired raised.
	{
		FSessionFixture F;
		F.Session->SetTokens(MakeJwt(TEXT("p-1")), TEXT("r-1"), Error);
		F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Status(401, TEXT("")));
		bool bResult = true;
		F.Session->RefreshAccessToken([&](bool bOk) { bResult = bOk; });
		TestFalse(TEXT("rejected"), bResult);
		TestFalse(TEXT("signed out"), F.Session->IsAuthenticated());
		TestEqual(TEXT("auth-expired raised"), F.Listener->AuthExpiredCount, 1);
		TestFalse(TEXT("store cleared"), F.Store->bHasTokens);
	}
	// Transport failure -> tokens kept (flaky network must not sign the player out).
	{
		FSessionFixture F;
		F.Session->SetTokens(MakeJwt(TEXT("p-1")), TEXT("r-1"), Error);
		F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Offline());
		bool bResult = true;
		F.Session->RefreshAccessToken([&](bool bOk) { bResult = bOk; });
		TestFalse(TEXT("failed"), bResult);
		TestTrue(TEXT("still signed in"), F.Session->IsAuthenticated());
		TestEqual(TEXT("no auth-expired"), F.Listener->AuthExpiredCount, 0);
	}
	// Success with an empty access token -> treated as a rejection.
	{
		FSessionFixture F;
		F.Session->SetTokens(MakeJwt(TEXT("p-1")), TEXT("r-1"), Error);
		F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Ok(TEXT("{\"player_id\":\"p-1\",\"access_token\":\"\"}")));
		bool bResult = true;
		F.Session->RefreshAccessToken([&](bool bOk) { bResult = bOk; });
		TestFalse(TEXT("empty token -> failed"), bResult);
		TestFalse(TEXT("signed out"), F.Session->IsAuthenticated());
		TestEqual(TEXT("auth-expired raised"), F.Listener->AuthExpiredCount, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAuthSessionRefreshSingleFlightTest, "Flock.Auth.Session.RefreshSingleFlight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAuthSessionRefreshSingleFlightTest::RunTest(const FString& Parameters)
{
	using namespace FlockAuthSessionTestHelpers;
	FString Error;

	// The fake transport completes synchronously; a deferring adapter lets two callers overlap.
	class FDeferringTransport : public IFlockHttpAdapter
	{
	public:
		virtual FFlockRequestHandle SendAsync(const FFlockHttpRequest& Request, TFunction<void(FFlockHttpResponse)> OnComplete) override
		{
			++RequestCount;
			Pending = MoveTemp(OnComplete);
			return FFlockRequestHandle();
		}
		void Complete(const FFlockHttpResponse& Response)
		{
			TFunction<void(FFlockHttpResponse)> Callback = MoveTemp(Pending);
			Pending = nullptr;
			if (Callback) { Callback(Response); }
		}
		int32 RequestCount = 0;
		TFunction<void(FFlockHttpResponse)> Pending;
	};

	const TSharedRef<FDeferringTransport> Deferring = MakeShared<FDeferringTransport>();
	const TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
	const TSharedRef<FFlockAuthSession> Session = MakeShared<FFlockAuthSession>(
		MakeShared<FFlockHttpClient>(Deferring, MakeShared<FFlockNullLogger>()),
		Store, MakeShared<FFlockNullLogger>(), TEXT("http://x/v1"), TMap<FString, FString>());
	Session->SetTokens(MakeJwt(TEXT("p-1")), TEXT("r-1"), Error);

	int32 FirstResult = -1;
	int32 SecondResult = -1;
	Session->RefreshAccessToken([&](bool bOk) { FirstResult = bOk ? 1 : 0; });
	Session->RefreshAccessToken([&](bool bOk) { SecondResult = bOk ? 1 : 0; });

	TestEqual(TEXT("one POST for two callers"), Deferring->RequestCount, 1);
	TestEqual(TEXT("first pending"), FirstResult, -1);
	TestEqual(TEXT("second pending"), SecondResult, -1);

	const FString NewJwt = MakeJwt(TEXT("p-1"), 7200);
	FFlockHttpResponse Response;
	Response.Result = EFlockHttpResult::Success;
	Response.StatusCode = 200;
	Response.Body = FString::Printf(TEXT("{\"player_id\":\"p-1\",\"access_token\":\"%s\",\"refresh_token\":\"r-2\"}"), *NewJwt);
	Deferring->Complete(Response);

	TestEqual(TEXT("first completed true"), FirstResult, 1);
	TestEqual(TEXT("second completed true"), SecondResult, 1);
	TestEqual(TEXT("still one POST"), Deferring->RequestCount, 1);

	// A caller after completion starts a fresh POST.
	Session->RefreshAccessToken([](bool) {});
	TestEqual(TEXT("new refresh -> new POST"), Deferring->RequestCount, 2);
	Deferring->Complete(Response);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
