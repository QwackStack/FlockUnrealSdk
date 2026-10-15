// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "FlockLogger.h"
#include "HAL/FileManager.h"
#include "Http/FlockError.h"
#include "Http/FlockErrorHints.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockSnapshotStore.h"
#include "Misc/Base64.h"
#include "Misc/Paths.h"
#include "Models/FlockGameModels.h"
#include "Providers/FlockShopProvider.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockRecordingLogger.h"

// Every failure a developer sees says what is wrong and how to fix it: the server's own reason when it gave one, the
// plain meaning of the status or the transport failure otherwise, and a fix that is never empty.

namespace FlockErrorFixTestHelpers
{
	/** The texts every failure used to read as, which named neither the cause nor the fix. */
	const TCHAR* const OldGenericTexts[] = {
		TEXT("HTTP request failed"), TEXT("Authentication failed"), TEXT("Validation failed"), TEXT("Network request failed"),
		TEXT("Request timeout"), TEXT("Empty response from server"), TEXT("Malformed response body"),
	};

	/** One read through the real client against an answer, so the classification under test is the one games get. */
	FFlockError FailureFor(const FFlockHttpResponse& Response)
	{
		const TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		const TSharedRef<FFlockHttpClient> Client = MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>());
		Fake->On(TEXT("test"), Response);
		FFlockError Error;
		Client->Get<FFlockGameVersionSchema>(TEXT("http://x/v1/test"), {}, [&Error](TFlockResult<FFlockGameVersionSchema> R) { Error = R.Error; });
		return Error;
	}

	FFlockHttpResponse Answer(int32 Status, const FString& Body)
	{
		return FFlockFakeTransport::Status(Status, Body);
	}

	FString MakeTestJwt(const FString& PlayerId)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + 3600;
		FString Payload = FBase64::Encode(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp));
		Payload.ReplaceInline(TEXT("+"), TEXT("-"));
		Payload.ReplaceInline(TEXT("/"), TEXT("_"));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Payload);
	}

	/** A shop provider over a fake server, logging into a recorder so the failure line a developer reads is checked too. */
	struct FShopFixture
	{
		FString Dir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("errorfix_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
		TSharedRef<FFlockHttpClient> Client = MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>());
		TSharedRef<FFlockAuthSession> Session = MakeShared<FFlockAuthSession>(Client, MakeShared<FFlockMemoryTokenStore>(),
			MakeShared<FFlockNullLogger>(), TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } });
		TSharedPtr<FFlockShopProvider> Provider;

		FShopFixture()
		{
			FFlockRetryPolicy NoRetry;
			NoRetry.MaxRetries = 0;
			const TSharedRef<FFlockSnapshotStore> Snapshot = MakeShared<FFlockSnapshotStore>(Dir, MakeShared<FFlockNullLogger>(), TEXT("9.9.9"));
			Provider = MakeShared<FFlockShopProvider>(Client, NoRetry, Log, Session, TEXT("http://x/v1"), Snapshot, TEXT("ver-1"));
		}

		~FShopFixture()
		{
			IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
		}

		void SignIn()
		{
			FString Error;
			Session->SetTokens(MakeTestJwt(TEXT("player-a")), TEXT("r-1"), Error);
		}

		TOptional<FFlockError> Purchase(const FString& ShopItemId)
		{
			TOptional<FFlockError> Failure;
			Provider->Purchase(ShopItemId, [&Failure](TFlockResult<FFlockPurchaseResult> R)
			{
				if (!R.bSuccess)
				{
					Failure = R.Error;
				}
			});
			return Failure;
		}
	};
}

using namespace FlockErrorFixTestHelpers;

// ── The report this was made for: a purchase given the item's name answered "HTTP request failed (HTTP 404)" ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockErrorFixWrongShopItemIdTest, "Flock.Http.Error.APurchaseGivenTheWrongIdSaysWhichIdItTakes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockErrorFixWrongShopItemIdTest::RunTest(const FString& Parameters)
{
	// The answer as the local backend sent it on 2026-10-15 for GET /v1/shop_item/GoldItem (the item's name, not its id).
	FShopFixture Fx;
	Fx.SignIn();
	Fx.Fake->On(TEXT("shop_item/GoldItem"), Answer(404,
		TEXT("{\"detail\":{\"code\":\"shop_item.shop_item_not_found\",\"message\":\"Shop item with ID 'GoldItem' not found\"}}")));

	const TOptional<FFlockError> Failure = Fx.Purchase(TEXT("GoldItem"));
	if (!TestTrue(TEXT("the purchase fails"), Failure.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("on its own code"), Failure->ErrorCode, EFlockErrorCode::ShopItemShopItemNotFound);
	TestEqualSensitive(TEXT("what is wrong is the server's own reason, naming the id given"), Failure->Message,
		FString(TEXT("Shop item with ID 'GoldItem' not found")));
	TestTrue(TEXT("the fix names the id a purchase takes"), Failure->Hint.Contains(TEXT("FFlockShopItem::Id"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("and the ids it does not"), Failure->Hint.Contains(TEXT("not the item's name or the shop's id"), ESearchCase::CaseSensitive));
	const FString Shown = Failure->ToDisplayText();
	TestTrue(TEXT("the text shown reads cause, tag, then fix"), Shown.StartsWith(
		TEXT("Fetch shop item failed: Shop item with ID 'GoldItem' not found [shop_item.shop_item_not_found, HTTP 404]\nFix: "), ESearchCase::CaseSensitive));
	for (const TCHAR* Old : OldGenericTexts)
	{
		TestFalse(FString::Printf(TEXT("no '%s'"), Old), Shown.Contains(Old));
	}
	// The error line in the Output Log is what the developer saw, so it carries the same cause and fix.
	TestTrue(TEXT("the logged failure names the cause"), FFlockRecordingLogger::AnyContains(Fx.Log->Errors, TEXT("Shop item with ID 'GoldItem' not found")));
	TestTrue(TEXT("and the fix"), FFlockRecordingLogger::AnyContains(Fx.Log->Errors, TEXT("Fix: ") + Failure->Hint));
	TestEqual(TEXT("nothing was bought"), Fx.Fake->CountTo(TEXT("shop/transaction")), 0);

	// Counter-case: the item reads, and the purchase itself is refused on the purchase route's own code.
	FShopFixture Bought;
	Bought.SignIn();
	Bought.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(
		TEXT("{\"id\":\"item-1\",\"name\":\"GoldItem\",\"status\":\"active\",\"type\":\"standard\",\"shop_id\":\"shop-1\",\"price\":10,\"currency\":\"GOLD\",\"created_at\":\"2026-10-15T00:00:00Z\",\"updated_at\":\"2026-10-15T00:00:00Z\"}")));
	Bought.Fake->On(TEXT("shop/transaction"), Answer(404, TEXT("{\"detail\":{\"code\":\"shop.item_not_found\",\"message\":\"Shop item not found\"}}")));
	const TOptional<FFlockError> Refused = Bought.Purchase(TEXT("item-1"));
	if (TestTrue(TEXT("the purchase route's refusal fails it"), Refused.IsSet()))
	{
		TestEqual(TEXT("on the purchase route's code"), Refused->ErrorCode, EFlockErrorCode::ShopItemNotFound);
		TestEqualSensitive(TEXT("with its fix"), Refused->Hint, FFlockErrorHints::For(EFlockErrorCode::ShopItemNotFound));
		TestTrue(TEXT("which names the id a purchase takes"), Refused->Hint.Contains(TEXT("FFlockShopItem::Id"), ESearchCase::CaseSensitive));
	}
	return true;
}

// ── Each answer measured from the local backend on 2026-10-15 reads as its cause and carries its code's fix ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockErrorFixMeasuredAnswersTest, "Flock.Http.Error.TheMeasuredAnswersSayWhatIsWrongAndHowToFixIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockErrorFixMeasuredAnswersTest::RunTest(const FString& Parameters)
{
	struct FMeasured
	{
		const TCHAR* What;
		int32 Status;
		const TCHAR* Body;
		EFlockErrorCode Code;
		const TCHAR* Message;
	};
	const FMeasured Answers[] = {
		{ TEXT("an API URL ending in /v1"), 404, TEXT("{\"detail\":{\"code\":\"request.not_found\",\"message\":\"Not Found\"}}"), EFlockErrorCode::RequestNotFound, TEXT("Not Found") },
		{ TEXT("a request of the wrong kind"), 405, TEXT("{\"detail\":{\"code\":\"request.method_not_allowed\",\"message\":\"Method Not Allowed\"}}"), EFlockErrorCode::RequestMethodNotAllowed, TEXT("Method Not Allowed") },
		{ TEXT("a body missing a member"), 422, TEXT("{\"detail\":{\"code\":\"request.validation_failed\",\"message\":\"body.shop_item_id: Field required\",\"fields\":[{\"type\":\"missing\",\"loc\":[\"body\",\"shop_item_id\"],\"msg\":\"Field required\",\"input\":{}}]}}"), EFlockErrorCode::RequestValidationFailed, TEXT("body.shop_item_id: Field required") },
		{ TEXT("a blank API key"), 401, TEXT("{\"detail\":{\"code\":\"game.missing_api_key\",\"message\":\"Missing X-Flock-API-Key header\"}}"), EFlockErrorCode::GameMissingApiKey, TEXT("Missing X-Flock-API-Key header") },
		{ TEXT("an unknown API key"), 401, TEXT("{\"detail\":{\"code\":\"game.invalid_api_key\",\"message\":\"Invalid API Key\"}}"), EFlockErrorCode::GameInvalidApiKey, TEXT("Invalid API Key") },
		{ TEXT("no sign-in"), 401, TEXT("{\"detail\":{\"code\":\"player.missing_token\",\"message\":\"Missing token.\"}}"), EFlockErrorCode::PlayerMissingToken, TEXT("Missing token.") },
		{ TEXT("a sign-in from elsewhere"), 401, TEXT("{\"detail\":{\"code\":\"player.invalid_token\",\"message\":\"Invalid token.\"}}"), EFlockErrorCode::PlayerInvalidToken, TEXT("Invalid token.") },
		{ TEXT("a purchase given the shop's id"), 404, TEXT("{\"detail\":{\"code\":\"shop.item_not_found\",\"message\":\"Shop item not found\"}}"), EFlockErrorCode::ShopItemNotFound, TEXT("Shop item not found") },
		{ TEXT("a purchase for an unknown player"), 404, TEXT("{\"detail\":{\"code\":\"shop.player_not_found\",\"message\":\"Player not found\"}}"), EFlockErrorCode::ShopPlayerNotFound, TEXT("Player not found") },
		{ TEXT("a shop by an unknown name"), 404, TEXT("{\"detail\":{\"code\":\"shop.shop_not_found\",\"message\":\"Shop with name 'no-such-shop' not found\"}}"), EFlockErrorCode::ShopShopNotFound, TEXT("Shop with name 'no-such-shop' not found") },
		{ TEXT("a board by an unknown name"), 404, TEXT("{\"detail\":{\"code\":\"leaderboard.not_found\",\"message\":\"Leaderboard 'Nope' not found\"}}"), EFlockErrorCode::LeaderboardNotFound, TEXT("Leaderboard 'Nope' not found") },
	};
	for (const FMeasured& Measured : Answers)
	{
		const FFlockError Error = FailureFor(Answer(Measured.Status, Measured.Body));
		TestEqual(FString::Printf(TEXT("%s: typed code"), Measured.What), Error.ErrorCode, Measured.Code);
		TestEqualSensitive(FString::Printf(TEXT("%s: the server's reason is what is wrong"), Measured.What), Error.Message, FString(Measured.Message));
		TestEqualSensitive(FString::Printf(TEXT("%s: the code's own fix"), Measured.What), Error.Hint, FFlockErrorHints::For(Measured.Code));
		TestFalse(FString::Printf(TEXT("%s: a fix there is"), Measured.What), Error.Hint.IsEmpty());
	}

	// A wrong API URL is named as such, wherever the route would have been.
	TestTrue(TEXT("request.not_found says to check API URL"),
		FFlockErrorHints::For(EFlockErrorCode::RequestNotFound).Contains(TEXT("API URL under Project Settings > Flock SDK"), ESearchCase::CaseSensitive));

	// An API URL without /v1 reaches the dashboard's routes, which answer with no Flock code: not the player's sign-in.
	const FFlockError NoVersion = FailureFor(Answer(401, TEXT("{\"detail\":\"Invalid token.\"}")));
	TestEqualSensitive(TEXT("an uncoded answer keeps its own words"), NoVersion.Message, FString(TEXT("Invalid token.")));
	TestTrue(TEXT("and is called an answer from something other than Flock's game API"), NoVersion.Hint.Contains(TEXT("no Flock error code"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("pointing at API URL"), NoVersion.Hint.Contains(TEXT("API URL"), ESearchCase::CaseSensitive));
	return true;
}

// ── Every status, body shape and transport failure reads as a cause and carries a fix ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockErrorFixEveryFailureTest, "Flock.Http.Error.EveryFailureSaysWhatIsWrongAndHowToFixIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockErrorFixEveryFailureTest::RunTest(const FString& Parameters)
{
	const int32 Statuses[] = { 400, 401, 403, 404, 405, 408, 409, 413, 418, 422, 429, 500, 502, 503 };
	struct FBody
	{
		const TCHAR* What;
		const TCHAR* Text;
	};
	const FBody Bodies[] = {
		{ TEXT("no body"), TEXT("") },
		{ TEXT("a captive portal's page"), TEXT("<html><body>Sign in to the Wi-Fi</body></html>") },
		{ TEXT("a known code"), TEXT("{\"detail\":{\"code\":\"shop.item_not_found\",\"message\":\"Shop item not found\"}}") },
		{ TEXT("a code this SDK does not know"), TEXT("{\"detail\":{\"code\":\"player.something_new\",\"message\":\"Something new\"}}") },
		{ TEXT("field errors"), TEXT("{\"detail\":[{\"loc\":[\"body\",\"name\"],\"msg\":\"Field required\",\"type\":\"missing\"}]}") },
	};

	for (const int32 Status : Statuses)
	{
		for (const FBody& Body : Bodies)
		{
			const FString Label = FString::Printf(TEXT("HTTP %d, %s"), Status, Body.What);
			const FFlockError Error = FailureFor(Answer(Status, Body.Text));
			TestFalse(Label + TEXT(": says what is wrong"), Error.Message.IsEmpty());
			TestFalse(Label + TEXT(": says how to fix it"), Error.Hint.IsEmpty());
			const FString Shown = Error.ToDisplayText();
			TestTrue(Label + TEXT(": shows the status"), Shown.Contains(FString::Printf(TEXT("HTTP %d]"), Status)));
			TestTrue(Label + TEXT(": shows the fix"), Shown.Contains(TEXT("\nFix: ") + Error.Hint));
			for (const TCHAR* Old : OldGenericTexts)
			{
				TestFalse(FString::Printf(TEXT("%s: no '%s'"), *Label, Old), Shown.Contains(Old));
			}
		}

		// Without a code the answer did not come from Flock's game API, and the fix says which kind of trouble it is.
		const FFlockError Bare = FailureFor(Answer(Status, TEXT("")));
		const FString Label = FString::Printf(TEXT("HTTP %d with no code"), Status);
		if (Status >= 500)
		{
			TestTrue(Label + TEXT(": a server failure"), Bare.Hint.Contains(TEXT("failed while handling the call")));
		}
		else if (Status == 408 || Status == 429)
		{
			TestTrue(Label + TEXT(": slow down"), Bare.Hint.Contains(TEXT("slow down")));
		}
		else
		{
			TestTrue(Label + TEXT(": not Flock's answer"), Bare.Hint.Contains(TEXT("no Flock error code")));
			TestTrue(Label + TEXT(": check API URL"), Bare.Hint.Contains(TEXT("API URL")));
		}

		// A code newer than this SDK: the server's reason, and the code named in the fix (below 500).
		const FFlockError Newer = FailureFor(Answer(Status, TEXT("{\"detail\":{\"code\":\"player.something_new\",\"message\":\"Something new\"}}")));
		TestEqualSensitive(FString::Printf(TEXT("HTTP %d, unknown code: the server's reason"), Status), Newer.Message, FString(TEXT("Something new")));
		if (Status < 500)
		{
			TestTrue(FString::Printf(TEXT("HTTP %d, unknown code: named in the fix"), Status), Newer.Hint.Contains(TEXT("player.something_new")));
		}
	}

	// A 403 without a reason is a refusal, not a sign-in problem.
	TestFalse(TEXT("a bare 403 does not blame the sign-in"), FailureFor(Answer(403, TEXT(""))).Message.Contains(TEXT("uthenticat")));

	// No answer at all, each with its own fix.
	const FFlockError Timeout = FailureFor(FFlockFakeTransport::Timeout());
	TestEqual(TEXT("timeout type"), Timeout.Type, EFlockErrorType::Timeout);
	TestTrue(TEXT("a timeout warns that a change may have been made"), Timeout.Hint.Contains(TEXT("may still have been made")));
	const FFlockError Offline = FailureFor(FFlockFakeTransport::Offline());
	TestEqual(TEXT("offline type"), Offline.Type, EFlockErrorType::Connection);
	TestTrue(TEXT("no connection points at the network and API URL"), Offline.Hint.Contains(TEXT("online")) && Offline.Hint.Contains(TEXT("API URL")));
	FFlockHttpResponse CancelledResponse;
	CancelledResponse.Result = EFlockHttpResult::Cancelled;
	const FFlockError Cancelled = FailureFor(CancelledResponse);
	TestEqual(TEXT("cancelled type"), Cancelled.Type, EFlockErrorType::Cancelled);
	TestTrue(TEXT("a cancel says when nothing needs fixing"), Cancelled.Hint.Contains(TEXT("Nothing needs fixing")));

	// A 2xx that is not Flock's answer.
	const FFlockError Empty = FailureFor(FFlockFakeTransport::Ok(TEXT("")));
	TestTrue(TEXT("an empty success says it was empty"), Empty.Message.Contains(TEXT("was empty")));
	TestEqualSensitive(TEXT("and that it was not Flock's answer"), Empty.Hint, FFlockErrorHints::NotFlocksAnswer());
	const FFlockError Portal = FailureFor(FFlockFakeTransport::Ok(TEXT("<html>Sign in</html>")));
	TestEqual(TEXT("an unreadable success fails the read"), Portal.Type, EFlockErrorType::Serialization);
	TestEqualSensitive(TEXT("as not Flock's answer"), Portal.Hint, FFlockErrorHints::NotFlocksAnswer());
	return true;
}

// ── A failure the SDK finds itself carries its own fix, and every kind gets one when it does not give it ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockErrorFixSdkMadeTest, "Flock.Http.Error.AFailureTheSdkFindsItselfCarriesAFix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockErrorFixSdkMadeTest::RunTest(const FString& Parameters)
{
	const FFlockError Given = FFlockError::MakeWithFix(EFlockErrorType::Validation, TEXT("Queue Name is empty"), TEXT("Pass the queue's name."));
	TestEqualSensitive(TEXT("the cause"), Given.Message, FString(TEXT("Queue Name is empty")));
	TestEqualSensitive(TEXT("the fix given"), Given.Hint, FString(TEXT("Pass the queue's name.")));
	TestEqualSensitive(TEXT("shown together"), Given.ToDisplayText(), FString(TEXT("Queue Name is empty\nFix: Pass the queue's name.")));

	const UEnum* Types = StaticEnum<EFlockErrorType>();
	for (int32 Index = 0; Index < Types->NumEnums() - 1; ++Index)
	{
		const EFlockErrorType Type = static_cast<EFlockErrorType>(Types->GetValueByIndex(Index));
		if (Type == EFlockErrorType::None)
		{
			continue;
		}
		const FString Name = Types->GetNameStringByIndex(Index);
		TestFalse(Name + TEXT(" made with no fix still carries one"), FFlockError::Make(Type, TEXT("x")).Hint.IsEmpty());
		TestFalse(Name + TEXT(" given an empty fix still carries one"), FFlockError::MakeWithFix(Type, TEXT("x"), FString()).Hint.IsEmpty());
	}
	TestEqualSensitive(TEXT("a call with nobody signed in says to sign in"), FFlockError::Make(EFlockErrorType::Auth, TEXT("x")).Hint, FFlockErrorHints::SignInFirst());

	// Through a provider: an empty argument, and a call that acts as the player with nobody signed in.
	FShopFixture Fx;
	const TOptional<FFlockError> NoId = Fx.Purchase(FString());
	if (TestTrue(TEXT("an empty id fails"), NoId.IsSet()))
	{
		TestEqualSensitive(TEXT("naming the argument"), NoId->Message, FString(TEXT("Shop Item ID is empty")));
		TestEqualSensitive(TEXT("and asking for it"), NoId->Hint, FString(TEXT("Pass a Shop Item ID; this call cannot run without one.")));
	}
	const TOptional<FFlockError> NobodySignedIn = Fx.Purchase(TEXT("item-1"));
	if (TestTrue(TEXT("a purchase with nobody signed in fails"), NobodySignedIn.IsSet()))
	{
		TestEqualSensitive(TEXT("saying to sign in"), NobodySignedIn->Hint, FFlockErrorHints::SignInFirst());
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
