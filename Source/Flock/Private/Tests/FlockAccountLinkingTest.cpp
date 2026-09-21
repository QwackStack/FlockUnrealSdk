// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "FlockEvents.h"
#include "FlockLogger.h"
#include "Http/FlockHttpClient.h"
#include "Misc/Base64.h"
#include "Models/FlockAuthModels.h"
#include "Providers/FlockAuthProvider.h"
#include "Tests/Support/FlockEventTestListener.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockRecordingLogger.h"

namespace FlockAccountLinkingTestHelpers
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

	/** Accounts body as the backend really sends it: the model at the root, NOT in a GenericResponse envelope. */
	inline FString AccountsBody(const FString& AccountsJson)
	{
		return FString::Printf(TEXT("{\"accounts\":[%s]}"), *AccountsJson);
	}

	inline FString Account(const FString& Provider, const FString& UserId, const FString& Email = FString(), bool bVerified = false)
	{
		return FString::Printf(TEXT("{\"provider\":\"%s\",\"provider_user_id\":\"%s\",\"email\":\"%s\",\"email_verified\":%s}"),
			*Provider, *UserId, *Email, bVerified ? TEXT("true") : TEXT("false"));
	}

	inline FFlockRetryPolicy NoRetryPolicy()
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = 0;
		return Policy;
	}

	struct FFixture
	{
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockAuthSession> Session;
		UFlockEvents* Events = nullptr;
		UFlockEventTestListener* Listener = nullptr;
		TUniquePtr<FFlockAuthProvider> Provider;
		TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();

		FFixture()
			: Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
		{
			Events = NewObject<UFlockEvents>();
			Listener = NewObject<UFlockEventTestListener>();
			Events->OnAccountLinked.AddDynamic(Listener, &UFlockEventTestListener::HandleAccountLinked);
			Events->OnAccountUnlinked.AddDynamic(Listener, &UFlockEventTestListener::HandleAccountUnlinked);
			Session->SetEvents(Events);
			Provider = MakeUnique<FFlockAuthProvider>(Client, NoRetryPolicy(), Log, Session, Events, TEXT("http://x/v1"));
		}

		/** Signs in without going through the network, so linking tests start from a real bearer. */
		void SignIn(const FString& PlayerId = TEXT("p-1"))
		{
			FString Error;
			Session->SetTokens(MakeJwt(PlayerId), TEXT("r-1"), Error);
			Session->SetAuthMethod(EFlockAuthMethod::Device);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAccountLinkingWireTest, "Flock.Auth.Linking.Wire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAccountLinkingWireTest::RunTest(const FString& Parameters)
{
	using namespace FlockAccountLinkingTestHelpers;

	// Root-shaped body parses, and each account gets its typed ProviderType filled in.
	{
		FFixture F;
		F.SignIn();
		F.Fake->On(TEXT("player/accounts"), FFlockFakeTransport::Ok(
			AccountsBody(Account(TEXT("email"), TEXT("u-1"), TEXT("a@b.c"), true) + TEXT(",") + Account(TEXT("device_id"), TEXT("d-1")))));

		TFlockResult<FFlockPlayerAccountsResponse> Got = TFlockResult<FFlockPlayerAccountsResponse>::Fail(FFlockError());
		F.Provider->GetLinkedAccounts([&](TFlockResult<FFlockPlayerAccountsResponse> R) { Got = R; });

		TestTrue(TEXT("read succeeded"), Got.bSuccess);
		TestEqual(TEXT("two accounts"), Got.Value.Accounts.Num(), 2);
		TestEqual(TEXT("email provider typed"), static_cast<int32>(Got.Value.Accounts[0].ProviderType),
			static_cast<int32>(EFlockCredentialProvider::Email));
		TestEqual(TEXT("email address parsed"), Got.Value.Accounts[0].Email, FString(TEXT("a@b.c")));
		TestTrue(TEXT("email_verified parsed"), Got.Value.Accounts[0].EmailVerified);
		TestEqual(TEXT("device_id provider typed"), static_cast<int32>(Got.Value.Accounts[1].ProviderType),
			static_cast<int32>(EFlockCredentialProvider::DeviceId));
		TestEqual(TEXT("provider_user_id parsed"), Got.Value.Accounts[1].ProviderUserId, FString(TEXT("d-1")));
	}
	// An enveloped body must NOT parse — this route is bare. Fails if anyone re-wraps it in GenericResponse.
	{
		FFixture F;
		F.SignIn();
		F.Fake->On(TEXT("player/accounts"), FFlockFakeTransport::Ok(
			FString::Printf(TEXT("{\"error\":false,\"response\":\"ok\",\"result\":%s}"), *AccountsBody(Account(TEXT("email"), TEXT("u-1"))))));

		TFlockResult<FFlockPlayerAccountsResponse> Got = TFlockResult<FFlockPlayerAccountsResponse>::Fail(FFlockError());
		F.Provider->GetLinkedAccounts([&](TFlockResult<FFlockPlayerAccountsResponse> R) { Got = R; });

		TestEqual(TEXT("enveloped body yields no accounts"), Got.Value.Accounts.Num(), 0);
	}
	// A provider this SDK version predates parses as Unknown rather than failing the whole list.
	{
		FFixture F;
		F.SignIn();
		F.Fake->On(TEXT("player/accounts"), FFlockFakeTransport::Ok(AccountsBody(Account(TEXT("nintendo"), TEXT("n-1")))));

		TFlockResult<FFlockPlayerAccountsResponse> Got = TFlockResult<FFlockPlayerAccountsResponse>::Fail(FFlockError());
		F.Provider->GetLinkedAccounts([&](TFlockResult<FFlockPlayerAccountsResponse> R) { Got = R; });

		TestTrue(TEXT("read succeeded"), Got.bSuccess);
		TestEqual(TEXT("unknown provider typed as Unknown"), static_cast<int32>(Got.Value.Accounts[0].ProviderType),
			static_cast<int32>(EFlockCredentialProvider::Unknown));
		TestEqual(TEXT("raw wire spelling kept"), Got.Value.Accounts[0].Provider, FString(TEXT("nintendo")));
	}
	// Wire mapping round-trips, and Unknown has no sendable spelling.
	{
		TestEqualSensitive(TEXT("device_id is the one non-lowercase-name mapping"),
			FFlockCredentialProviders::ToWire(EFlockCredentialProvider::DeviceId), FString(TEXT("device_id")));
		TestEqual(TEXT("Unknown has no wire value"),
			FFlockCredentialProviders::ToWire(EFlockCredentialProvider::Unknown), FString());
		TestEqual(TEXT("parse is case-insensitive"), static_cast<int32>(FFlockCredentialProviders::Parse(TEXT("Google"))),
			static_cast<int32>(EFlockCredentialProvider::Google));
		TestEqual(TEXT("empty parses as Unknown"), static_cast<int32>(FFlockCredentialProviders::Parse(FString())),
			static_cast<int32>(EFlockCredentialProvider::Unknown));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAccountLinkingRequestShapeTest, "Flock.Auth.Linking.RequestShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAccountLinkingRequestShapeTest::RunTest(const FString& Parameters)
{
	using namespace FlockAccountLinkingTestHelpers;

	// Email link posts email + password, and carries the bearer.
	{
		FFixture F;
		F.SignIn();
		F.Fake->On(TEXT("player/link/email"), FFlockFakeTransport::Ok(AccountsBody(Account(TEXT("email"), TEXT("u-1")))));
		F.Provider->LinkEmail(TEXT("a@b.c"), TEXT("pw"), nullptr);

		TestEqual(TEXT("one request"), F.Fake->Requests.Num(), 1);
		const FFlockHttpRequest& Sent = F.Fake->Requests.Last();
		TestEqual(TEXT("POST"), Sent.Method, FString(TEXT("POST")));
		TestTrue(TEXT("email on the wire"), Sent.JsonBody.Contains(TEXT("\"email\":\"a@b.c\""), ESearchCase::CaseSensitive));
		TestTrue(TEXT("password on the wire"), Sent.JsonBody.Contains(TEXT("\"password\":\"pw\""), ESearchCase::CaseSensitive));
		TestTrue(TEXT("Authorization sent"), Sent.Headers.Contains(TEXT("Authorization")));
	}
	// Device link snake_cases both fields.
	{
		FFixture F;
		F.SignIn();
		F.Fake->On(TEXT("player/link/device"), FFlockFakeTransport::Ok(AccountsBody(Account(TEXT("device_id"), TEXT("d-1")))));
		F.Provider->LinkDevice(TEXT("dev-9"), nullptr);

		const FFlockHttpRequest& Sent = F.Fake->Requests.Last();
		TestTrue(TEXT("device_id snake_cased"), Sent.JsonBody.Contains(TEXT("\"device_id\":\"dev-9\""), ESearchCase::CaseSensitive));
		TestTrue(TEXT("device_type sent"), Sent.JsonBody.Contains(TEXT("\"device_type\""), ESearchCase::CaseSensitive));
	}
	// Every OAuth provider posts a BARE {token} to its own route — not the login routes' id_token/identity_token/session_ticket.
	{
		struct FCase { EFlockCredentialProvider Provider; const TCHAR* Wire; };
		const FCase Cases[] = {
			{ EFlockCredentialProvider::Google, TEXT("google") },
			{ EFlockCredentialProvider::Apple, TEXT("apple") },
			{ EFlockCredentialProvider::Steam, TEXT("steam") },
			{ EFlockCredentialProvider::Facebook, TEXT("facebook") },
			{ EFlockCredentialProvider::Discord, TEXT("discord") },
		};
		for (const FCase& Case : Cases)
		{
			FFixture F;
			F.SignIn();
			F.Fake->On(TEXT("player/link/oauth"), FFlockFakeTransport::Ok(AccountsBody(Account(Case.Wire, TEXT("o-1")))));
			switch (Case.Provider)
			{
			case EFlockCredentialProvider::Google: F.Provider->LinkGoogle(TEXT("tok"), nullptr); break;
			case EFlockCredentialProvider::Apple: F.Provider->LinkApple(TEXT("tok"), nullptr); break;
			case EFlockCredentialProvider::Steam: F.Provider->LinkSteam(TEXT("tok"), nullptr); break;
			case EFlockCredentialProvider::Facebook: F.Provider->LinkFacebook(TEXT("tok"), nullptr); break;
			default: F.Provider->LinkDiscord(TEXT("tok"), nullptr); break;
			}

			const FFlockHttpRequest& Sent = F.Fake->Requests.Last();
			TestTrue(FString::Printf(TEXT("%s hits its own route"), Case.Wire),
				Sent.Url.EndsWith(FString::Printf(TEXT("player/link/oauth/%s"), Case.Wire)));
			TestTrue(FString::Printf(TEXT("%s posts a bare token"), Case.Wire), Sent.JsonBody.Contains(TEXT("\"token\":\"tok\""), ESearchCase::CaseSensitive));
			TestFalse(FString::Printf(TEXT("%s does not send id_token"), Case.Wire), Sent.JsonBody.Contains(TEXT("id_token")));
			TestFalse(FString::Printf(TEXT("%s does not send session_ticket"), Case.Wire), Sent.JsonBody.Contains(TEXT("session_ticket")));
		}
	}
	// Unlink puts the wire spelling in the path segment.
	{
		FFixture F;
		F.SignIn();
		F.Fake->On(TEXT("player/unlink"), FFlockFakeTransport::Ok(AccountsBody(Account(TEXT("email"), TEXT("u-1")))));
		F.Provider->Unlink(EFlockCredentialProvider::DeviceId, nullptr);

		const FFlockHttpRequest& Sent = F.Fake->Requests.Last();
		TestTrue(TEXT("device_id segment, not DeviceId"), Sent.Url.EndsWith(TEXT("player/unlink/device_id")));
		TestEqual(TEXT("POST"), Sent.Method, FString(TEXT("POST")));
		TestTrue(TEXT("Authorization sent"), Sent.Headers.Contains(TEXT("Authorization")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAccountLinkingGuardTest, "Flock.Auth.Linking.Guards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAccountLinkingGuardTest::RunTest(const FString& Parameters)
{
	using namespace FlockAccountLinkingTestHelpers;

	// Signed out: every linking call fails fast without touching the network.
	{
		FFixture F;
		int32 Failures = 0;
		TFunction<void(TFlockResult<FFlockPlayerAccountsResponse>)> Count =
			[&](TFlockResult<FFlockPlayerAccountsResponse> R)
			{
				if (!R.bSuccess && R.Error.Type == EFlockErrorType::Auth) { ++Failures; }
			};

		F.Provider->GetLinkedAccounts(Count);
		F.Provider->LinkEmail(TEXT("a@b.c"), TEXT("pw"), Count);
		F.Provider->LinkDevice(TEXT("d"), Count);
		F.Provider->LinkGoogle(TEXT("t"), Count);
		F.Provider->LinkApple(TEXT("t"), Count);
		F.Provider->LinkSteam(TEXT("t"), Count);
		F.Provider->LinkFacebook(TEXT("t"), Count);
		F.Provider->LinkDiscord(TEXT("t"), Count);
		F.Provider->Unlink(EFlockCredentialProvider::Email, Count);

		TestEqual(TEXT("all nine failed with an Auth error"), Failures, 9);
		TestEqual(TEXT("no network touched"), F.Fake->Requests.Num(), 0);
	}
	// Empty arguments are a Validation failure, not a request.
	{
		FFixture F;
		F.SignIn();
		int32 Failures = 0;
		TFunction<void(TFlockResult<FFlockPlayerAccountsResponse>)> Count =
			[&](TFlockResult<FFlockPlayerAccountsResponse> R)
			{
				if (!R.bSuccess && R.Error.Type == EFlockErrorType::Validation) { ++Failures; }
			};

		F.Provider->LinkEmail(FString(), TEXT("pw"), Count);
		F.Provider->LinkEmail(TEXT("a@b.c"), FString(), Count);
		F.Provider->LinkDevice(FString(), Count);
		F.Provider->LinkGoogle(FString(), Count);

		TestEqual(TEXT("four validation failures"), Failures, 4);
		TestEqual(TEXT("no network touched"), F.Fake->Requests.Num(), 0);
	}
	// Unknown is not a sendable provider — it must never build a URL with an empty segment.
	// Unlink is the only public entry that takes one; the link side hardcodes its provider per method.
	{
		FFixture F;
		F.SignIn();
		EFlockErrorType UnlinkType = EFlockErrorType::None;
		F.Provider->Unlink(EFlockCredentialProvider::Unknown,
			[&](TFlockResult<FFlockPlayerAccountsResponse> R) { UnlinkType = R.Error.Type; });

		TestEqual(TEXT("unlink rejects Unknown"), static_cast<int32>(UnlinkType), static_cast<int32>(EFlockErrorType::Validation));
		TestEqual(TEXT("no network touched"), F.Fake->Requests.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAccountLinkingEventTest, "Flock.Auth.Linking.Events",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAccountLinkingEventTest::RunTest(const FString& Parameters)
{
	using namespace FlockAccountLinkingTestHelpers;

	// A successful link raises OnAccountLinked with the provider that was linked.
	{
		FFixture F;
		F.SignIn();
		F.Fake->On(TEXT("player/link/email"), FFlockFakeTransport::Ok(AccountsBody(Account(TEXT("email"), TEXT("u-1")))));
		F.Provider->LinkEmail(TEXT("a@b.c"), TEXT("pw"), nullptr);

		TestEqual(TEXT("linked once"), F.Listener->AccountLinkedCount, 1);
		TestEqual(TEXT("carries the provider"), static_cast<int32>(F.Listener->LastLinkedProvider),
			static_cast<int32>(EFlockCredentialProvider::Email));
		TestEqual(TEXT("no unlink raised"), F.Listener->AccountUnlinkedCount, 0);
	}
	// A successful unlink raises OnAccountUnlinked.
	{
		FFixture F;
		F.SignIn();
		F.Fake->On(TEXT("player/unlink"), FFlockFakeTransport::Ok(AccountsBody(Account(TEXT("email"), TEXT("u-1")))));
		F.Provider->Unlink(EFlockCredentialProvider::Steam, nullptr);

		TestEqual(TEXT("unlinked once"), F.Listener->AccountUnlinkedCount, 1);
		TestEqual(TEXT("carries the provider"), static_cast<int32>(F.Listener->LastUnlinkedProvider),
			static_cast<int32>(EFlockCredentialProvider::Steam));
		TestEqual(TEXT("no link raised"), F.Listener->AccountLinkedCount, 0);
	}
	// A failed link raises nothing.
	{
		FFixture F;
		F.SignIn();
		F.Fake->On(TEXT("player/link/email"), FFlockFakeTransport::Coded(409, TEXT("player.account_already_linked")));
		F.Provider->LinkEmail(TEXT("a@b.c"), TEXT("pw"), nullptr);

		TestEqual(TEXT("no link event on failure"), F.Listener->AccountLinkedCount, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAccountLinkingCodedErrorTest, "Flock.Auth.Linking.CodedErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAccountLinkingCodedErrorTest::RunTest(const FString& Parameters)
{
	using namespace FlockAccountLinkingTestHelpers;

	struct FCase
	{
		const TCHAR* Route;
		int32 Status;
		const TCHAR* Wire;
		EFlockErrorCode Expected;
	};
	const FCase Cases[] = {
		{ TEXT("player/link/email"), 409, TEXT("player.account_already_linked"), EFlockErrorCode::PlayerAccountAlreadyLinked },
		{ TEXT("player/link/email"), 400, TEXT("player.invalid_link_request"), EFlockErrorCode::PlayerInvalidLinkRequest },
		{ TEXT("player/unlink"), 400, TEXT("player.cannot_unlink_last_credential"), EFlockErrorCode::PlayerCannotUnlinkLastCredential },
		{ TEXT("player/unlink"), 404, TEXT("player.account_not_linked"), EFlockErrorCode::PlayerAccountNotLinked },
	};

	for (const FCase& Case : Cases)
	{
		FFixture F;
		F.SignIn();
		F.Fake->On(Case.Route, FFlockFakeTransport::Coded(Case.Status, Case.Wire));

		TFlockResult<FFlockPlayerAccountsResponse> Got = TFlockResult<FFlockPlayerAccountsResponse>::Fail(FFlockError());
		if (FString(Case.Route).Contains(TEXT("unlink")))
		{
			F.Provider->Unlink(EFlockCredentialProvider::Email, [&](TFlockResult<FFlockPlayerAccountsResponse> R) { Got = R; });
		}
		else
		{
			F.Provider->LinkEmail(TEXT("a@b.c"), TEXT("pw"), [&](TFlockResult<FFlockPlayerAccountsResponse> R) { Got = R; });
		}

		TestFalse(FString::Printf(TEXT("%s failed"), Case.Wire), Got.bSuccess);
		TestEqual(FString::Printf(TEXT("%s kept its raw wire code"), Case.Wire), Got.Error.Code, FString(Case.Wire));
		TestEqual(FString::Printf(TEXT("%s parsed to its typed code"), Case.Wire),
			static_cast<int32>(Got.Error.ErrorCode), static_cast<int32>(Case.Expected));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAccountLinkingResetGateTest, "Flock.Auth.Linking.PasswordResetGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAccountLinkingResetGateTest::RunTest(const FString& Parameters)
{
	using namespace FlockAccountLinkingTestHelpers;

	// Baseline: a device session cannot reset a password.
	{
		FFixture F;
		F.SignIn();
		EFlockErrorType Type = EFlockErrorType::None;
		F.Provider->ResetPassword(TEXT("a@b.c"), TEXT("code"), TEXT("new"),
			[&](TFlockResult<FFlockAuthActionResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("gated on a device session"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Auth));
		TestEqual(TEXT("no network touched"), F.Fake->Requests.Num(), 0);
	}
	// Linking an email during a device session opens the gate.
	{
		FFixture F;
		F.SignIn();
		F.Fake->On(TEXT("player/link/email"), FFlockFakeTransport::Ok(AccountsBody(Account(TEXT("email"), TEXT("u-1")))));
		F.Provider->LinkEmail(TEXT("a@b.c"), TEXT("pw"), nullptr);

		F.Fake->On(TEXT("player/password/reset"), FFlockFakeTransport::Ok(TEXT("{\"success\":true}")));
		bool bSuccess = false;
		F.Provider->ResetPassword(TEXT("a@b.c"), TEXT("code"), TEXT("new"),
			[&](TFlockResult<FFlockAuthActionResponse> R) { bSuccess = R.bSuccess; });
		TestTrue(TEXT("reset allowed after linking an email"), bSuccess);
	}
	// Reading a list with no email credential leaves the gate closed.
	{
		FFixture F;
		F.SignIn();
		F.Fake->On(TEXT("player/accounts"), FFlockFakeTransport::Ok(AccountsBody(Account(TEXT("device_id"), TEXT("d-1")))));
		F.Provider->GetLinkedAccounts(nullptr);

		EFlockErrorType Type = EFlockErrorType::None;
		F.Provider->ResetPassword(TEXT("a@b.c"), TEXT("code"), TEXT("new"),
			[&](TFlockResult<FFlockAuthActionResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("still gated"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Auth));
	}
	// Unlinking the email closes the gate again — the flag re-derives from the returned list.
	{
		FFixture F;
		F.SignIn();
		F.Fake->On(TEXT("player/link/email"), FFlockFakeTransport::Ok(AccountsBody(Account(TEXT("email"), TEXT("u-1")))));
		F.Provider->LinkEmail(TEXT("a@b.c"), TEXT("pw"), nullptr);

		F.Fake->On(TEXT("player/unlink"), FFlockFakeTransport::Ok(AccountsBody(Account(TEXT("device_id"), TEXT("d-1")))));
		F.Provider->Unlink(EFlockCredentialProvider::Email, nullptr);

		EFlockErrorType Type = EFlockErrorType::None;
		F.Provider->ResetPassword(TEXT("a@b.c"), TEXT("code"), TEXT("new"),
			[&](TFlockResult<FFlockAuthActionResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("re-gated after unlink"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Auth));
	}
	// Cross-player trap: a new sign-in without an intervening logout must not inherit the previous player's answer.
	{
		FFixture F;
		F.SignIn(TEXT("p-1"));
		F.Fake->On(TEXT("player/link/email"), FFlockFakeTransport::Ok(AccountsBody(Account(TEXT("email"), TEXT("u-1")))));
		F.Provider->LinkEmail(TEXT("a@b.c"), TEXT("pw"), nullptr);

		// Sign in as a different player through the real login path, no Logout() in between.
		F.Fake->On(TEXT("player/login/device"), FFlockFakeTransport::Ok(FString::Printf(
			TEXT("{\"player_id\":\"p-2\",\"access_token\":\"%s\",\"refresh_token\":\"r-2\"}"), *MakeJwt(TEXT("p-2")))));
		F.Provider->LoginWithDevice(TEXT("dev-2"), nullptr);

		EFlockErrorType Type = EFlockErrorType::None;
		F.Provider->ResetPassword(TEXT("a@b.c"), TEXT("code"), TEXT("new"),
			[&](TFlockResult<FFlockAuthActionResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("new player does not inherit the gate"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Auth));
	}
	// Logout clears it.
	{
		FFixture F;
		F.SignIn();
		F.Fake->On(TEXT("player/link/email"), FFlockFakeTransport::Ok(AccountsBody(Account(TEXT("email"), TEXT("u-1")))));
		F.Provider->LinkEmail(TEXT("a@b.c"), TEXT("pw"), nullptr);
		F.Provider->Logout();
		F.SignIn();

		EFlockErrorType Type = EFlockErrorType::None;
		F.Provider->ResetPassword(TEXT("a@b.c"), TEXT("code"), TEXT("new"),
			[&](TFlockResult<FFlockAuthActionResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("gate cleared by logout"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Auth));
	}
	// A restore knows nothing about linked credentials until the game reads the list.
	{
		FFixture F;
		F.SignIn();
		F.Fake->On(TEXT("player/link/email"), FFlockFakeTransport::Ok(AccountsBody(Account(TEXT("email"), TEXT("u-1")))));
		F.Provider->LinkEmail(TEXT("a@b.c"), TEXT("pw"), nullptr);

		FFlockStoredTokens Stored;
		Stored.AccessToken = MakeJwt(TEXT("p-1"));
		Stored.RefreshToken = TEXT("r-1");
		Stored.AuthMethod = EFlockAuthMethod::Device;
		F.Store->Save(Stored);
		F.Provider->TryRestoreSession(nullptr);

		EFlockErrorType Type = EFlockErrorType::None;
		F.Provider->ResetPassword(TEXT("a@b.c"), TEXT("code"), TEXT("new"),
			[&](TFlockResult<FFlockAuthActionResponse> R) { Type = R.Error.Type; });
		TestEqual(TEXT("gate cleared by restore"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Auth));
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
