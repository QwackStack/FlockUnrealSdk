// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"
#include "Misc/FlockEngineCompat.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "FlockLogger.h"
#include "HAL/FileManager.h"
#include "Http/FlockEndpoints.h"
#include "Http/FlockErrorHints.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockProviderBase.h"
#include "Http/FlockSnapshotStore.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Blueprint/FlockShopLibrary.h"
#include "Providers/FlockCommandProvider.h"
#include "Providers/FlockConfigProvider.h"
#include "Providers/FlockPlayerProvider.h"
#include "Providers/FlockShopProvider.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockRecordingLogger.h"
#include "Tests/Support/FlockTestRetries.h"
#include "Tests/Support/FlockTestSafeIndex.h"

// The wallet and shop-screen calls: a wallet purchase sent again under its key, a money call's unknown outcome, the wallet by
// currency code, owned counts, consume one, a player-data read in flight across a drop or a server row, and the token read on
// every attempt. Fixtures are the canonical suite's, verbatim where it has them.
namespace FlockWalletShopScreenTestHelpers
{
	inline FFlockRetryPolicy Policy(int32 MaxRetries)
	{
		FFlockRetryPolicy Retry;
		Retry.MaxRetries = MaxRetries;
		Retry.InitialDelaySeconds = 0.f;
		Retry.bUseJitter = false;
		return Retry;
	}

	inline FString Jwt(const FString& PlayerId, int32 Variant = 0)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + 3600 + Variant;
		FString Payload = FBase64::Encode(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp));
		Payload.ReplaceInline(TEXT("+"), TEXT("-"));
		Payload.ReplaceInline(TEXT("/"), TEXT("_"));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Payload);
	}

	// ── The canonical suite's wire fixtures ──

	inline FString Currency(const FString& Code, const FString& Value)
	{
		return FString::Printf(TEXT("{\"type\":\"integer\",\"value\":%s,\"type_name\":\"currency\",\"field_name\":\"%s\"}"), *Value, *Code);
	}

	/** The template list, enveloped, as the local backend sent it. */
	inline FString TemplateList()
	{
		return TEXT("{\"error\":{\"code\":null},\"response\":{\"message\":null,\"code\":null},\"result\":[")
			TEXT("{\"id\":\"tpl-progress\",\"name\":\"progress\",\"game_version_id\":\"gv-1\",\"schema\":[],\"data\":[],\"tag\":\"progress\"},")
			TEXT("{\"id\":\"tpl-wallet\",\"name\":\"currencies\",\"game_version_id\":\"gv-1\",\"schema\":[],\"data\":[],\"tag\":\"currency\"}]}");
	}

	/** The wallet row, each category an object of integer fields, with this much Gold. */
	inline FString WalletRow(const FString& Gold = TEXT("250"), const FString& PlayerId = TEXT("player-a"))
	{
		return FString::Printf(TEXT("{\"id\":\"pd-wallet\",\"player_template_id\":\"tpl-wallet\",\"game_id\":\"game-1\",\"player_id\":\"%s\",\"data\":[")
			TEXT("{\"type\":\"object\",\"value\":[%s,%s],\"type_name\":\"wallet\",\"field_name\":\"game_currencies\"},")
			TEXT("{\"type\":\"object\",\"value\":[%s],\"type_name\":\"wallet\",\"field_name\":\"real_currencies\"}]}"),
			*PlayerId, *Currency(TEXT("Gold"), Gold), *Currency(TEXT("Gems"), TEXT("0")), *Currency(TEXT("SAR"), TEXT("1200")));
	}

	inline FString ProgressRow()
	{
		return TEXT("{\"id\":\"pd-progress\",\"player_template_id\":\"tpl-progress\",\"player_id\":\"player-a\",\"data\":[]}");
	}

	inline FString RowsPage(const TArray<FString>& Rows)
	{
		return FString::Printf(TEXT("{\"items\":[%s],\"total\":%d,\"page\":1,\"limit\":100}"), *FString::Join(Rows, TEXT(",")), Rows.Num());
	}

	/** The wallet row a purchase, consume or fund grant answers with, holding this much Gold. */
	inline FFlockPlayerData WalletWithGold(const FString& Gold, const FString& PlayerId = TEXT("player-a"))
	{
		FFlockPlayerData Row;
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(WalletRow(Gold, PlayerId));
		if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid())
		{
			FString Error;
			FFlockPlayerData::FromWireObject(Object.ToSharedRef(), Row, Error);
		}
		return Row;
	}

	/** An inventory entry as the inventory route answers with it, on a page at the root. */
	inline FString InventoryEntry(const FString& Id, const FString& ShopItemId, const FString& Status)
	{
		return FString::Printf(TEXT("{\"id\":\"%s\",\"player_id\":\"player-a\",\"shop_item_id\":\"%s\",\"status\":\"%s\",\"created_at\":\"2026-10-27T00:00:00Z\"}"),
			*Id, *ShopItemId, *Status);
	}

	inline FString InventoryPage(int32 Page, int32 Total, const TArray<FString>& Entries)
	{
		return FString::Printf(TEXT("{\"items\":[%s],\"total\":%d,\"page\":%d,\"limit\":100}"), *FString::Join(Entries, TEXT(",")), Total, Page);
	}

	inline FString ConsumedAnswer(const FString& EntryId)
	{
		return FString::Printf(TEXT("{\"inventory\":{\"id\":\"%s\",\"player_id\":\"player-a\",\"shop_item_id\":\"potion\",\"status\":\"used\"},")
			TEXT("\"granted\":[{\"type\":\"currency\",\"code\":\"Gold\",\"amount\":10}]}"), *EntryId);
	}

	/** The item the shop item route answers with, at the root. */
	inline FString ItemBody()
	{
		return TEXT("{\"id\":\"item-1\",\"name\":\"Gold Pack\",\"type\":\"currency_pack\",\"price\":100,\"currency\":\"USD\",")
			TEXT("\"rewards\":[{\"type\":\"currency\",\"code\":\"GOLD\",\"amount\":500}]}");
	}

	inline FString PurchaseAnswer()
	{
		return TEXT("{\"purchase_id\":\"pur-1\",\"item_type\":\"standard\"}");
	}

	inline FString Route(const FString& Fragment) { return Fragment; }
	inline FString InventoryRoute() { return FlockEndpoints::PlayerInventoryByPlayer(TEXT("player-a")); }

	inline FFlockHttpResponse ServerError() { return FFlockFakeTransport::Status(500, TEXT("{}")); }
	inline FFlockHttpResponse TooMany() { return FFlockFakeTransport::Status(429, TEXT("{}")); }
	inline FFlockHttpResponse BadGateway() { return FFlockFakeTransport::Status(502, TEXT("<html>Bad Gateway</html>")); }
	inline FFlockHttpResponse Expired()
	{
		return FFlockFakeTransport::Status(401, TEXT("{\"detail\":{\"code\":\"player.expired_token\",\"message\":\"Expired token.\"}}"));
	}

	/** The idempotency_key a purchase request carried; empty when it carried none. */
	inline FString KeyOf(const FFlockHttpRequest& Request)
	{
		TSharedPtr<FJsonObject> Body;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Request.JsonBody);
		FString Key;
		if (FJsonSerializer::Deserialize(Reader, Body) && Body.IsValid())
		{
			Body->TryGetStringField(TEXT("idempotency_key"), Key);
		}
		return Key;
	}

	inline TArray<FFlockHttpRequest> RequestsTo(const FFlockFakeTransport& Fake, const FString& Fragment)
	{
		return Fake.Requests.FilterByPredicate([&Fragment](const FFlockHttpRequest& Request) { return Request.Url.Contains(Fragment); });
	}

	struct FFixture
	{
		FString Dir;
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockAuthSession> Session;
		TSharedPtr<FFlockSnapshotStore> Snapshot;
		TSharedPtr<FFlockShopProvider> Shop;
		TSharedPtr<FFlockPlayerProvider> Players;

		explicit FFixture(int32 MaxRetries = 0)
			: Dir(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
				FString::Printf(TEXT("wallet_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))))
			, Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
		{
			Snapshot = MakeShared<FFlockSnapshotStore>(Dir, MakeShared<FFlockNullLogger>(), TEXT("9.9.9"));
			Shop = MakeShared<FFlockShopProvider>(Client, Policy(MaxRetries), Log, Session, TEXT("http://x/v1"), Snapshot, TEXT("ver-1"));
			Players = MakeShared<FFlockPlayerProvider>(Client, Policy(MaxRetries), MakeShared<FFlockNullLogger>(), Session,
				TEXT("http://x/v1"), Snapshot, TEXT("ver-1"));
			Shop->SetPlayerProvider(Players);
		}

		~FFixture()
		{
			IFileManager::Get().DeleteDirectory(*Dir, false, true);
		}

		void SignIn(const FString& PlayerId = TEXT("player-a"), int32 Variant = 0)
		{
			FString Error;
			Session->SetTokens(Jwt(PlayerId, Variant), TEXT("r-1"), Error);
		}

		/** The wallet route answers with these rows; the template list as the server sends it. */
		void WithRows(const TArray<FString>& Rows)
		{
			Fake->On(FlockEndpoints::PlayerTemplate, FFlockFakeTransport::Ok(TemplateList()));
			Fake->On(TEXT("player_data?page="), FFlockFakeTransport::Ok(RowsPage(Rows)));
		}

		/** Reads the wallet; false when the read never answered. */
		bool ReadWallet(TFlockResult<FFlockWallet>& Out)
		{
			bool bAnswered = false;
			Players->GetWallet([&](TFlockResult<FFlockWallet> Result) { Out = Result; bAnswered = true; });
			return bAnswered;
		}

		/** Seeds the cached wallet with this much Gold through a read; false when the read failed. */
		bool CacheWallet(const FString& Gold)
		{
			WithRows({ WalletRow(Gold) });
			TFlockResult<FFlockWallet> Read;
			return ReadWallet(Read) && Read.bSuccess;
		}

		/** True when the wallet row is still cached, so a read would answer without asking the server. */
		bool WalletIsCached() const
		{
			FFlockPlayerData Row;
			return Players->TryGetCachedRow(TEXT("pd-wallet"), Row);
		}

		void WithItem()
		{
			Fake->On(FlockEndpoints::ShopItemById(TEXT("item-1")), FFlockFakeTransport::Ok(ItemBody()));
		}

		bool Buy(TFlockResult<FFlockPurchaseResult>& Out)
		{
			bool bAnswered = false;
			Shop->Purchase(TEXT("item-1"), [&](TFlockResult<FFlockPurchaseResult> Result) { Out = Result; bAnswered = true; });
			FlockTestRetries::Pump();
			return bAnswered;
		}

		bool ConsumeOne(const FString& ShopItemId, TFlockResult<FFlockConsumeResult>& Out)
		{
			bool bAnswered = false;
			Shop->ConsumeOne(ShopItemId, [&](TFlockResult<FFlockConsumeResult> Result) { Out = Result; bAnswered = true; });
			FlockTestRetries::Pump();
			return bAnswered;
		}

		bool OwnedCounts(TFlockResult<FFlockOwnedItemCounts>& Out)
		{
			bool bAnswered = false;
			Shop->GetOwnedItemCounts([&](TFlockResult<FFlockOwnedItemCounts> Result) { Out = Result; bAnswered = true; });
			return bAnswered;
		}

		/** A full first page, then a second: 99 potions and a used sword, then an unused sword and a refunded shield. */
		void WithTwoInventoryPages()
		{
			TArray<FString> First;
			for (int32 Index = 0; Index < 99; ++Index)
			{
				First.Add(InventoryEntry(FString::Printf(TEXT("inv-p%d"), Index), TEXT("potion"), TEXT("unused")));
			}
			First.Add(InventoryEntry(TEXT("inv-s1"), TEXT("sword"), TEXT("used")));
			const TArray<FString> Second = { InventoryEntry(TEXT("inv-s2"), TEXT("sword"), TEXT("unused")),
				InventoryEntry(TEXT("inv-sh"), TEXT("shield"), TEXT("revoked")) };
			Fake->On(InventoryRoute() + TEXT("?page=1&limit=100"), FFlockFakeTransport::Ok(InventoryPage(1, 102, First)));
			Fake->On(InventoryRoute() + TEXT("?page=2&limit=100"), FFlockFakeTransport::Ok(InventoryPage(2, 102, Second)));
		}

		void InventoryOf(const TArray<FString>& Entries)
		{
			Fake->On(InventoryRoute(), FFlockFakeTransport::Ok(InventoryPage(1, Entries.Num(), Entries)));
		}

		void RenewsTo(const FString& Renewed)
		{
			Fake->On(FlockEndpoints::PlayerTokenRefresh, FFlockFakeTransport::Ok(FString::Printf(
				TEXT("{\"player_id\":\"player-a\",\"access_token\":\"%s\",\"refresh_token\":\"r-2\"}"), *Renewed)));
		}
	};

	inline int64 GoldOf(const TFlockResult<FFlockWallet>& Read) { return Read.bSuccess ? Read.Value.BalanceOf(TEXT("Gold")) : -1; }

	/** Exposes the protected money rule, so a cancel through the call's own handle is tested where it lives. */
	class FMoneyRuleProvider : public FFlockProviderBase
	{
	public:
		using FFlockProviderBase::FFlockProviderBase;
		using FFlockProviderBase::ExecuteMoneyCall;
		using FFlockProviderBase::FMoneyCall;
	};
}

using namespace FlockWalletShopScreenTestHelpers;

// ───────────────────────────── A wallet purchase sent again under its key ─────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPurchaseSentAgainWithTheSameKeyTest, "Flock.Shop.Money.PurchaseServerErrorIsSentAgainWithTheSameKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPurchaseSentAgainWithTheSameKeyTest::RunTest(const FString& Parameters)
{
	FFixture Fx(3);
	Fx.SignIn();
	Fx.WithItem();
	Fx.Fake->OnSequence(FlockEndpoints::ShopTransaction, { ServerError(), FFlockFakeTransport::Ok(PurchaseAnswer()) });

	TFlockResult<FFlockPurchaseResult> Result;
	TestTrue(TEXT("the purchase answered"), Fx.Buy(Result));
	TestTrue(TEXT("bought once the second attempt was answered"), Result.bSuccess);
	const TArray<FFlockHttpRequest> Sent = RequestsTo(*Fx.Fake, FlockEndpoints::ShopTransaction);
	if (TestEqual(TEXT("sent again after the 500"), Sent.Num(), 2))
	{
		const FString Key = KeyOf(Sent[0]);
		TestEqual(TEXT("the key is a 32-character id"), Key.Len(), 32);
		TestEqualSensitive(TEXT("the attempt sent again carries the same key"), KeyOf(Sent[1]), Key);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPurchaseKeyPerCallTest, "Flock.Shop.Money.TwoPurchaseCallsEachCarryAKeyOfTheirOwn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPurchaseKeyPerCallTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.WithItem();
	Fx.Fake->On(FlockEndpoints::ShopTransaction, FFlockFakeTransport::Ok(PurchaseAnswer()));

	TFlockResult<FFlockPurchaseResult> First;
	TFlockResult<FFlockPurchaseResult> Second;
	Fx.Buy(First);
	Fx.Buy(Second);
	const TArray<FFlockHttpRequest> Sent = RequestsTo(*Fx.Fake, FlockEndpoints::ShopTransaction);
	if (TestEqual(TEXT("two purchases sent"), Sent.Num(), 2))
	{
		TestFalse(TEXT("each carries a key"), KeyOf(Sent[0]).IsEmpty() || KeyOf(Sent[1]).IsEmpty());
		TestFalse(TEXT("a key of its own: a second purchase is never answered with the first"),
			KeyOf(Sent[0]).Equals(KeyOf(Sent[1]), ESearchCase::CaseSensitive));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPurchaseKeyReusedTest, "Flock.Shop.Money.KeyAlreadyUsedForAnotherItemIsItsCodeAndNotSentAgain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPurchaseKeyReusedTest::RunTest(const FString& Parameters)
{
	FFixture Fx(3);
	Fx.SignIn();
	Fx.WithItem();
	Fx.Fake->On(FlockEndpoints::ShopTransaction, FFlockFakeTransport::Status(409,
		TEXT("{\"detail\":{\"code\":\"shop.idempotency_key_reused\",\"message\":\"idempotency_key was already used for a different item\"}}")));

	TFlockResult<FFlockPurchaseResult> Result;
	Fx.Buy(Result);
	TestFalse(TEXT("refused"), Result.bSuccess);
	TestEqual(TEXT("with its own code"), Result.Error.ErrorCode, EFlockErrorCode::ShopIdempotencyKeyReused);
	TestFalse(TEXT("certain: nothing was bought"), Result.Error.bOutcomeUnknown);
	TestTrue(TEXT("its fix says to buy again"), Result.Error.Hint.Contains(TEXT("call Purchase again"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("a 409 is not sent again"), Fx.Fake->CountTo(FlockEndpoints::ShopTransaction), 1);
	return true;
}

// ───────────────────────────── A money call whose outcome is unknown ─────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPurchaseNoAnswerUnknownTest, "Flock.Shop.Money.PurchaseServerErrorOnEveryAttemptMayHaveBeenMade",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPurchaseNoAnswerUnknownTest::RunTest(const FString& Parameters)
{
	FFixture Fx(3);
	Fx.SignIn();
	TestTrue(TEXT("the wallet is cached first"), Fx.CacheWallet(TEXT("250")));
	Fx.WithItem();
	Fx.Fake->On(FlockEndpoints::ShopTransaction, ServerError());

	TFlockResult<FFlockPurchaseResult> Result;
	TestTrue(TEXT("the purchase answered"), Fx.Buy(Result));
	TestFalse(TEXT("not bought"), Result.bSuccess);
	TestTrue(TEXT("its outcome is unknown"), Result.Error.bOutcomeUnknown);
	TestTrue(TEXT("the fix says it may have been made"), Result.Error.Hint.Contains(TEXT("may have been made"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("and to read the inventory and wallet first"), Result.Error.Hint.Contains(TEXT("inventory and wallet"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("every attempt the policy allows"), Fx.Fake->CountTo(FlockEndpoints::ShopTransaction), 4);
	TestTrue(TEXT("the logged failure carries the fix"), FFlockRecordingLogger::AnyContains(Fx.Log->Errors, TEXT("may have been made")));
	TestFalse(TEXT("the cached wallet is dropped, so the next read asks the server"), Fx.WalletIsCached());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPurchaseUnknownAfterRefreshTest, "Flock.Shop.Money.PurchaseServerErrorAfterATokenRenewalIsUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPurchaseUnknownAfterRefreshTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.WithItem();
	Fx.RenewsTo(Jwt(TEXT("player-a"), 7));
	Fx.Fake->OnSequence(FlockEndpoints::ShopTransaction, { Expired(), ServerError() });

	TFlockResult<FFlockPurchaseResult> Result;
	Fx.Buy(Result);
	TestFalse(TEXT("not bought"), Result.bSuccess);
	TestTrue(TEXT("the attempt after the renewal got no answer, so it is unknown"), Result.Error.bOutcomeUnknown);
	TestEqual(TEXT("the expired attempt and the one after renewing"), Fx.Fake->CountTo(FlockEndpoints::ShopTransaction), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPurchaseNoAnswerThenNotDoneTest, "Flock.Shop.Money.PurchaseNoAnswerThenAttemptsNotDoneIsStillUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPurchaseNoAnswerThenNotDoneTest::RunTest(const FString& Parameters)
{
	FFixture Fx(2);
	Fx.SignIn();
	Fx.WithItem();
	// The first attempt timed out, the rest were turned away before being processed: the first may still have bought it.
	Fx.Fake->OnSequence(FlockEndpoints::ShopTransaction, { FFlockFakeTransport::Timeout(),
		TooMany() });

	TFlockResult<FFlockPurchaseResult> Result;
	Fx.Buy(Result);
	TestFalse(TEXT("not bought"), Result.bSuccess);
	TestEqual(TEXT("ends on the 429"), Result.Error.StatusCode, 429);
	TestTrue(TEXT("still unknown: the timed-out attempt may have bought it"), Result.Error.bOutcomeUnknown);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPurchaseNoAnswerThenRefusedRenewalTest, "Flock.Shop.Money.PurchaseNoAnswerThenARefusedRenewalIsUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPurchaseNoAnswerThenRefusedRenewalTest::RunTest(const FString& Parameters)
{
	FFixture Fx(1);
	Fx.SignIn();
	Fx.WithItem();
	Fx.Fake->On(FlockEndpoints::PlayerTokenRefresh, FFlockFakeTransport::Status(401,
		TEXT("{\"detail\":{\"code\":\"player.invalid_refresh_token\",\"message\":\"Invalid refresh token.\"}}")));
	Fx.Fake->OnSequence(FlockEndpoints::ShopTransaction, { ServerError(), Expired() });

	TFlockResult<FFlockPurchaseResult> Result;
	Fx.Buy(Result);
	TestFalse(TEXT("not bought"), Result.bSuccess);
	TestEqual(TEXT("ends on the expired sign-in"), Result.Error.StatusCode, 401);
	TestTrue(TEXT("unknown: the 500 may have bought it"), Result.Error.bOutcomeUnknown);
	TestTrue(TEXT("with the purchase's fix"), Result.Error.Hint.Contains(TEXT("may have been made"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPurchaseSignedOutBeforeRetryTest, "Flock.Shop.Money.PurchaseSignedOutBeforeItIsSentAgainAfterNoAnswerIsUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPurchaseSignedOutBeforeRetryTest::RunTest(const FString& Parameters)
{
	FFixture Fx(3);
	Fx.SignIn();
	Fx.WithItem();
	Fx.Fake->OnSequence(FlockEndpoints::ShopTransaction, { BadGateway(), FFlockFakeTransport::Ok(PurchaseAnswer()) });

	TFlockResult<FFlockPurchaseResult> Result;
	bool bAnswered = false;
	Fx.Shop->Purchase(TEXT("item-1"), [&](TFlockResult<FFlockPurchaseResult> R) { Result = R; bAnswered = true; });
	// The retry waits on the ticker; the player signs out before it goes.
	Fx.Session->ClearTokens();
	FlockTestRetries::Pump();

	TestTrue(TEXT("the purchase answered"), bAnswered);
	TestEqual(TEXT("not sent again as nobody"), Fx.Fake->CountTo(FlockEndpoints::ShopTransaction), 1);
	TestNotEqual(TEXT("not a plain cancel: the 502 may have bought it"), Result.Error.Type, EFlockErrorType::Cancelled);
	TestEqual(TEXT("ends with the attempt in doubt"), Result.Error.StatusCode, 502);
	TestTrue(TEXT("unknown"), Result.Error.bOutcomeUnknown);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPurchaseCancelNothingInDoubtTest, "Flock.Shop.Money.PurchaseCancelledWithNothingInDoubtStaysACancelAndDropsTheWallet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPurchaseCancelNothingInDoubtTest::RunTest(const FString& Parameters)
{
	FFixture Fx(3);
	Fx.SignIn();
	TestTrue(TEXT("the wallet is cached first"), Fx.CacheWallet(TEXT("250")));
	Fx.WithItem();
	Fx.Fake->OnSequence(FlockEndpoints::ShopTransaction, { TooMany(),
		FFlockFakeTransport::Ok(PurchaseAnswer()) });

	TFlockResult<FFlockPurchaseResult> Result;
	Fx.Shop->Purchase(TEXT("item-1"), [&](TFlockResult<FFlockPurchaseResult> R) { Result = R; });
	Fx.Session->ClearTokens();
	FlockTestRetries::Pump();

	TestEqual(TEXT("a 429 was not processed, so the sign-out ends it as a cancel"), Result.Error.Type, EFlockErrorType::Cancelled);
	TestFalse(TEXT("not unknown"), Result.Error.bOutcomeUnknown);
	TestFalse(TEXT("the cached wallet is dropped all the same"), Fx.WalletIsCached());
	return true;
}

// A refusal under the same key proves nothing was bought: the server looks the key up before these checks. Each refusal still
// does its own clean-up: funds drop the cached wallet, a switched-off item the cached shops.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPurchaseNoAnswerThenRefusedAfterTheKeyTest, "Flock.Shop.Money.PurchaseNoAnswerThenARefusalAfterTheKeyIsCertain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPurchaseNoAnswerThenRefusedAfterTheKeyTest::RunTest(const FString& Parameters)
{
	struct FRefusal
	{
		int32 Status;
		const TCHAR* Code;
		EFlockErrorCode Expected;
	};
	const FRefusal Refusals[] = {
		{ 400, TEXT("shop.insufficient_funds"), EFlockErrorCode::ShopInsufficientFunds },
		{ 400, TEXT("shop.currency_not_held"), EFlockErrorCode::ShopCurrencyNotHeld },
		{ 409, TEXT("shop.item_not_for_sale"), EFlockErrorCode::ShopItemNotForSale },
	};
	for (const FRefusal& Refusal : Refusals)
	{
		FFixture Fx(3);
		Fx.SignIn();
		TestTrue(TEXT("the wallet is cached first"), Fx.CacheWallet(TEXT("250")));
		Fx.Fake->On(FlockEndpoints::ShopByName(TEXT("main")),
			FFlockFakeTransport::Ok(FString::Printf(TEXT("{\"id\":\"shop-1\",\"name\":\"main\",\"shop_items\":[%s]}"), *ItemBody())));
		Fx.Shop->GetByName(TEXT("main"), [](TFlockResult<FFlockShop>) {});
		Fx.WithItem();
		Fx.Fake->OnSequence(FlockEndpoints::ShopTransaction, { FFlockFakeTransport::Timeout(), FFlockFakeTransport::Coded(Refusal.Status, Refusal.Code) });

		TFlockResult<FFlockPurchaseResult> Result;
		Fx.Buy(Result);
		Fx.Shop->GetByName(TEXT("main"), [](TFlockResult<FFlockShop>) {});
		const FString Case = Refusal.Code;
		TestEqual(FString::Printf(TEXT("%s: control, the first attempt got no answer and was sent again under its key"), *Case),
			Fx.Fake->CountTo(FlockEndpoints::ShopTransaction), 2);
		TestEqual(FString::Printf(TEXT("%s: ends on its code"), *Case), Result.Error.ErrorCode, Refusal.Expected);
		TestFalse(FString::Printf(TEXT("%s: certain, nothing was bought"), *Case), Result.Error.bOutcomeUnknown);
		TestEqualSensitive(FString::Printf(TEXT("%s: the code's own fix, not the one for a purchase that may have been made"), *Case),
			Result.Error.Hint, FFlockErrorHints::For(Refusal.Expected));
		const bool bForSale = Refusal.Expected == EFlockErrorCode::ShopItemNotForSale;
		TestEqual(FString::Printf(TEXT("%s: the cached wallet is dropped by a refusal for funds only"), *Case), Fx.WalletIsCached(), bForSale);
		TestEqual(FString::Printf(TEXT("%s: the cached shops are dropped by a switched-off item only"), *Case),
			Fx.Fake->CountTo(FlockEndpoints::ShopByName(TEXT("main"))), bForSale ? 2 : 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPurchaseNoAnswerThenOtherRefusalTest, "Flock.Shop.Money.PurchaseNoAnswerThenAnyOtherRefusalIsStillUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPurchaseNoAnswerThenOtherRefusalTest::RunTest(const FString& Parameters)
{
	const TPair<int32, const TCHAR*> Refusals[] = { { 404, TEXT("shop_item.shop_item_not_found") }, { 404, TEXT("shop.player_not_found") },
		{ 403, TEXT("shop.player_mismatch") }, { 408, nullptr } };
	for (const TPair<int32, const TCHAR*>& Refusal : Refusals)
	{
		FFixture Fx(3);
		Fx.SignIn();
		Fx.WithItem();
		Fx.Fake->OnSequence(FlockEndpoints::ShopTransaction, { FFlockFakeTransport::Timeout(),
			Refusal.Value ? FFlockFakeTransport::Coded(Refusal.Key, Refusal.Value) : FFlockFakeTransport::Status(Refusal.Key, TEXT("{}")) });

		TFlockResult<FFlockPurchaseResult> Result;
		Fx.Buy(Result);
		const FString Case = FString::Printf(TEXT("%d %s"), Refusal.Key, Refusal.Value ? Refusal.Value : TEXT("(no code)"));
		TestEqual(FString::Printf(TEXT("%s: control, the last attempt was refused"), *Case), Result.Error.StatusCode, Refusal.Key);
		TestTrue(FString::Printf(TEXT("%s: the attempt with no answer may have bought it"), *Case), Result.Error.bOutcomeUnknown);
		TestTrue(FString::Printf(TEXT("%s: the fix says it may have been made"), *Case),
			Result.Error.Hint.Contains(TEXT("may have been made"), ESearchCase::CaseSensitive));
		const FString CodeFix = FFlockErrorHints::For(Result.Error.ErrorCode);
		TestTrue(FString::Printf(TEXT("%s: and keeps the refusal's own fix"), *Case),
			CodeFix.IsEmpty() || Result.Error.Hint.Contains(CodeFix, ESearchCase::CaseSensitive));
	}
	return true;
}

// The doubt sits in the middle: a 429 (not processed), a timeout, then 429s. A rule that judged only the first attempt and the
// last would read it as certain.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPurchaseDoubtInTheMiddleTest, "Flock.Shop.Money.PurchaseAnAttemptInDoubtBetweenOnesNotProcessedIsUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPurchaseDoubtInTheMiddleTest::RunTest(const FString& Parameters)
{
	FFixture Fx(2);
	Fx.SignIn();
	Fx.WithItem();
	Fx.Fake->OnSequence(FlockEndpoints::ShopTransaction, { TooMany(), FFlockFakeTransport::Timeout(), TooMany() });

	TFlockResult<FFlockPurchaseResult> Result;
	Fx.Buy(Result);
	TestEqual(TEXT("control: three attempts"), Fx.Fake->CountTo(FlockEndpoints::ShopTransaction), 3);
	TestEqual(TEXT("ends on the last 429"), Result.Error.StatusCode, 429);
	TestTrue(TEXT("unknown: the middle attempt may have bought it"), Result.Error.bOutcomeUnknown);
	return true;
}

// A 4xx is the server's answer even with no code, and a server failure naming its cause refused before buying anything.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPurchaseCertainRefusalsTest, "Flock.Shop.Money.PurchaseRefusalsWithAnAnswerAreCertain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPurchaseCertainRefusalsTest::RunTest(const FString& Parameters)
{
	const TPair<FFlockHttpResponse, const TCHAR*> Answers[] = {
		{ FFlockFakeTransport::Coded(400, TEXT("shop.insufficient_funds")), TEXT("not enough funds") },
		{ FFlockFakeTransport::Status(409, TEXT("{}")), TEXT("a 409 naming no code") },
		{ FFlockFakeTransport::Coded(503, TEXT("shop.real_currency_settlement_unavailable")), TEXT("a 503 naming a game setting") },
	};
	for (const TPair<FFlockHttpResponse, const TCHAR*>& Answer : Answers)
	{
		FFixture Fx(3);
		Fx.SignIn();
		Fx.WithItem();
		Fx.Fake->On(FlockEndpoints::ShopTransaction, Answer.Key);

		TFlockResult<FFlockPurchaseResult> Result;
		Fx.Buy(Result);
		TestFalse(FString::Printf(TEXT("%s: refused"), Answer.Value), Result.bSuccess);
		TestFalse(FString::Printf(TEXT("%s: certain, nothing was bought"), Answer.Value), Result.Error.bOutcomeUnknown);
		TestEqual(FString::Printf(TEXT("%s: not sent again"), Answer.Value), Fx.Fake->CountTo(FlockEndpoints::ShopTransaction), 1);
	}
	return true;
}

// A cancel while the attempt is still out ends as a cancel, as one between attempts with nothing in doubt does: no answer was
// had at all. The request may have gone out, so the cached player data is dropped.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockMoneyCallCancelInFlightTest, "Flock.Shop.Money.ACancelWhileTheAttemptIsOutIsACancelAndDropsTheCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockMoneyCallCancelInFlightTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	FMoneyRuleProvider Provider(Fx.Client, Policy(3), MakeShared<FFlockNullLogger>());
	// The fake cannot abort a request it holds: the engine answers an aborted one as cancelled, so this answer stands in for it.
	FFlockHttpResponse Aborted;
	Aborted.Result = EFlockHttpResult::Cancelled;
	Fx.Fake->On(TEXT("money"), Aborted);
	Fx.Fake->bDeferred = true;
	int32 Forgotten = 0;
	FMoneyRuleProvider::FMoneyCall Rules;
	Rules.ForgetPlayersData = [&Forgotten]() { ++Forgotten; };
	Rules.FixWhenUnknown = TEXT("read first");
	const TSharedRef<FFlockHttpClient> ClientRef = Fx.Client;
	TFlockResult<FFlockPlayerData> Result;
	bool bAnswered = false;
	FFlockRequestHandle Handle = Provider.ExecuteMoneyCall<FFlockPlayerData>(
		[ClientRef](TFunction<void(TFlockResult<FFlockPlayerData>)> OnAttempt)
		{
			return ClientRef->PostJsonRaw<FFlockPlayerData>(TEXT("http://x/v1/money"), TMap<FString, FString>(), TEXT("{}"), MoveTemp(OnAttempt));
		},
		[&](TFlockResult<FFlockPlayerData> R) { Result = R; bAnswered = true; }, TEXT("Money"), true, Rules);
	Handle.Cancel();
	Fx.Fake->FlushPending();
	FlockTestRetries::Pump();

	TestTrue(TEXT("answered"), bAnswered);
	TestEqual(TEXT("a cancel"), Result.Error.Type, EFlockErrorType::Cancelled);
	TestFalse(TEXT("not marked unknown"), Result.Error.bOutcomeUnknown);
	TestTrue(TEXT("the cached player data is dropped"), Forgotten > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockReadNoAnswerNotUnknownTest, "Flock.Shop.Money.AReadWithNoAnswerIsNotUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockReadNoAnswerNotUnknownTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.Fake->On(FlockEndpoints::ShopItemById(TEXT("item-1")), ServerError());

	TFlockResult<FFlockShopItem> Item;
	Fx.Shop->GetItem(TEXT("item-1"), [&](TFlockResult<FFlockShopItem> R) { Item = R; });
	TestFalse(TEXT("failed"), Item.bSuccess);
	TestFalse(TEXT("a read moves nothing, so its outcome is never unknown"), Item.Error.bOutcomeUnknown);

	// And a purchase whose item read fails sent nothing, so it is certain too.
	TFlockResult<FFlockPurchaseResult> Bought;
	Fx.Buy(Bought);
	TestFalse(TEXT("a purchase that never sent is not unknown"), Bought.Error.bOutcomeUnknown);
	TestEqual(TEXT("nothing sent"), Fx.Fake->CountTo(FlockEndpoints::ShopTransaction), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRedeemNoAnswerUnknownTest, "Flock.Shop.Money.RedeemServerErrorOnEveryAttemptIsUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRedeemNoAnswerUnknownTest::RunTest(const FString& Parameters)
{
	FFixture Fx(2);
	Fx.SignIn();
	TestTrue(TEXT("the wallet is cached first"), Fx.CacheWallet(TEXT("250")));
	Fx.Fake->On(FlockEndpoints::ShopValidateReceipt, ServerError());

	TFlockResult<FFlockPurchaseResult> Result;
	Fx.Shop->RedeemStoreReceipt(TEXT("item-1"), FFlockStoreReceipt::FromAppleAppStore(TEXT("2000000123456789")),
		[&](TFlockResult<FFlockPurchaseResult> R) { Result = R; });
	FlockTestRetries::Pump();
	TestFalse(TEXT("not redeemed"), Result.bSuccess);
	TestTrue(TEXT("unknown"), Result.Error.bOutcomeUnknown);
	TestTrue(TEXT("its fix says to send the same receipt again"), Result.Error.Hint.Contains(TEXT("send the same receipt again"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("sent on every attempt: a redemption may be sent again"), Fx.Fake->CountTo(FlockEndpoints::ShopValidateReceipt), 3);
	TestFalse(TEXT("the cached wallet is dropped"), Fx.WalletIsCached());
	return true;
}

// A server failure that names its cause (a setting someone has to fix) was refused before anything was done: it is certain.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRedeemCodedServerFailureTest, "Flock.Shop.Money.ARefusalOnACodedServerFailureIsCertain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRedeemCodedServerFailureTest::RunTest(const FString& Parameters)
{
	FFixture Fx(2);
	Fx.SignIn();
	Fx.Fake->On(FlockEndpoints::ShopValidateReceipt, FFlockFakeTransport::Status(503,
		TEXT("{\"detail\":{\"code\":\"payment_provider.credentials_unreadable\",\"message\":\"Apple credentials could not be used to sign a request\"}}")));

	TFlockResult<FFlockPurchaseResult> Result;
	Fx.Shop->RedeemStoreReceipt(TEXT("item-1"), FFlockStoreReceipt::FromAppleAppStore(TEXT("2000000123456789")),
		[&](TFlockResult<FFlockPurchaseResult> R) { Result = R; });
	FlockTestRetries::Pump();
	TestEqual(TEXT("refused on its code"), Result.Error.ErrorCode, EFlockErrorCode::PaymentProviderCredentialsUnreadable);
	TestFalse(TEXT("certain: the server refused before asking the store"), Result.Error.bOutcomeUnknown);
	TestEqualSensitive(TEXT("with the code's own fix"), Result.Error.Hint, FFlockErrorHints::For(EFlockErrorCode::PaymentProviderCredentialsUnreadable));
	TestEqual(TEXT("not sent again"), Fx.Fake->CountTo(FlockEndpoints::ShopValidateReceipt), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConsumeNamingNoEntryTest, "Flock.Shop.Money.ConsumeAnswerNamingNoEntryIsRefusedAndUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConsumeNamingNoEntryTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.Fake->On(FlockEndpoints::PlayerInventoryConsume(TEXT("inv-1")), FFlockFakeTransport::Ok(TEXT("{\"granted\":[]}")));

	TFlockResult<FFlockConsumeResult> Result;
	Fx.Shop->Consume(TEXT("inv-1"), [&](TFlockResult<FFlockConsumeResult> R) { Result = R; });
	TestFalse(TEXT("a 2xx naming no entry is not a use"), Result.bSuccess);
	TestEqual(TEXT("an unreadable answer"), Result.Error.Type, EFlockErrorType::Serialization);
	TestTrue(TEXT("whether it was used is unknown"), Result.Error.bOutcomeUnknown);
	TestTrue(TEXT("its fix says to read the inventory"), Result.Error.Hint.Contains(TEXT("Read the player's inventory"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConsumeNoAnswerTest, "Flock.Shop.Money.ConsumeWithNoAnswerIsUnknownAndNotSentAgain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConsumeNoAnswerTest::RunTest(const FString& Parameters)
{
	FFixture Fx(3);
	Fx.SignIn();
	Fx.Fake->On(FlockEndpoints::PlayerInventoryConsume(TEXT("inv-1")), FFlockFakeTransport::Offline());

	TFlockResult<FFlockConsumeResult> Result;
	Fx.Shop->Consume(TEXT("inv-1"), [&](TFlockResult<FFlockConsumeResult> R) { Result = R; });
	FlockTestRetries::Pump();
	TestTrue(TEXT("unknown"), Result.Error.bOutcomeUnknown);
	TestEqual(TEXT("a consume is never sent again after no answer"), Fx.Fake->CountTo(TEXT("/consume")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockFundGrantNoAnswerTest, "Flock.Shop.Money.FundGrantWithNoAnswerIsUnknownAndDropsTheWallet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockFundGrantNoAnswerTest::RunTest(const FString& Parameters)
{
	FFixture Fx(3);
	Fx.SignIn();
	TestTrue(TEXT("the wallet is cached first"), Fx.CacheWallet(TEXT("250")));
	const TSharedRef<FFlockCommandProvider> Commands = MakeShared<FFlockCommandProvider>(Fx.Client, Policy(3),
		MakeShared<FFlockNullLogger>(), Fx.Session, TEXT("http://x/v1"), Fx.Snapshot, TEXT("ver-1"));
	Commands->SetPlayerProvider(Fx.Players);
	Fx.Fake->On(FlockEndpoints::CommandAddGameFunds, BadGateway());

	TFlockResult<FFlockPlayerData> Result;
	bool bAnswered = false;
	Commands->AddGameFunds(TEXT("Gold"), 50, TEXT("tpl-wallet"), [&](TFlockResult<FFlockPlayerData> R) { Result = R; bAnswered = true; });
	FlockTestRetries::Pump();
	TestTrue(TEXT("the grant answered"), bAnswered);
	TestTrue(TEXT("unknown: the funds may have been added"), Result.Error.bOutcomeUnknown);
	TestTrue(TEXT("its fix says to read the wallet first"), Result.Error.Hint.Contains(TEXT("Read the player's wallet"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("never sent again"), Fx.Fake->CountTo(FlockEndpoints::CommandAddGameFunds), 1);
	TestFalse(TEXT("the cached wallet is dropped"), Fx.WalletIsCached());
	return true;
}

// The game's own cancel, through the call's handle, after an attempt in doubt: "cancelled" never reads as "nothing done".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockMoneyCallCancelAfterDoubtTest, "Flock.Shop.Money.ACancelAfterAnAttemptInDoubtEndsWithThatAttemptUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockMoneyCallCancelAfterDoubtTest::RunTest(const FString& Parameters)
{
	for (const bool bInDoubt : { true, false })
	{
		FFixture Fx;
		FMoneyRuleProvider Provider(Fx.Client, Policy(3), MakeShared<FFlockNullLogger>());
		Fx.Fake->OnSequence(TEXT("money"), { bInDoubt ? BadGateway() : TooMany(),
			FFlockFakeTransport::Ok(TEXT("{}")) });
		int32 Forgotten = 0;
		FMoneyRuleProvider::FMoneyCall Rules;
		Rules.ForgetPlayersData = [&Forgotten]() { ++Forgotten; };
		Rules.FixWhenUnknown = TEXT("read first");
		const TSharedRef<FFlockHttpClient> ClientRef = Fx.Client;
		TFlockResult<FFlockPlayerData> Result;
		FFlockRequestHandle Handle = Provider.ExecuteMoneyCall<FFlockPlayerData>(
			[ClientRef](TFunction<void(TFlockResult<FFlockPlayerData>)> OnAttempt)
			{
				return ClientRef->PostJsonRaw<FFlockPlayerData>(TEXT("http://x/v1/money"), TMap<FString, FString>(), TEXT("{}"), MoveTemp(OnAttempt));
			},
			[&Result](TFlockResult<FFlockPlayerData> R) { Result = R; }, TEXT("Money"), true, Rules);
		Handle.Cancel();
		FlockTestRetries::Pump();

		const FString Case = bInDoubt ? TEXT("after a 502") : TEXT("after a 429");
		TestEqual(FString::Printf(TEXT("%s: sent once"), *Case), Fx.Fake->CountTo(TEXT("money")), 1);
		TestEqual(FString::Printf(TEXT("%s: the cached player data is dropped"), *Case), Forgotten > 0, true);
		if (bInDoubt)
		{
			TestEqual(TEXT("after a 502: ends with that attempt's failure"), Result.Error.StatusCode, 502);
			TestTrue(TEXT("after a 502: unknown"), Result.Error.bOutcomeUnknown);
			TestEqualSensitive(TEXT("after a 502: with the call's fix"), Result.Error.Hint, FString(TEXT("read first")));
		}
		else
		{
			TestEqual(TEXT("after a 429: stays a cancel"), Result.Error.Type, EFlockErrorType::Cancelled);
			TestFalse(TEXT("after a 429: not unknown"), Result.Error.bOutcomeUnknown);
		}
	}
	return true;
}

// ───────────────────────────── Owned counts and owns ─────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockOwnedCountsEveryPageTest, "Flock.Shop.Owned.CountsReadEveryPageAndCountOnlyUnusedEntries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockOwnedCountsEveryPageTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.WithTwoInventoryPages();

	TFlockResult<FFlockOwnedItemCounts> Owned;
	TestTrue(TEXT("answered"), Fx.OwnedCounts(Owned));
	TestTrue(TEXT("read"), Owned.bSuccess);
	TestEqual(TEXT("99 potions"), Owned.Value.CountOf(TEXT("potion")), 99);
	TestEqual(TEXT("the used sword does not count; the one on the second page does"), Owned.Value.CountOf(TEXT("sword")), 1);
	TestEqual(TEXT("a refunded entry is not owned"), Owned.Value.CountOf(TEXT("shield")), 0);
	TestEqual(TEXT("one row per item held"), Owned.Value.Items.Num(), 2);
	TestEqual(TEXT("both pages were read, and no more"), Fx.Fake->CountTo(InventoryRoute()), 2);
	return true;
}

// Exactly one full page: the total says there is no second, so none is read.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockOwnedCountsExactlyFullPageTest, "Flock.Shop.Owned.AFullPageThatIsTheWholeInventoryIsReadOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockOwnedCountsExactlyFullPageTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	TArray<FString> Entries;
	for (int32 Index = 0; Index < 100; ++Index)
	{
		Entries.Add(InventoryEntry(FString::Printf(TEXT("inv-p%d"), Index), TEXT("potion"), TEXT("unused")));
	}
	Fx.Fake->On(InventoryRoute() + TEXT("?page=1&limit=100"), FFlockFakeTransport::Ok(InventoryPage(1, 100, Entries)));
	Fx.Fake->On(InventoryRoute() + TEXT("?page=2&limit=100"), FFlockFakeTransport::Ok(InventoryPage(2, 100, {})));

	TFlockResult<FFlockOwnedItemCounts> Owned;
	Fx.OwnedCounts(Owned);
	TestEqual(TEXT("100 potions"), Owned.Value.CountOf(TEXT("potion")), 100);
	TestEqual(TEXT("one read"), Fx.Fake->CountTo(InventoryRoute()), 1);
	return true;
}

// A purchase landing between two page reads shifts the pages (newest first, by offset): the last entry of the first page comes
// back as the first of the second. It counts once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockOwnedCountsEntryOnTwoPagesTest, "Flock.Shop.Owned.AnEntryHandedBackOnTwoPagesCountsOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockOwnedCountsEntryOnTwoPagesTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	TArray<FString> First;
	for (int32 Index = 0; Index < 99; ++Index)
	{
		First.Add(InventoryEntry(FString::Printf(TEXT("inv-p%d"), Index), TEXT("potion"), TEXT("unused")));
	}
	First.Add(InventoryEntry(TEXT("inv-s1"), TEXT("sword"), TEXT("unused")));
	const TArray<FString> Second = { InventoryEntry(TEXT("inv-s1"), TEXT("sword"), TEXT("unused")),
		InventoryEntry(TEXT("inv-s2"), TEXT("sword"), TEXT("unused")) };
	Fx.Fake->On(InventoryRoute() + TEXT("?page=1&limit=100"), FFlockFakeTransport::Ok(InventoryPage(1, 102, First)));
	Fx.Fake->On(InventoryRoute() + TEXT("?page=2&limit=100"), FFlockFakeTransport::Ok(InventoryPage(2, 102, Second)));

	TFlockResult<FFlockOwnedItemCounts> Owned;
	Fx.OwnedCounts(Owned);
	TestEqual(TEXT("two swords, not three"), Owned.Value.CountOf(TEXT("sword")), 2);
	TestEqual(TEXT("99 potions"), Owned.Value.CountOf(TEXT("potion")), 99);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockOwnsTest, "Flock.Shop.Owned.OwnsIsTrueOnlyForAnItemWithAnUnusedEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockOwnsTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.WithTwoInventoryPages();

	auto Owns = [&Fx](const FString& Id)
	{
		TFlockResult<bool> Result;
		Fx.Shop->Owns(Id, [&Result](TFlockResult<bool> R) { Result = R; });
		return Result;
	};
	TestTrue(TEXT("sword"), Owns(TEXT("sword")).Value);
	TestFalse(TEXT("refunded"), Owns(TEXT("shield")).Value);
	TestFalse(TEXT("never bought"), Owns(TEXT("never-bought")).Value);
	TestFalse(TEXT("an id matches with its letter case"), Owns(TEXT("SWORD")).Value);
	const TFlockResult<bool> Empty = Owns(FString());
	TestFalse(TEXT("an empty id is refused"), Empty.bSuccess);
	TestEqual(TEXT("as a validation failure"), Empty.Error.Type, EFlockErrorType::Validation);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockOwnedCountsEmptyTest, "Flock.Shop.Owned.EmptyInventoryIsEmptyAfterOneRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockOwnedCountsEmptyTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.InventoryOf({});

	TFlockResult<FFlockOwnedItemCounts> Owned;
	Fx.OwnedCounts(Owned);
	TestTrue(TEXT("read"), Owned.bSuccess);
	TestEqual(TEXT("nothing held"), Owned.Value.Items.Num(), 0);
	TestEqual(TEXT("one read"), Fx.Fake->CountTo(InventoryRoute()), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockOwnedCountsSignedOutTest, "Flock.Shop.Owned.CountsNotSignedInFailBeforeSending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockOwnedCountsSignedOutTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.InventoryOf({ InventoryEntry(TEXT("inv-p1"), TEXT("potion"), TEXT("unused")) });

	TFlockResult<FFlockOwnedItemCounts> Owned;
	TestTrue(TEXT("answered"), Fx.OwnedCounts(Owned));
	TestFalse(TEXT("refused"), Owned.bSuccess);
	TestTrue(TEXT("with the sign-in fix"), Owned.Error.Hint.Contains(FFlockErrorHints::SignInFirst(), ESearchCase::CaseSensitive));
	TestEqual(TEXT("nothing sent"), Fx.Fake->Requests.Num(), 0);
	return true;
}

// ───────────────────────────── Consume one ─────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConsumeOneFirstUnusedTest, "Flock.Shop.ConsumeOne.UsesTheFirstUnusedEntryOfThatItem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConsumeOneFirstUnusedTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.InventoryOf({ InventoryEntry(TEXT("inv-used"), TEXT("potion"), TEXT("used")), InventoryEntry(TEXT("inv-sword"), TEXT("sword"), TEXT("unused")),
		InventoryEntry(TEXT("inv-p1"), TEXT("potion"), TEXT("unused")), InventoryEntry(TEXT("inv-p2"), TEXT("potion"), TEXT("unused")) });
	Fx.Fake->On(FlockEndpoints::PlayerInventoryConsume(TEXT("inv-p1")), FFlockFakeTransport::Ok(ConsumedAnswer(TEXT("inv-p1"))));

	TFlockResult<FFlockConsumeResult> Result;
	TestTrue(TEXT("answered"), Fx.ConsumeOne(TEXT("potion"), Result));
	TestTrue(TEXT("used"), Result.bSuccess);
	TestEqualSensitive(TEXT("the first unused potion"), Result.Value.Inventory.Id, FString(TEXT("inv-p1")));
	TestEqual(TEXT("what it granted"), FlockTestAt(Result.Value.Granted, 0).Amount, 10);
	TestEqual(TEXT("one entry used, not a used one, not another item's"), Fx.Fake->CountTo(TEXT("/consume")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConsumeOneUsedMeanwhileTest, "Flock.Shop.ConsumeOne.AnEntryUsedMeanwhileIsPassedOverForTheNext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConsumeOneUsedMeanwhileTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.InventoryOf({ InventoryEntry(TEXT("inv-p1"), TEXT("potion"), TEXT("unused")), InventoryEntry(TEXT("inv-p2"), TEXT("potion"), TEXT("unused")) });
	Fx.Fake->On(FlockEndpoints::PlayerInventoryConsume(TEXT("inv-p1")), FFlockFakeTransport::Coded(409, TEXT("player_inventory.already_used")));
	Fx.Fake->On(FlockEndpoints::PlayerInventoryConsume(TEXT("inv-p2")), FFlockFakeTransport::Ok(ConsumedAnswer(TEXT("inv-p2"))));

	TFlockResult<FFlockConsumeResult> Result;
	Fx.ConsumeOne(TEXT("potion"), Result);
	TestTrue(TEXT("used"), Result.bSuccess);
	TestEqualSensitive(TEXT("another device used the first since the read; the second is still the player's"), Result.Value.Inventory.Id,
		FString(TEXT("inv-p2")));
	TestFalse(TEXT("the entry passed over is a step of the call, not an error in the log"),
		FFlockRecordingLogger::AnyContains(Fx.Log->Errors, TEXT("player_inventory.already_used")));
	TestTrue(TEXT("it is still in the log, at debug level"), FFlockRecordingLogger::AnyContains(Fx.Log->Debugs, TEXT("player_inventory.already_used")));
	// Control: Consume itself, asked for a used entry, reports the refusal as an error.
	TFlockResult<FFlockConsumeResult> Direct;
	Fx.Shop->Consume(TEXT("inv-p1"), [&Direct](TFlockResult<FFlockConsumeResult> R) { Direct = R; });
	TestTrue(TEXT("control: a direct consume of a used entry is an error in the log"),
		FFlockRecordingLogger::AnyContains(Fx.Log->Errors, TEXT("player_inventory.already_used")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConsumeOneNoAnswerStopsTest, "Flock.Shop.ConsumeOne.NoAnswerStopsWithoutTryingAnotherEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConsumeOneNoAnswerStopsTest::RunTest(const FString& Parameters)
{
	FFixture Fx(3);
	Fx.SignIn();
	Fx.InventoryOf({ InventoryEntry(TEXT("inv-p1"), TEXT("potion"), TEXT("unused")), InventoryEntry(TEXT("inv-p2"), TEXT("potion"), TEXT("unused")) });
	Fx.Fake->On(FlockEndpoints::PlayerInventoryConsume(TEXT("inv-p1")), BadGateway());
	Fx.Fake->On(FlockEndpoints::PlayerInventoryConsume(TEXT("inv-p2")), FFlockFakeTransport::Ok(ConsumedAnswer(TEXT("inv-p2"))));

	TFlockResult<FFlockConsumeResult> Result;
	Fx.ConsumeOne(TEXT("potion"), Result);
	TestFalse(TEXT("not used"), Result.bSuccess);
	TestTrue(TEXT("unknown"), Result.Error.bOutcomeUnknown);
	TestEqual(TEXT("sent once, and no other entry tried"), Fx.Fake->CountTo(TEXT("/consume")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConsumeOneEveryPageTest, "Flock.Shop.ConsumeOne.ReadsEveryPage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConsumeOneEveryPageTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.WithTwoInventoryPages();
	Fx.Fake->On(FlockEndpoints::PlayerInventoryConsume(TEXT("inv-s2")), FFlockFakeTransport::Ok(ConsumedAnswer(TEXT("inv-s2"))));

	TFlockResult<FFlockConsumeResult> Result;
	Fx.ConsumeOne(TEXT("sword"), Result);
	TestEqualSensitive(TEXT("the only unused sword is on the second page"), Result.Value.Inventory.Id, FString(TEXT("inv-s2")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConsumeOneNoneHeldTest, "Flock.Shop.ConsumeOne.NoneHeldIsRefusedAndNothingIsUsed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConsumeOneNoneHeldTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.InventoryOf({ InventoryEntry(TEXT("inv-used"), TEXT("potion"), TEXT("used")), InventoryEntry(TEXT("inv-sword"), TEXT("sword"), TEXT("unused")) });

	TFlockResult<FFlockConsumeResult> Result;
	Fx.ConsumeOne(TEXT("potion"), Result);
	TestEqual(TEXT("refused"), Result.Error.Type, EFlockErrorType::Validation);
	TestTrue(TEXT("its fix names the owned counts"), Result.Error.Hint.Contains(TEXT("GetOwnedItemCounts"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("nothing used"), Fx.Fake->CountTo(TEXT("/consume")), 0);
	// An id matches with its case: "POTION" is not the potion.
	TFlockResult<FFlockConsumeResult> OtherCase;
	Fx.InventoryOf({ InventoryEntry(TEXT("inv-p1"), TEXT("potion"), TEXT("unused")) });
	Fx.ConsumeOne(TEXT("POTION"), OtherCase);
	TestEqual(TEXT("another letter case is another item"), OtherCase.Error.Type, EFlockErrorType::Validation);
	TestEqual(TEXT("still nothing used"), Fx.Fake->CountTo(TEXT("/consume")), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConsumeOneSignedOutTest, "Flock.Shop.ConsumeOne.NotSignedInIsRefusedBeforeSending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConsumeOneSignedOutTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.InventoryOf({ InventoryEntry(TEXT("inv-p1"), TEXT("potion"), TEXT("unused")) });

	TFlockResult<FFlockConsumeResult> Result;
	TestTrue(TEXT("answered"), Fx.ConsumeOne(TEXT("potion"), Result));
	TestEqual(TEXT("refused as a sign-in failure"), Result.Error.Type, EFlockErrorType::Auth);
	TestEqual(TEXT("nothing sent"), Fx.Fake->Requests.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConsumeOnePlayerChangesTest, "Flock.Shop.ConsumeOne.PlayerChangesWhileTheInventoryIsReadNothingIsUsed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConsumeOnePlayerChangesTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn(TEXT("player-a"));
	Fx.InventoryOf({ InventoryEntry(TEXT("inv-p1"), TEXT("potion"), TEXT("unused")) });
	Fx.Fake->On(FlockEndpoints::PlayerInventoryConsume(TEXT("inv-p1")), FFlockFakeTransport::Ok(ConsumedAnswer(TEXT("inv-p1"))));
	Fx.Fake->bDeferred = true;

	TFlockResult<FFlockConsumeResult> Result;
	bool bAnswered = false;
	Fx.Shop->ConsumeOne(TEXT("potion"), [&](TFlockResult<FFlockConsumeResult> R) { Result = R; bAnswered = true; });
	Fx.SignIn(TEXT("player-b"));
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Fx.Fake->FlushPending();
	}

	TestTrue(TEXT("the use ended"), bAnswered);
	TestFalse(TEXT("not used for the player who signed in next"), Result.bSuccess);
	TestEqual(TEXT("nothing sent to use it"), Fx.Fake->CountTo(TEXT("/consume")), 0);
	return true;
}

// ───────────────────────────── The wallet ─────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockWalletBalancesTest, "Flock.Player.Wallet.ReadsEachCurrencysBalanceFromTheCurrencyTemplatesRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockWalletBalancesTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.WithRows({ ProgressRow(), WalletRow() });

	TFlockResult<FFlockWallet> Read;
	TestTrue(TEXT("answered"), Fx.ReadWallet(Read));
	TestTrue(TEXT("read"), Read.bSuccess);
	const FFlockWallet& Wallet = Read.Value;
	TestEqual(TEXT("Gold"), Wallet.BalanceOf(TEXT("Gold")), static_cast<int64>(250));
	TestEqual(TEXT("Gems"), Wallet.BalanceOf(TEXT("Gems")), static_cast<int64>(0));
	TestTrue(TEXT("a zero balance is still held"), Wallet.Holds(TEXT("Gems")));
	TestEqual(TEXT("SAR is a real currency"), Wallet.RealCurrencies.Num(), 1);
	TestEqual(TEXT("its balance"), Wallet.BalanceOf(TEXT("SAR")), static_cast<int64>(1200));
	TestFalse(TEXT("real currencies are kept apart from the game's own"),
		Wallet.GameCurrencies.ContainsByPredicate([](const FFlockCurrencyBalance& B) { return B.Code == TEXT("SAR"); }));
	TestEqual(TEXT("not held: 0"), Wallet.BalanceOf(TEXT("Shard")), static_cast<int64>(0));
	TestFalse(TEXT("not held"), Wallet.Holds(TEXT("Shard")));
	TestFalse(TEXT("codes are matched exactly, as the server matches them"), Wallet.Holds(TEXT("gold")));
	TestEqual(TEXT("and the balance of another case is none"), Wallet.BalanceOf(TEXT("gold")), static_cast<int64>(0));
	return true;
}

// A code is kept exactly as the server sends it, even one the flattened player data would recase.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockWalletExactCodesTest, "Flock.Player.Wallet.CodesAreKeptExactlyAsTheServerSendsThem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockWalletExactCodesTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	const FString Row = WalletRow().Replace(*Currency(TEXT("Gems"), TEXT("0")), *Currency(TEXT("gold_coin"), TEXT("5")), ESearchCase::CaseSensitive);
	Fx.WithRows({ Row });

	TFlockResult<FFlockWallet> Read;
	Fx.ReadWallet(Read);
	TestEqual(TEXT("a lower-case code with an underscore"), Read.Value.BalanceOf(TEXT("gold_coin")), static_cast<int64>(5));
	TestFalse(TEXT("never under the recased name"), Read.Value.Holds(TEXT("GoldCoin")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockWalletCanAffordTest, "Flock.Player.Wallet.CanAffordOnlyAGameCurrencyItemItsBalanceCovers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockWalletCanAffordTest::RunTest(const FString& Parameters)
{
	const FFlockWallet Wallet = FFlockWallet::From(WalletWithGold(TEXT("250")));
	auto Item = [](const FString& Code, int32 Price, bool bRealMoney)
	{
		FFlockShopItem Shop;
		Shop.Id = TEXT("item");
		Shop.Currency = Code;
		Shop.Price = Price;
		Shop.IsRealMoney = bRealMoney;
		return Shop;
	};
	TestTrue(TEXT("exactly the balance"), Wallet.CanAfford(Item(TEXT("Gold"), 250, false)));
	TestFalse(TEXT("one more than the balance"), Wallet.CanAfford(Item(TEXT("Gold"), 251, false)));
	TestFalse(TEXT("a currency the wallet does not hold buys nothing, even at no cost"), Wallet.CanAfford(Item(TEXT("Shard"), 0, false)));
	TestFalse(TEXT("another letter case"), Wallet.CanAfford(Item(TEXT("gold"), 1, false)));
	TestFalse(TEXT("a real-money item is never bought from the wallet, even priced in a game currency its balance covers"),
		Wallet.CanAfford(Item(TEXT("Gold"), 1, true)));
	TestFalse(TEXT("nor a real currency's balance spent on a game item"), Wallet.CanAfford(Item(TEXT("SAR"), 1, false)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockWalletSignedOutTest, "Flock.Player.Wallet.NotSignedInFailsBeforeReadingPlayerData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockWalletSignedOutTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.WithRows({ WalletRow() });
	TFlockResult<FFlockWallet> Read;
	TestTrue(TEXT("answered"), Fx.ReadWallet(Read));
	TestFalse(TEXT("refused"), Read.bSuccess);
	TestEqual(TEXT("nothing read"), Fx.Fake->CountTo(TEXT("player_data?page=")), 0);
	return true;
}

// A purchase's or consume's wallet is the same row, read the same way; a result that moved no currency reads as an empty wallet.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockWalletFromAResultTest, "Flock.Player.Wallet.APurchasesWalletReadsTheSameShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockWalletFromAResultTest::RunTest(const FString& Parameters)
{
	FFlockPurchaseResult Bought;
	Bought.Wallet = WalletWithGold(TEXT("250"));
	TestEqual(TEXT("Purchase Balances"), UFlockShopLibrary::PurchaseBalances(Bought).BalanceOf(TEXT("Gold")), static_cast<int64>(250));
	FFlockConsumeResult Used;
	Used.Wallet = WalletWithGold(TEXT("480"));
	TestEqual(TEXT("Consume Balances"), UFlockShopLibrary::ConsumeBalances(Used).BalanceOf(TEXT("Gold")), static_cast<int64>(480));
	TestEqual(TEXT("no wallet: no balances"), UFlockShopLibrary::PurchaseBalances(FFlockPurchaseResult()).GameCurrencies.Num(), 0);

	// An older row's form, a plain object per category, reads the same codes.
	FFlockPlayerData Older;
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(
		TEXT("{\"id\":\"pd-wallet\",\"player_template_id\":\"tpl-wallet\",\"player_id\":\"player-a\",")
		TEXT("\"data\":{\"game_currencies\":{\"gold_coin\":7},\"real_currencies\":{\"SAR\":1200}}}"));
	FString Error;
	if (TestTrue(TEXT("the older row parses"), FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid()
		&& FFlockPlayerData::FromWireObject(Object.ToSharedRef(), Older, Error)))
	{
		const FFlockWallet Wallet = FFlockWallet::From(Older);
		TestEqual(TEXT("its game currency, code as sent"), Wallet.BalanceOf(TEXT("gold_coin")), static_cast<int64>(7));
		TestEqual(TEXT("its real currency"), Wallet.RealCurrencies.Num(), 1);
	}
	return true;
}

// Two money answers arriving in the opposite order to the one the server wrote them in: the older never replaces the newer.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockWalletOlderRowArrivingLateTest, "Flock.Player.Wallet.AnOlderServerRowArrivingLateDoesNotReplaceANewerOne",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockWalletOlderRowArrivingLateTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	TestTrue(TEXT("the wallet is cached first"), Fx.CacheWallet(TEXT("250")));
	FFlockPlayerData Newer = WalletWithGold(TEXT("210"));
	Newer.UpdatedAt = TEXT("2026-11-08T10:00:02.654321");
	FFlockPlayerData Older = WalletWithGold(TEXT("230"));
	Older.UpdatedAt = TEXT("2026-11-08T10:00:01.123456");

	Fx.Players->ApplyServerPlayerData(Newer);
	Fx.Players->ApplyServerPlayerData(Older);
	TFlockResult<FFlockWallet> Read;
	Fx.ReadWallet(Read);
	TestEqual(TEXT("the newer balance stays"), GoldOf(Read), static_cast<int64>(210));

	FFlockPlayerData Later = WalletWithGold(TEXT("190"));
	Later.UpdatedAt = TEXT("2026-11-08T10:00:03.000001");
	Fx.Players->ApplyServerPlayerData(Later);
	Fx.ReadWallet(Read);
	TestEqual(TEXT("control: a newer row replaces it"), GoldOf(Read), static_cast<int64>(190));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockWalletNoneTest, "Flock.Player.Wallet.APlayerWithNoWalletRowIsRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockWalletNoneTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.WithRows({ ProgressRow() });

	TFlockResult<FFlockWallet> Read;
	Fx.ReadWallet(Read);
	TestFalse(TEXT("refused"), Read.bSuccess);
	TestEqual(TEXT("as a validation failure"), Read.Error.Type, EFlockErrorType::Validation);
	TestTrue(TEXT("its fix names the currency template"), Read.Error.Hint.Contains(TEXT("currency template"), ESearchCase::CaseSensitive));
	return true;
}

// ───────────────────────────── A player-data read in flight across a drop or a server row ─────────────────────────────

namespace FlockWalletShopScreenTestHelpers
{
	/** The player's data reads, held: the first answers the wallet with 250 Gold, every later one with 80. */
	inline void FirstReadOlderThanTheRest(FFixture& Fx)
	{
		Fx.Fake->On(FlockEndpoints::PlayerTemplate, FFlockFakeTransport::Ok(TemplateList()));
		Fx.Fake->OnSequence(TEXT("player_data?page="), { FFlockFakeTransport::Ok(RowsPage({ WalletRow(TEXT("250")) })),
			FFlockFakeTransport::Ok(RowsPage({ WalletRow(TEXT("80")) })) });
		// The templates are read first, so only the player's data reads are held.
		TFlockResult<TArray<FFlockPlayerTemplateSchema>> Templates;
		Fx.Players->GetTemplates([&Templates](TFlockResult<TArray<FFlockPlayerTemplateSchema>> R) { Templates = R; });
		Fx.Fake->bDeferred = true;
	}


	/** Reads the wallet with the transport answering at once. */
	inline TFlockResult<FFlockWallet> ReadNow(FFixture& Fx)
	{
		Fx.Fake->bDeferred = false;
		TFlockResult<FFlockWallet> Read;
		Fx.ReadWallet(Read);
		return Read;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerDataReadAcrossADropTest, "Flock.Player.Wallet.AReadOnItsWayWhenThePlayersDataIsDroppedDoesNotPutTheOldDataBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerDataReadAcrossADropTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	FirstReadOlderThanTheRest(Fx);

	TFlockResult<FFlockWallet> Before;
	Fx.ReadWallet(Before);
	TestEqual(TEXT("the first read is on its way"), Fx.Fake->CountTo(TEXT("player_data?page=")), 1);
	Fx.Players->ClearPlayerDataCache();
	Fx.Fake->FlushPending();

	TestEqual(TEXT("control: the earlier read answered with the earlier balance, to its caller"), GoldOf(Before), static_cast<int64>(250));
	TestEqual(TEXT("the earlier answer was not kept: the next read asked the server"), GoldOf(ReadNow(Fx)), static_cast<int64>(80));
	TestEqual(TEXT("two reads"), Fx.Fake->CountTo(TEXT("player_data?page=")), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerDataReadAcrossARowTest, "Flock.Player.Wallet.AReadOnItsWayWhenAServerRowIsAppliedDoesNotPutTheOlderDataBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerDataReadAcrossARowTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	FirstReadOlderThanTheRest(Fx);

	TFlockResult<FFlockWallet> Before;
	Fx.ReadWallet(Before);
	Fx.Players->ApplyServerPlayerData(WalletWithGold(TEXT("80")));
	Fx.Fake->FlushPending();

	TestEqual(TEXT("control: the earlier read answered its caller with the earlier balance"), GoldOf(Before), static_cast<int64>(250));
	TestEqual(TEXT("the earlier answer was not kept: the next read asked the server"), GoldOf(ReadNow(Fx)), static_cast<int64>(80));
	TestEqual(TEXT("two reads"), Fx.Fake->CountTo(TEXT("player_data?page=")), 2);
	Fx.Players->ApplyServerPlayerData(WalletWithGold(TEXT("40")));
	TestEqual(TEXT("control: a row applied over the cached data is kept"), GoldOf(ReadNow(Fx)), static_cast<int64>(40));
	TestEqual(TEXT("read from the cache"), Fx.Fake->CountTo(TEXT("player_data?page=")), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerDataReadAfterARowNotJoinedTest, "Flock.Player.Wallet.AReadAfterAServerRowIsAppliedIsNotJoinedToAReadFromBefore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerDataReadAfterARowNotJoinedTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	FirstReadOlderThanTheRest(Fx);

	TFlockResult<FFlockWallet> Before;
	TFlockResult<FFlockWallet> After;
	Fx.ReadWallet(Before);
	Fx.Players->ApplyServerPlayerData(WalletWithGold(TEXT("80")));
	Fx.ReadWallet(After);
	Fx.Fake->FlushPending();

	TestEqual(TEXT("control: the read from before the row answered with the earlier balance"), GoldOf(Before), static_cast<int64>(250));
	TestEqual(TEXT("the read after the row asked the server"), GoldOf(After), static_cast<int64>(80));
	TestEqual(TEXT("two reads"), Fx.Fake->CountTo(TEXT("player_data?page=")), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerDataAnotherPlayersRowTest, "Flock.Player.Wallet.AReadEndedAfterAnotherPlayersRowWasAppliedIsNotJoinedByTheNextRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerDataAnotherPlayersRowTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn(TEXT("player-b"));
	FirstReadOlderThanTheRest(Fx);

	TFlockResult<FFlockWallet> Before;
	Fx.ReadWallet(Before);
	Fx.Players->ApplyServerPlayerData(WalletWithGold(TEXT("80")));
	Fx.Fake->FlushPending();

	TestEqual(TEXT("control: the read answered its caller"), GoldOf(Before), static_cast<int64>(250));
	TestEqual(TEXT("the next read asked the server, not the ended read"), GoldOf(ReadNow(Fx)), static_cast<int64>(80));
	TestEqual(TEXT("two reads"), Fx.Fake->CountTo(TEXT("player_data?page=")), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerDataOldReadEndingLateTest, "Flock.Player.Wallet.AReadFromBeforeARowEndingLateLeavesTheNewerReadForTheNextToJoin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerDataOldReadEndingLateTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	FirstReadOlderThanTheRest(Fx);

	TFlockResult<FFlockWallet> First;
	Fx.ReadWallet(First);
	Fx.Players->ApplyServerPlayerData(WalletWithGold(TEXT("80")));
	TFlockResult<FFlockWallet> Second;
	Fx.ReadWallet(Second);
	TestEqual(TEXT("two reads on their way"), Fx.Fake->CountTo(TEXT("player_data?page=")), 2);

	// The first read ends while the second is still on its way; a third read made then must join the second.
	TestTrue(TEXT("the first read's answer is delivered"), Fx.Fake->DeliverNext());
	TestEqual(TEXT("control: the first answered with the earlier balance"), GoldOf(First), static_cast<int64>(250));
	TFlockResult<FFlockWallet> Third;
	bool bThirdAnswered = false;
	Fx.Players->GetWallet([&](TFlockResult<FFlockWallet> R) { Third = R; bThirdAnswered = true; });
	TestEqual(TEXT("it joined the newer read: no third request"), Fx.Fake->CountTo(TEXT("player_data?page=")), 2);
	Fx.Fake->FlushPending();
	TestTrue(TEXT("the third read answered"), bThirdAnswered);
	TestEqual(TEXT("with the newer balance"), GoldOf(Third), static_cast<int64>(80));
	TestEqual(TEXT("the second with the newer"), GoldOf(Second), static_cast<int64>(80));
	TestEqual(TEXT("the newer answer is kept"), GoldOf(ReadNow(Fx)), static_cast<int64>(80));
	TestEqual(TEXT("still two reads"), Fx.Fake->CountTo(TEXT("player_data?page=")), 2);
	return true;
}

// ───────────────────────────── The token on every attempt (Unreal-36) ─────────────────────────────

// An expired token is renewed and the call sent again: the attempt after the renewal carries the new token, on a game-wide read,
// on a player's own read and on a money call alike.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRenewedTokenOnTheReplayTest, "Flock.Auth.SignIn.TheAttemptAfterARenewalCarriesTheNewToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRenewedTokenOnTheReplayTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* Name;
		FString Route;
		FString Answer;
		TFunction<void(FFixture&, TFunction<void(bool)>)> Call;
	};
	const FCase Cases[] = {
		{ TEXT("a shop item (game-wide)"), FlockEndpoints::ShopItemById(TEXT("item-1")), ItemBody(),
			[](FFixture& Fx, TFunction<void(bool)> Done) { Fx.Shop->GetItem(TEXT("item-1"), [Done](TFlockResult<FFlockShopItem> R) { Done(R.bSuccess); }); } },
		{ TEXT("the player's inventory"), InventoryRoute(), InventoryPage(1, 0, {}),
			[](FFixture& Fx, TFunction<void(bool)> Done) { Fx.Shop->GetPlayerInventory(1, 100, [Done](TFlockResult<FFlockPlayerInventoryPage> R) { Done(R.bSuccess); }); } },
		{ TEXT("a player data row"), FlockEndpoints::PlayerDataById(TEXT("pd-1")),
			TEXT("{\"error\":null,\"response\":null,\"result\":{\"id\":\"pd-1\",\"player_template_id\":\"t\",\"player_id\":\"player-a\",\"data\":[]}}"),
			[](FFixture& Fx, TFunction<void(bool)> Done) { Fx.Players->GetDataById(TEXT("pd-1"), [Done](TFlockResult<FFlockPlayerData> R) { Done(R.bSuccess); }); } },
		{ TEXT("a consume"), FlockEndpoints::PlayerInventoryConsume(TEXT("inv-p1")), ConsumedAnswer(TEXT("inv-p1")),
			[](FFixture& Fx, TFunction<void(bool)> Done) { Fx.Shop->Consume(TEXT("inv-p1"), [Done](TFlockResult<FFlockConsumeResult> R) { Done(R.bSuccess); }); } },
		{ TEXT("a wallet purchase"), FlockEndpoints::ShopTransaction, PurchaseAnswer(),
			[](FFixture& Fx, TFunction<void(bool)> Done)
			{
				Fx.WithItem();
				Fx.Shop->Purchase(TEXT("item-1"), [Done](TFlockResult<FFlockPurchaseResult> R) { Done(R.bSuccess); });
			} },
		{ TEXT("a store redemption"), FlockEndpoints::ShopValidateReceipt, PurchaseAnswer(),
			[](FFixture& Fx, TFunction<void(bool)> Done)
			{
				Fx.Shop->RedeemStoreReceipt(TEXT("item-1"), FFlockStoreReceipt::FromAppleAppStore(TEXT("2000000123456789")),
					[Done](TFlockResult<FFlockPurchaseResult> R) { Done(R.bSuccess); });
			} },
		{ TEXT("a ban (player-scoped)"), FlockEndpoints::PlayerBan, TEXT("{\"error\":null,\"response\":null,\"result\":null}"),
			[](FFixture& Fx, TFunction<void(bool)> Done) { Fx.Players->GetBan(FString(), [Done](TFlockResult<FFlockPlayerBan> R) { Done(R.bSuccess); }); } },
		{ TEXT("the shops page (snapshot-backed, game-wide)"), TEXT("shop?page="), TEXT("{\"items\":[],\"total\":0,\"page\":1,\"limit\":100}"),
			[](FFixture& Fx, TFunction<void(bool)> Done) { Fx.Shop->GetAll(1, 100, [Done](TFlockResult<FFlockShopPage> R) { Done(R.bSuccess); }); } },
		{ TEXT("a page of player data"), TEXT("player_data?page="), TEXT("{\"items\":[],\"total\":0,\"page\":1,\"limit\":100}"),
			[](FFixture& Fx, TFunction<void(bool)> Done) { Fx.Players->GetMyData(1, 100, [Done](TFlockResult<FFlockPlayerDataPage> R) { Done(R.bSuccess); }); } },
	};
	for (const FCase& Case : Cases)
	{
		FFixture Fx;
		Fx.SignIn();
		const FString Renewed = Jwt(TEXT("player-a"), 9);
		Fx.RenewsTo(Renewed);
		Fx.Fake->OnSequence(Case.Route, { Expired(), FFlockFakeTransport::Ok(Case.Answer) });

		bool bSucceeded = false;
		Case.Call(Fx, [&bSucceeded](bool bOk) { bSucceeded = bOk; });
		TestTrue(FString::Printf(TEXT("%s: answered once the token was renewed"), Case.Name), bSucceeded);
		const TArray<FFlockHttpRequest> Sent = RequestsTo(*Fx.Fake, Case.Route);
		if (TestEqual(FString::Printf(TEXT("%s: the expired attempt and the one after"), Case.Name), Sent.Num(), 2))
		{
			const FString* Authorization = Sent[1].Headers.Find(TEXT("Authorization"));
			TestTrue(FString::Printf(TEXT("%s: the attempt after the renewal carries the new token"), Case.Name),
				Authorization != nullptr && Authorization->EndsWith(Renewed, ESearchCase::CaseSensitive));
		}
	}
	return true;
}

// A player's own data read is sent only while its sign-in lasts: a retry after a switch never goes out as the next player.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerDataKeepsItsSignInTest, "Flock.Player.Data.ARetryAfterAPlayerSwitchIsNotSentAsTheNextPlayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerDataKeepsItsSignInTest::RunTest(const FString& Parameters)
{
	for (const bool bSwitch : { true, false })
	{
		FFixture Fx(3);
		Fx.SignIn(TEXT("player-a"));
		Fx.Fake->OnSequence(FlockEndpoints::PlayerDataById(TEXT("pd-1")), { ServerError(),
			FFlockFakeTransport::Ok(TEXT("{\"error\":null,\"response\":null,\"result\":{\"id\":\"pd-1\",\"player_template_id\":\"t\",\"player_id\":\"player-a\",\"data\":[]}}")) });
		TFlockResult<FFlockPlayerData> Result;
		Fx.Players->GetDataById(TEXT("pd-1"), [&Result](TFlockResult<FFlockPlayerData> R) { Result = R; });
		if (bSwitch)
		{
			Fx.SignIn(TEXT("player-b"));
		}
		FlockTestRetries::Pump();
		const FString Case = bSwitch ? TEXT("after a switch") : TEXT("control, no switch");
		TestEqual(FString::Printf(TEXT("%s: requests"), *Case), Fx.Fake->CountTo(FlockEndpoints::PlayerDataById(TEXT("pd-1"))), bSwitch ? 1 : 2);
		TestEqual(FString::Printf(TEXT("%s: outcome"), *Case), Result.bSuccess, !bSwitch);
	}
	return true;
}

// Every provider reads the token inside the attempt: one taken at dispatch goes out on the replay after a renewal, expired.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNoTokenTakenAtDispatchTest, "Flock.Auth.SignIn.NoProviderTakesTheTokenOnceBeforeSending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockNoTokenTakenAtDispatchTest::RunTest(const FString& Parameters)
{
	// The token is read only inside an attempt: through the session reference the attempt captured, or through the provider the
	// attempt pinned (Self->Session). Anything else taking it (a local, an init-capture, a helper) took it once before sending.
	auto TakesItOnce = [](const FString& Text)
	{
		if (Text.Contains(TEXT("HeadersNow"), ESearchCase::CaseSensitive))
		{
			return true;
		}
		int32 From = 0;
		while (true)
		{
			const int32 At = Text.Find(TEXT("GetAuthHeaders()"), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
			if (At == INDEX_NONE)
			{
				return false;
			}
			const FString Before = Text.Mid(FMath::Max(0, At - 16), FMath::Min(At, 16));
			if (!Before.EndsWith(TEXT("SessionRef->"), ESearchCase::CaseSensitive) && !Before.EndsWith(TEXT("Self->Session->"), ESearchCase::CaseSensitive))
			{
				return true;
			}
			From = At + 1;
		}
	};
	TestTrue(TEXT("control: a local taken at dispatch is seen"), TakesItOnce(TEXT("\tconst TMap<FString, FString> Headers = Session->GetAuthHeaders();")));
	TestTrue(TEXT("control: so is an auto local"), TakesItOnce(TEXT("auto Headers = Session->GetAuthHeaders();")));
	TestTrue(TEXT("control: and an init-capture"), TakesItOnce(TEXT("[Headers = Session->GetAuthHeaders()](auto Done) {}")));
	TestTrue(TEXT("control: and the old helper"), TakesItOnce(TEXT("Client->Get(Url, HeadersNow(), Done);")));
	TestFalse(TEXT("control: one read in the attempt is not"), TakesItOnce(TEXT("ClientRef->Get<T>(Url, SessionRef->GetAuthHeaders(), Done);")));
	TestFalse(TEXT("control: nor one through the pinned provider"), TakesItOnce(TEXT("Headers = Self->Session->GetAuthHeaders();")));

	const FString Private = FPaths::Combine(FPaths::GetPath(FString(ANSI_TO_TCHAR(__FILE__))), TEXT(".."));
	int32 Scanned = 0;
	for (const TCHAR* Folder : { TEXT("Providers"), TEXT("../Public/Providers"), TEXT("Multiplayer"), TEXT("../Public/Multiplayer") })
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(Private, Folder, TEXT("*.*")), true, false);
		for (const FString& File : Files)
		{
			FString Text;
			if (!TestTrue(FString::Printf(TEXT("%s can be read"), *File), FFileHelper::LoadFileToString(Text, *FPaths::Combine(Private, Folder, File))))
			{
				continue;
			}
			++Scanned;
			TestFalse(FString::Printf(TEXT("%s reads the token on each attempt, never once before sending"), *File), TakesItOnce(Text));
		}
	}
	TestTrue(FString::Printf(TEXT("the scan found the providers and the multiplayer requests (%d files)"), Scanned), Scanned >= 40);
	return true;
}

// The player provider torn down while a read of the player's data is out: its callers still hear back, as a cancel.
// The same player signing in again while a read is out: that read's later pages would be refused, so a read made under the
// new sign-in goes out on its own rather than joining it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerDataReadUnderAnEndedSignInTest, "Flock.Player.Wallet.AReadSentUnderAnEndedSignInIsNotJoined",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerDataReadUnderAnEndedSignInTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn(TEXT("player-a"));
	FirstReadOlderThanTheRest(Fx);

	TFlockResult<FFlockWallet> Before;
	Fx.ReadWallet(Before);
	Fx.SignIn(TEXT("player-a"), 1);
	TFlockResult<FFlockWallet> After;
	Fx.ReadWallet(After);
	TestEqual(TEXT("the read under the new sign-in went out on its own"), Fx.Fake->CountTo(TEXT("player_data?page=")), 2);
	Fx.Fake->FlushPending();
	TestEqual(TEXT("control: the earlier read answered its caller"), GoldOf(Before), static_cast<int64>(250));
	TestEqual(TEXT("the later one answered with its own read"), GoldOf(After), static_cast<int64>(80));

	// Control: under one sign-in, a second read joins the first.
	FFixture Same;
	Same.SignIn(TEXT("player-a"));
	FirstReadOlderThanTheRest(Same);
	TFlockResult<FFlockWallet> First;
	TFlockResult<FFlockWallet> Second;
	Same.ReadWallet(First);
	Same.ReadWallet(Second);
	TestEqual(TEXT("control: one read for two callers under one sign-in"), Same.Fake->CountTo(TEXT("player_data?page=")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerDataReadAfterTeardownTest, "Flock.Player.Wallet.AReadOutWhenThePlayerProviderIsTornDownStillAnswers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerDataReadAfterTeardownTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	FirstReadOlderThanTheRest(Fx);

	TFlockResult<FFlockWallet> Read;
	bool bAnswered = false;
	Fx.Players->GetWallet([&](TFlockResult<FFlockWallet> R) { Read = R; bAnswered = true; });
	TestEqual(TEXT("the read is out"), Fx.Fake->CountTo(TEXT("player_data?page=")), 1);
	Fx.Shop->SetPlayerProvider(nullptr);
	Fx.Players.Reset();
	Fx.Fake->FlushPending();

	TestTrue(TEXT("the caller heard back"), bAnswered);
	TestEqual(TEXT("as a cancel"), Read.Error.Type, EFlockErrorType::Cancelled);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
