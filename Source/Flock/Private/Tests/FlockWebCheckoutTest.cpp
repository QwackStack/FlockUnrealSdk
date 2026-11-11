// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockEvents.h"
#include "FlockInitConfig.h"
#include "FlockSubsystem.h"
#include "Http/FlockEndpoints.h"
#include "Misc/Base64.h"
#include "Misc/ConfigCacheIni.h"
#include "Providers/FlockAuthProvider.h"
#include "Providers/FlockMultiplayerProvider.h"
#include "Providers/FlockPlayerProvider.h"
#include "Providers/FlockShopProvider.h"
#include "Store/FlockStore.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockTestSdk.h"
#include "Tests/Support/FlockTestSpelling.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Tests/Support/FlockStoreNodeTestListener.h"
#include "Blueprint/FlockShopLibrary.h"
#include "Blueprint/FlockStoreLibrary.h"
#include "Containers/Ticker.h"

// Web checkout, driven through FFlockShopProvider::Purchase with UFlockStore connected on this desktop: a checkout opened once
// and never sent again after an answer that did not come, its payment page opened, the purchase read on the repeating calls'
// clock until it is paid, failed or expired, and checkouts left open found at each sign-in.

#define FLOCK_WEB_CHECKOUT_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Store.WebCheckout." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

namespace FlockWebCheckoutTesting
{
	const FString PaymentPage = TEXT("https://checkout.stripe.com/c/pay/cs_test_a1");
	const FString Checkout = FString::Printf(TEXT("{\"purchase_id\":\"pur-1\",\"provider\":\"stripe\",\"payment_url\":\"%s\",\"expires_at\":\"2026-10-31T13:00:00Z\"}"), *PaymentPage);
	const FString Sword = TEXT("{\"id\":\"sword\",\"name\":\"Sword\",\"type\":\"standard\",\"price\":799,\"currency\":\"SAR\",\"is_real_money\":true,\"store_skus\":{},\"status\":\"active\"}");
	const FString GoldItem = TEXT("{\"id\":\"gold-item\",\"name\":\"GoldItem\",\"type\":\"standard\",\"price\":20,\"currency\":\"Gold\",\"is_real_money\":false,\"store_skus\":{},\"status\":\"active\"}");
	const FString Granted10Gold = TEXT("[{\"type\":\"currency\",\"code\":\"Gold\",\"amount\":10}]");

	inline FString Purchase(const FString& Id, const FString& Status, const FString& Granted = TEXT("[]"))
	{
		const bool bPaid = Status == TEXT("paid");
		return FString::Printf(TEXT("{\"id\":\"%s\",\"shop_item_id\":\"sword\",\"item_type\":\"standard\",\"status\":\"%s\",\"price_amount\":799,\"price_currency\":\"SAR\",")
			TEXT("\"settlement_method\":\"external\",\"payment_provider\":\"stripe\",\"granted\":%s,\"player_inventory_id\":%s,")
			TEXT("\"created_at\":\"2026-10-31T12:00:00Z\",\"settled_at\":%s,\"refunded_at\":null,\"expires_at\":\"2026-10-31T13:00:00Z\"}"),
			*Id, *Status, *Granted, bPaid ? TEXT("\"inv-1\"") : TEXT("null"), bPaid ? TEXT("\"2026-10-31T12:05:00Z\"") : TEXT("null"));
	}

	inline FString CatalogOf(const TArray<FString>& Items)
	{
		return FString::Printf(TEXT("{\"items\":[{\"id\":\"shop-1\",\"name\":\"main\",\"shop_items\":[%s]}],\"total\":1,\"page\":1,\"limit\":100}"),
			*FString::Join(Items, TEXT(",")));
	}

	inline FString OpenList(const TArray<FString>& Purchases)
	{
		return FString::Printf(TEXT("{\"items\":[%s],\"total\":%d,\"page\":1,\"limit\":100}"), *FString::Join(Purchases, TEXT(",")), Purchases.Num());
	}

	inline FString WalletPage(int32 Gems)
	{
		return FString::Printf(TEXT("{\"items\":[{\"id\":\"pd-wallet\",\"player_template_id\":\"tpl-wallet\",\"player_id\":\"player-a\",\"data\":[{\"type\":\"object\",\"value\":[")
			TEXT("{\"type\":\"integer\",\"value\":%d,\"type_name\":\"currency\",\"field_name\":\"Gems\"}],\"type_name\":\"wallet\",\"field_name\":\"game_currencies\"}]}],\"total\":1,\"page\":1,\"limit\":100}"), Gems);
	}

	inline FString Base64Url(const FString& In)
	{
		FString Encoded = FBase64::Encode(In);
		Encoded.ReplaceInline(TEXT("+"), TEXT("-"));
		Encoded.ReplaceInline(TEXT("/"), TEXT("_"));
		Encoded.ReplaceInline(TEXT("="), TEXT(""));
		return Encoded;
	}

	inline FString Jwt(const FString& PlayerId)
	{
		return FString::Printf(TEXT("h.%s.s"), *Base64Url(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, FDateTime::UtcNow().ToUnixTimestamp() + 3600)));
	}

	/** Where a test keeps what the SDK answers, in memory that outlives the test's frame (Flock answers waiting calls as it shuts down). */
	template <typename T>
	struct TAnswer
	{
		TSharedRef<TOptional<T>> Held = MakeShared<TOptional<T>>();

		bool IsSet() const { return Held->IsSet(); }
		const T& GetValue() const { return Held->GetValue(); }
		const T* operator->() const { return &Held->GetValue(); }

		TFunction<void(T)> Into() const
		{
			const TSharedRef<TOptional<T>> Answer = Held;
			return [Answer](T Value) { *Answer = MoveTemp(Value); };
		}
	};

	/** A whole SDK over a fake server that sells the Sword for real money, with the repeating calls on a clock the test moves. */
	struct FFixture
	{
		FFlockTestSdk Test;
		UFlockSubsystem* Sdk = Test.Sdk;
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockMemoryTokenStore> Tokens = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<double> Clock = MakeShared<double>(1000.0);
		TSharedRef<TArray<FString>> Opened = MakeShared<TArray<FString>>();
		TSharedRef<TArray<FString>> Completed = MakeShared<TArray<FString>>();
		UFlockStore* Connected = nullptr;

		explicit FFixture(const TArray<FString>& Items = { Sword, GoldItem })
		{
			Sdk->SetHttpAdapterForTesting(Fake);
			Sdk->SetTokenStoreForTesting(Tokens);
			Fake->On(TEXT("shop?page="), FFlockFakeTransport::Ok(CatalogOf(Items)));
			Fake->On(TEXT("shop/purchases?"), FFlockFakeTransport::Ok(OpenList({})));
			for (const FString& Item : Items)
			{
				ServeItem(Item);
			}
			FFlockInitConfig Config;
			Config.ApiUrl = TEXT("https://api-flock.qwacks.com");
			Config.ApiKey = TEXT("secret");
			Config.GameId = TEXT("my-game");
			Config.GameVersion = TEXT("1.2.3");
			Config.GameVersionId = TEXT("ver-abc");
			Sdk->InitializeWithConfig(Config);
			const TSharedRef<double> Now = Clock;
			// A roll of one half makes each wait exactly the interval.
			Calls().SetClockForTesting([Now]() { return *Now; }, []() { return 0.5; });
		}

		~FFixture()
		{
			Sdk->ShutdownSdk();
		}

		/** The item's own read (shop_item/<id>), which a web purchase makes when it buys. */
		void ServeItem(const FString& Item)
		{
			FString Rest;
			FString Id;
			if (Item.Split(TEXT("\"id\":\""), nullptr, &Rest) && Rest.Split(TEXT("\""), &Id, nullptr))
			{
				Fake->On(TEXT("shop_item/") + Id, FFlockFakeTransport::Ok(Item));
			}
		}

		FFlockRepeatingCalls& Calls() const { return Sdk->GetMultiplayerProvider()->GetRepeatingCalls(); }

		void SignIn(const FString& PlayerId)
		{
			Fake->On(TEXT("player/login/device"), FFlockFakeTransport::Ok(FString::Printf(
				TEXT("{\"player_id\":\"%s\",\"access_token\":\"%s\",\"refresh_token\":\"r-%s\"}"), *PlayerId, *Jwt(PlayerId), *PlayerId)));
			Sdk->GetAuthProvider()->LoginWithDevice(TEXT("device-") + PlayerId, nullptr);
		}

		/** Connects a web checkout; empty names no gateway (this desktop's own way: the game's one active gateway). */
		TFlockResult<UFlockStore*> Connect(const FString& Gateway = FString(), float CheckEverySeconds = 0.f)
		{
			FFlockStoreOptions Options;
			Options.PaymentProvider = Gateway;
			Options.CheckEverySeconds = CheckEverySeconds;
			const TSharedRef<TArray<FString>> Pages = Opened;
			Options.OpenPaymentPage = [Pages](const FString& Url) { Pages->Add(Url); };
			TOptional<TFlockResult<UFlockStore*>> Out;
			UFlockStore::Connect(Sdk, Options, [&Out](TFlockResult<UFlockStore*> Result) { Out = Result; });
			if (!Out.IsSet())
			{
				return TFlockResult<UFlockStore*>::Fail(FFlockError::Make(EFlockErrorType::Validation, TEXT("Connect never answered")));
			}
			Connected = Out->IsSuccess() ? Out->Value : nullptr;
			if (Connected != nullptr)
			{
				const TSharedRef<TArray<FString>> Reported = Completed;
				Connected->OnPurchaseCompleted.AddLambda([Reported](const FFlockShopItem& Item, const FFlockPurchaseResult& Result)
				{
					Reported->Add(Item.Id + TEXT(":") + Result.PurchaseId);
				});
			}
			return Out.GetValue();
		}

		TAnswer<TFlockResult<FFlockPurchaseResult>> Buy(const FString& ShopItemId = TEXT("sword"))
		{
			TAnswer<TFlockResult<FFlockPurchaseResult>> Answer;
			Sdk->GetShopProvider()->Purchase(ShopItemId, Answer.Into());
			return Answer;
		}

		/** Moves the clock on and sends every read that is due, as a frame does. */
		void Tick(double Seconds = 3.0)
		{
			*Clock += Seconds;
			Calls().SendThoseDue();
		}

		int32 Reads(const FString& PurchaseId = TEXT("pur-1")) const { return Fake->CountTo(TEXT("shop/purchases/") + PurchaseId); }

		const FFlockHttpRequest* LastTo(const FString& Fragment) const
		{
			for (int32 Index = Fake->Requests.Num() - 1; Index >= 0; --Index)
			{
				if (Fake->Requests[Index].Url.Contains(Fragment))
				{
					return &Fake->Requests[Index];
				}
			}
			return nullptr;
		}
	};
}

using namespace FlockWebCheckoutTesting;

// ── The shop provider's calls ──

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutBodyTest, "StartCheckoutSendsTheItemAndWhatIsSetAndLeavesTheRestOut")
bool FFlockWebCheckoutBodyTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	TAnswer<TFlockResult<FFlockCheckout>> Opened;
	F.Sdk->GetShopProvider()->StartCheckout(TEXT("sword"), FlockPaymentProviders::Stripe, TEXT("https://game.example/done"), FString(), Opened.Into());

	const FFlockHttpRequest* Sent = F.LastTo(FlockEndpoints::ShopCheckout);
	if (!TestNotNull(TEXT("A checkout was sent"), Sent) || !TestTrue(TEXT("Answered"), Opened.IsSet()))
	{
		return false;
	}
	TestEqualSensitive(TEXT("The item and what is set, nothing else"), Sent->JsonBody,
		FString(TEXT("{\"shop_item_id\":\"sword\",\"provider\":\"stripe\",\"return_url\":\"https://game.example/done\"}")));
	TestTrue(TEXT("Sent as the signed-in player"), Sent->Headers.Contains(TEXT("Authorization")));
	TestTrue(TEXT("Opened"), Opened->IsSuccess());
	TestEqualSensitive(TEXT("Its purchase"), Opened->Value.PurchaseId, FString(TEXT("pur-1")));
	TestEqualSensitive(TEXT("Its page"), Opened->Value.PaymentUrl, PaymentPage);
	TestEqualSensitive(TEXT("When it closes"), Opened->Value.ExpiresAt, FString(TEXT("2026-10-31T13:00:00Z")));
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutNeverSentAgainTest, "StartCheckoutWhoseAnswerNeverCameIsNotSentAgain")
bool FFlockWebCheckoutNeverSentAgainTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	const TArray<FFlockHttpResponse> NoAnswer = { FFlockFakeTransport::Status(500, TEXT("<html>Internal Server Error</html>")), FFlockFakeTransport::Timeout() };
	for (const FFlockHttpResponse& Answer : NoAnswer)
	{
		FFixture F;
		F.SignIn(TEXT("player-a"));
		F.Fake->On(FlockEndpoints::ShopCheckout, Answer);
		TAnswer<TFlockResult<FFlockCheckout>> Opened;
		F.Sdk->GetShopProvider()->StartCheckout(TEXT("sword"), FString(), FString(), FString(), Opened.Into());
		for (int32 Frame = 0; Frame < 20 && !Opened.IsSet(); ++Frame)
		{
			FTSTicker::GetCoreTicker().Tick(1.f);
		}
		if (!TestTrue(TEXT("Answered"), Opened.IsSet()))
		{
			continue;
		}
		TestEqual(TEXT("A checkout is never sent again after an answer that did not come"), F.Fake->CountTo(FlockEndpoints::ShopCheckout), 1);
		TestFalse(TEXT("Failed"), Opened->IsSuccess());
		TestTrue(TEXT("It may have been opened"), Opened->Error.bOutcomeUnknown);
		TestTrue(TEXT("The fix says to read the open checkouts first"), Opened->Error.Hint.Contains(TEXT("GetPurchases")));
	}
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutNoPageTest, "StartCheckoutAnAnswerWithNoPaymentPageFailsTheCall")
bool FFlockWebCheckoutNoPageTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(TEXT("{\"purchase_id\":\"pur-1\",\"provider\":\"stripe\"}")));
	TAnswer<TFlockResult<FFlockCheckout>> Opened;
	F.Sdk->GetShopProvider()->StartCheckout(TEXT("sword"), FString(), FString(), FString(), Opened.Into());
	if (!TestTrue(TEXT("Answered"), Opened.IsSet()))
	{
		return false;
	}
	TestFalse(TEXT("Failed"), Opened->IsSuccess());
	TestEqual(TEXT("Unreadable"), Opened->Error.Type, EFlockErrorType::Serialization);
	TestTrue(TEXT("A checkout may have been opened"), Opened->Error.bOutcomeUnknown);
	TestFalse(TEXT("With a fix"), Opened->Error.Hint.IsEmpty());
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutSignedOutTest, "StartCheckoutGetPurchaseAndGetPurchasesNeedASignedInPlayer")
bool FFlockWebCheckoutSignedOutTest::RunTest(const FString& Parameters)
{
	FFixture F;
	TAnswer<TFlockResult<FFlockCheckout>> Opened;
	TAnswer<TFlockResult<FFlockPlayerPurchase>> Read;
	TAnswer<TFlockResult<FFlockPlayerPurchasePage>> Listed;
	F.Sdk->GetShopProvider()->StartCheckout(TEXT("sword"), FString(), FString(), FString(), Opened.Into());
	F.Sdk->GetShopProvider()->GetPurchase(TEXT("pur-1"), Read.Into());
	F.Sdk->GetShopProvider()->GetPurchases(FlockPurchaseStatuses::Pending, 1, 25, Listed.Into());
	TestTrue(TEXT("Checkout refused"), Opened.IsSet() && Opened->Error.Type == EFlockErrorType::Auth && !Opened->Error.Hint.IsEmpty());
	TestTrue(TEXT("Read refused"), Read.IsSet() && Read->Error.Type == EFlockErrorType::Auth);
	TestTrue(TEXT("List refused"), Listed.IsSet() && Listed->Error.Type == EFlockErrorType::Auth);
	TestEqual(TEXT("Nothing sent"), F.Fake->CountTo(TEXT("shop/checkout")) + F.Fake->CountTo(TEXT("shop/purchases")), 0);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutReadTest, "GetPurchaseReadsThePurchaseAndGetPurchasesSendsTheStatus")
bool FFlockWebCheckoutReadTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("paid"), Granted10Gold)));
	F.Fake->On(TEXT("shop/purchases?"), FFlockFakeTransport::Ok(OpenList({ Purchase(TEXT("pur-2"), TEXT("pending")) })));
	TAnswer<TFlockResult<FFlockPlayerPurchase>> Read;
	TAnswer<TFlockResult<FFlockPlayerPurchasePage>> Listed;
	F.Sdk->GetShopProvider()->GetPurchase(TEXT("pur-1"), Read.Into());
	F.Sdk->GetShopProvider()->GetPurchases(FlockPurchaseStatuses::Pending, 2, 50, Listed.Into());
	if (!TestTrue(TEXT("Both answered"), Read.IsSet() && Listed.IsSet()) || !TestTrue(TEXT("Read"), Read->IsSuccess()) || !TestTrue(TEXT("Listed"), Listed->IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("Paid"), Read->Value.IsPaid());
	TestEqual(TEXT("Price"), Read->Value.PriceAmount, static_cast<int64>(799));
	TestEqualSensitive(TEXT("Its entry"), Read->Value.PlayerInventoryId, FString(TEXT("inv-1")));
	TestTrue(TEXT("What it granted"), Read->Value.Granted.Num() == 1 && Read->Value.Granted[0].Amount == 10);
	const FFlockHttpRequest* Sent = F.LastTo(TEXT("shop/purchases?"));
	TestTrue(TEXT("The page, size and status"), Sent != nullptr && Sent->Url.EndsWith(TEXT("shop/purchases?page=2&limit=50&status=pending"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("Pending"), Listed->Value.Items.Num() == 1 && Listed->Value.Items[0].IsPending());
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutUnknownStatusTest, "GetPurchasesByAStatusTheServerDoesNotHaveReadsAsItsOwnCodeWithItsFix")
bool FFlockWebCheckoutUnknownStatusTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(TEXT("shop/purchases?"), FFlockFakeTransport::Coded(422, TEXT("shop.invalid_purchase_status")));
	TAnswer<TFlockResult<FFlockPlayerPurchasePage>> Listed;
	F.Sdk->GetShopProvider()->GetPurchases(TEXT("unpaid"), 1, 25, Listed.Into());
	if (!TestTrue(TEXT("Answered"), Listed.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("Its own code"), Listed->Error.ErrorCode, EFlockErrorCode::ShopInvalidPurchaseStatus);
	TestTrue(TEXT("The fix names the statuses"), Listed->Error.Hint.Contains(TEXT("pending")));
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutNotForSaleTest, "StartCheckoutRefusedAsNotForSaleDropsTheCachedShops")
bool FFlockWebCheckoutNotForSaleTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	struct FCase { const TCHAR* Code; int32 Status; int32 ShopReads; };
	// Switched off in the dashboard: the shops kept stop showing it; another refusal keeps them.
	for (const FCase& Case : { FCase{ TEXT("shop.item_not_for_sale"), 409, 2 }, FCase{ TEXT("shop.not_a_real_currency_item"), 422, 1 } })
	{
		FFixture F({ Sword });
		F.SignIn(TEXT("player-a"));
		F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Coded(Case.Status, Case.Code));
		F.Sdk->GetShopProvider()->GetAll(1, 100, nullptr);
		TAnswer<TFlockResult<FFlockCheckout>> Opened;
		F.Sdk->GetShopProvider()->StartCheckout(TEXT("sword"), FString(), FString(), FString(), Opened.Into());
		F.Sdk->GetShopProvider()->GetAll(1, 100, nullptr);
		TestTrue(FString::Printf(TEXT("%s: refused with its code"), Case.Code), Opened.IsSet() && Opened->Error.Code == Case.Code);
		TestFalse(FString::Printf(TEXT("%s: a refusal is certain"), Case.Code), Opened.IsSet() && Opened->Error.bOutcomeUnknown);
		TestEqual(FString::Printf(TEXT("%s: shop reads"), Case.Code), F.Fake->CountTo(TEXT("shop?page=")), Case.ShopReads);
	}
	return true;
}

// ── The purchase call through web checkout ──

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutBuyTest, "BuyOpensThePaymentPageReadsUntilPaidAndReportsItOnce")
bool FFlockWebCheckoutBuyTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->OnSequence(TEXT("shop/purchases/pur-1"), { FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("pending"))),
		FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("pending"))), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("paid"), Granted10Gold)) });
	if (!TestTrue(TEXT("Connected"), F.Connect().IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("A web checkout"), F.Connected->IsWebCheckout());
	TestTrue(TEXT("No gateway named"), F.Connected->GetPaymentProvider().IsEmpty());

	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	TestEqual(TEXT("The payment page opened once"), F.Opened->Num(), 1);
	TestTrue(TEXT("The gateway's page"), F.Opened->Num() == 1 && (*F.Opened)[0] == PaymentPage);
	TestFalse(TEXT("Waiting for the payment"), Bought.IsSet());
	TestEqual(TEXT("Nothing read before the interval"), F.Reads(), 0);
	F.Tick(2.0);
	TestEqual(TEXT("Still nothing read before the interval"), F.Reads(), 0);
	F.Tick(1.0);
	F.Tick();
	TestFalse(TEXT("Still pending"), Bought.IsSet());
	F.Tick();
	if (!TestTrue(TEXT("Answered once paid"), Bought.IsSet()) || !TestTrue(TEXT("Bought"), Bought->IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("Paid"), Bought->Value.IsPaid());
	TestEqualSensitive(TEXT("Its purchase"), Bought->Value.PurchaseId, FString(TEXT("pur-1")));
	TestTrue(TEXT("What it granted"), Bought->Value.Granted.Num() == 1 && Bought->Value.Granted[0].Amount == 10);
	TestEqual(TEXT("Three reads"), F.Reads(), 3);
	TestEqual(TEXT("Reported once"), F.Completed->Num(), 1);
	TestTrue(TEXT("With its item"), F.Completed->Num() == 1 && (*F.Completed)[0] == TEXT("sword:pur-1"));
	F.Tick();
	F.Tick();
	TestEqual(TEXT("Nothing read after it ended"), F.Reads(), 3);
	TestEqual(TEXT("Still reported once"), F.Completed->Num(), 1);
	TestEqual(TEXT("One checkout"), F.Fake->CountTo(FlockEndpoints::ShopCheckout), 1);
	TestEqual(TEXT("Nothing taken from the wallet"), F.Fake->CountTo(FlockEndpoints::ShopTransaction), 0);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutEventFirstTest, "APaidCheckoutIsReportedBeforeTheCallIsAnsweredAndTheItemCanBeBoughtAgainThere")
bool FFlockWebCheckoutEventFirstTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("paid"))));
	if (!TestTrue(TEXT("Connected"), F.Connect().IsSuccess()))
	{
		return false;
	}
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	const TSharedRef<TOptional<TFlockResult<FFlockPurchaseResult>>> Held = Bought.Held;
	const TSharedRef<bool> AnsweredFirst = MakeShared<bool>(false);
	const TSharedRef<TOptional<TFlockResult<FFlockPurchaseResult>>> Again = MakeShared<TOptional<TFlockResult<FFlockPurchaseResult>>>();
	UFlockSubsystem* Sdk = F.Sdk;
	F.Connected->OnPurchaseCompleted.AddLambda([Held, AnsweredFirst, Again, Sdk](const FFlockShopItem&, const FFlockPurchaseResult&)
	{
		*AnsweredFirst = Held->IsSet();
		// The call is off the books when the event runs, so buying the item again is not refused as already being bought.
		Sdk->GetShopProvider()->Purchase(TEXT("sword"), [Again](TFlockResult<FFlockPurchaseResult> Result) { *Again = Result; });
	});
	F.Tick();
	TestTrue(TEXT("Answered"), Bought.IsSet());
	TestFalse(TEXT("The event came before the answer"), *AnsweredFirst);
	TestFalse(TEXT("A second purchase from the event is not refused as already being bought"),
		Again->IsSet() && Again->GetValue().Error.Message.Contains(TEXT("already being bought")));
	TestEqual(TEXT("It opened a second checkout"), F.Fake->CountTo(FlockEndpoints::ShopCheckout), 2);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutWalletTest, "BuyPaidDropsTheCachedWalletSoTheNextReadAsksTheServer")
bool FFlockWebCheckoutWalletTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("paid"))));
	F.Fake->On(FlockEndpoints::PlayerTemplate, FFlockFakeTransport::Ok(
		TEXT("{\"error\":{\"code\":null},\"response\":{\"message\":null,\"code\":null},\"result\":[{\"id\":\"tpl-wallet\",\"name\":\"currencies\",\"schema\":[],\"data\":[],\"tag\":\"currency\"}]}")));
	F.Fake->OnSequence(TEXT("player_data?page="), { FFlockFakeTransport::Ok(WalletPage(0)), FFlockFakeTransport::Ok(WalletPage(100)) });
	TAnswer<TFlockResult<FFlockWallet>> Before;
	F.Sdk->GetPlayerProvider()->GetWallet(Before.Into());
	if (!TestTrue(TEXT("Read before"), Before.IsSet() && Before->IsSuccess()) || !TestTrue(TEXT("Connected"), F.Connect().IsSuccess()))
	{
		return false;
	}
	TestEqual(TEXT("0 Gems before"), Before->Value.BalanceOf(TEXT("Gems")), static_cast<int64>(0));
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	F.Tick();
	TestTrue(TEXT("Bought"), Bought.IsSet() && Bought->IsSuccess());
	TAnswer<TFlockResult<FFlockWallet>> After;
	F.Sdk->GetPlayerProvider()->GetWallet(After.Into());
	TestTrue(TEXT("The server's balance, not the cached 0"), After.IsSet() && After->IsSuccess() && After->Value.BalanceOf(TEXT("Gems")) == 100);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutUnpaidTest, "BuyACheckoutThatEndsUnpaidFailsWithItsReasonAndReportsNothing")
bool FFlockWebCheckoutUnpaidTest::RunTest(const FString& Parameters)
{
	struct FCase { const TCHAR* Status; const TCHAR* Reason; };
	for (const FCase& Case : { FCase{ FlockPurchaseStatuses::Failed, FlockStoreFailureReasons::PaymentDeclined },
		FCase{ FlockPurchaseStatuses::Expired, FlockStoreFailureReasons::Expired }, FCase{ FlockPurchaseStatuses::Refunded, FlockStoreFailureReasons::Unknown } })
	{
		FFixture F;
		F.SignIn(TEXT("player-a"));
		F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
		F.Fake->OnSequence(TEXT("shop/purchases/pur-1"), { FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("pending"))),
			FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), Case.Status)) });
		F.Connect();
		TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
		F.Tick();
		F.Tick();
		if (!TestTrue(FString::Printf(TEXT("%s: answered"), Case.Status), Bought.IsSet()))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s: a store failure"), Case.Status), Bought->Error.Type, EFlockErrorType::Store);
		TestEqualSensitive(FString::Printf(TEXT("%s: its reason"), Case.Status), Bought->Error.StoreFailureReason, FString(Case.Reason));
		TestFalse(FString::Printf(TEXT("%s: with a fix"), Case.Status), Bought->Error.Hint.IsEmpty());
		TestTrue(FString::Printf(TEXT("%s: the fix says what was charged"), Case.Status), Bought->Error.Hint.Contains(TEXT("Nothing was")));
		TestEqual(FString::Printf(TEXT("%s: nothing reported"), Case.Status), F.Completed->Num(), 0);
		F.Tick();
		TestEqual(FString::Printf(TEXT("%s: nothing read after it ended"), Case.Status), F.Reads(), 2);
	}
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutSignOutTest, "SigningOutWhileWaitingEndsTheWaitAndNothingMoreIsRead")
bool FFlockWebCheckoutSignOutTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("pending"))));
	F.Connect();
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	F.Tick();
	TestEqual(TEXT("Read once"), F.Reads(), 1);
	F.Sdk->GetAuthProvider()->Logout();
	TestTrue(TEXT("The wait ended"), Bought.IsSet() && Bought->Error.Type == EFlockErrorType::Cancelled);
	TestTrue(TEXT("With a fix"), Bought.IsSet() && !Bought->Error.Hint.IsEmpty());
	F.Tick();
	F.Tick();
	TestEqual(TEXT("Nothing more read"), F.Reads(), 1);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutSwitchTest, "APlayerSwitchWhileWaitingEndsTheWaitAndReadsTheNewPlayersOpenCheckouts")
bool FFlockWebCheckoutSwitchTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("pending"))));
	F.Connect();
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	const int32 ListsBefore = F.Fake->CountTo(TEXT("shop/purchases?"));
	F.SignIn(TEXT("player-b"));
	TestTrue(TEXT("The wait ended"), Bought.IsSet() && Bought->Error.Type == EFlockErrorType::Cancelled);
	TestEqual(TEXT("The new player's open checkouts are listed"), F.Fake->CountTo(TEXT("shop/purchases?")), ListsBefore + 1);
	F.Tick();
	TestEqual(TEXT("The first player's checkout is not read as the second"), F.Reads(), 0);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutAnswerAfterSignOutTest, "TheCheckoutsAnswerAfterSignOutOpensNoPage")
bool FFlockWebCheckoutAnswerAfterSignOutTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Connect();
	F.Sdk->GetShopProvider()->GetItem(TEXT("sword"), nullptr);
	F.Fake->bDeferred = true;
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	F.Fake->bDeferred = false;
	TestEqual(TEXT("The checkout is on its way"), F.Fake->CountTo(FlockEndpoints::ShopCheckout), 1);
	F.Sdk->GetAuthProvider()->Logout();
	F.Fake->FlushPending();
	TestTrue(TEXT("Answered"), Bought.IsSet() && Bought->Error.Type == EFlockErrorType::Cancelled);
	TestEqual(TEXT("No page opened"), F.Opened->Num(), 0);
	F.Tick();
	TestEqual(TEXT("Nothing read"), F.Reads(), 0);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutFoundTest, "ACheckoutFoundAtSignInIsReadUntilPaidAndReportedWithItsItem")
bool FFlockWebCheckoutFoundTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Fake->On(TEXT("shop/purchases?"), FFlockFakeTransport::Ok(OpenList({ Purchase(TEXT("pur-2"), TEXT("pending")) })));
	F.Fake->OnSequence(TEXT("shop/purchases/pur-2"), { FFlockFakeTransport::Ok(Purchase(TEXT("pur-2"), TEXT("pending"))),
		FFlockFakeTransport::Ok(Purchase(TEXT("pur-2"), TEXT("paid"))) });
	F.Connect();
	TestEqual(TEXT("Nothing listed before a sign-in"), F.Fake->CountTo(TEXT("shop/purchases?")), 0);
	F.SignIn(TEXT("player-a"));
	const FFlockHttpRequest* Listed = F.LastTo(TEXT("shop/purchases?"));
	TestTrue(TEXT("The open ones listed at the sign-in"), Listed != nullptr && Listed->Url.Contains(TEXT("status=pending"), ESearchCase::CaseSensitive));
	F.Tick();
	TestEqual(TEXT("Still pending, nothing reported"), F.Completed->Num(), 0);
	F.Tick();
	TestEqual(TEXT("Reported once paid"), F.Completed->Num(), 1);
	TestTrue(TEXT("With its item"), F.Completed->Num() == 1 && (*F.Completed)[0] == TEXT("sword:pur-2"));
	F.Tick();
	TestEqual(TEXT("Nothing read after it ended"), F.Reads(TEXT("pur-2")), 2);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutFoundTwiceTest, "TheCallsOwnCheckoutFoundAgainByRestoreIsReadOnceAndReportedOnce")
bool FFlockWebCheckoutFoundTwiceTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->OnSequence(TEXT("shop/purchases/pur-1"), { FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("pending"))),
		FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("paid"))) });
	F.Connect();
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	F.Fake->On(TEXT("shop/purchases?"), FFlockFakeTransport::Ok(OpenList({ Purchase(TEXT("pur-1"), TEXT("pending")) })));
	TAnswer<TFlockResult<FFlockStoreRestore>> Restored;
	F.Connected->Restore(Restored.Into());
	TestTrue(TEXT("Restored, both lists empty"), Restored.IsSet() && Restored->IsSuccess() && Restored->Value.Restored.Num() == 0 && Restored->Value.NotDelivered.Num() == 0);
	F.Tick();
	TestEqual(TEXT("One read per interval"), F.Reads(), 1);
	F.Tick();
	TestTrue(TEXT("Bought"), Bought.IsSet() && Bought->IsSuccess());
	TestEqual(TEXT("Reported once"), F.Completed->Num(), 1);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutStaleListTest, "AStaleListOfOpenCheckoutsAfterOneWasPaidDoesNotReportItAgain")
bool FFlockWebCheckoutStaleListTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("paid"))));
	F.Fake->On(TEXT("shop/purchases?"), FFlockFakeTransport::Ok(OpenList({ Purchase(TEXT("pur-1"), TEXT("pending")) })));
	F.Connect();
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	// The list, answered while the checkout was pending, is handed over after it was paid.
	F.Fake->bDeferred = true;
	TAnswer<TFlockResult<FFlockStoreRestore>> Restored;
	F.Connected->Restore(Restored.Into());
	F.Fake->bDeferred = false;
	F.Tick();
	TestTrue(TEXT("Paid"), Bought.IsSet() && Bought->IsSuccess());
	F.Fake->FlushPending();
	F.Tick();
	F.Tick();
	TestEqual(TEXT("Not read again"), F.Reads(), 1);
	TestEqual(TEXT("Reported once"), F.Completed->Num(), 1);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutShutdownTest, "ShuttingFlockDownEndsTheWait")
bool FFlockWebCheckoutShutdownTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("pending"))));
	F.Connect();
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	F.Sdk->ShutdownSdk();
	TestTrue(TEXT("The wait ended"), Bought.IsSet() && Bought->Error.Type == EFlockErrorType::Cancelled);
	TestTrue(TEXT("Disconnected"), F.Connected != nullptr && !F.Connected->IsConnected());
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutDisconnectTest, "DisconnectingEndsTheWaitAndStopsReadingAndAnotherCanConnect")
bool FFlockWebCheckoutDisconnectTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("pending"))));
	F.Connect();
	UFlockStore* First = F.Connected;
	// With nothing connected the purchase reads the item itself.
	F.Fake->On(TEXT("shop_item/sword"), FFlockFakeTransport::Ok(Sword));
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	TestFalse(TEXT("A second connect is refused"), F.Connect().IsSuccess());
	First->Disconnect();
	TestTrue(TEXT("The wait ended"), Bought.IsSet() && Bought->Error.Type == EFlockErrorType::Cancelled);
	F.Tick();
	TestEqual(TEXT("Nothing read after the disconnect"), F.Reads(), 0);
	TAnswer<TFlockResult<FFlockPurchaseResult>> Refused;
	First->Purchase(TEXT("sword"), Refused.Into());
	TestTrue(TEXT("The disconnected connection refuses, naming the connect"), Refused.IsSet() && Refused->Error.Hint.Contains(TEXT("UFlockStore::Connect")));
	TAnswer<TFlockResult<FFlockPurchaseResult>> NoneConnected = F.Buy();
	TestTrue(TEXT("Real money refused with nothing connected"), NoneConnected.IsSet() && !NoneConnected->IsSuccess()
		&& NoneConnected->Error.Hint.Contains(TEXT("UFlockStore::Connect")));
	TestEqual(TEXT("One checkout"), F.Fake->CountTo(FlockEndpoints::ShopCheckout), 1);
	TestTrue(TEXT("Another can connect"), F.Connect().IsSuccess());
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutRefusedForGoodTest, "AReadTheServerRefusesForGoodEndsTheWaitWithItsCode")
bool FFlockWebCheckoutRefusedForGoodTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("pending"))));
	F.Connect();
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	// Past the time a new checkout takes to be saved, so not found is the server's last word.
	F.Tick(31.0);
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Coded(404, TEXT("shop.purchase_not_found")));
	F.Tick();
	if (!TestTrue(TEXT("The wait ended"), Bought.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("With the server's code"), Bought->Error.ErrorCode, EFlockErrorCode::ShopPurchaseNotFound);
	TestTrue(TEXT("Saying Flock does not hold the checkout it opened"), Bought->Error.Message.Contains(TEXT("does not hold")));
	TestTrue(TEXT("Unknown: the player may already have paid on that page"), Bought->Error.bOutcomeUnknown);
	const int32 Reads = F.Reads();
	F.Tick();
	TestEqual(TEXT("Nothing read after"), F.Reads(), Reads);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutNotSavedYetTest, "ACheckoutJustOpenedReadAsNotFoundIsReadAgainAndEndsPaid")
bool FFlockWebCheckoutNotSavedYetTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->OnSequence(TEXT("shop/purchases/pur-1"), { FFlockFakeTransport::Coded(404, TEXT("shop.purchase_not_found")),
		FFlockFakeTransport::Coded(404, TEXT("shop.purchase_not_found")), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("pending"))),
		FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("paid"))) });
	F.Connect();
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	F.Tick();
	F.Tick();
	F.Tick();
	TestFalse(TEXT("Not ended by not found within the window, nor by pending"), Bought.IsSet());
	F.Tick();
	TestTrue(TEXT("Paid"), Bought.IsSet() && Bought->IsSuccess());
	TestEqual(TEXT("Four reads"), F.Reads(), 4);
	TestEqual(TEXT("Reported once"), F.Completed->Num(), 1);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutFoundNotFoundTest, "ACheckoutFoundAtSignInReadAsNotFoundIsNotReadAgain")
bool FFlockWebCheckoutFoundNotFoundTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F;
	F.Fake->On(TEXT("shop/purchases?"), FFlockFakeTransport::Ok(OpenList({ Purchase(TEXT("pur-2"), TEXT("pending")) })));
	F.Fake->On(TEXT("shop/purchases/pur-2"), FFlockFakeTransport::Coded(404, TEXT("shop.purchase_not_found")));
	F.Connect();
	F.SignIn(TEXT("player-a"));
	F.Tick();
	F.Tick();
	F.Tick();
	TestEqual(TEXT("Read once: one listed as open is already saved"), F.Reads(TEXT("pur-2")), 1);
	TestEqual(TEXT("Nothing reported"), F.Completed->Num(), 0);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutGatewayNamedTest, "AGatewayNamedIsTheOneEachCheckoutIsOpenedWith")
bool FFlockWebCheckoutGatewayNamedTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("paid"))));
	if (!TestTrue(TEXT("Connected"), F.Connect(FlockPaymentProviders::Tap).IsSuccess()))
	{
		return false;
	}
	TestEqualSensitive(TEXT("The gateway named"), F.Connected->GetPaymentProvider(), FString(FlockPaymentProviders::Tap));
	TestTrue(TEXT("A web checkout"), F.Connected->IsWebCheckout());
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	F.Tick();
	TestTrue(TEXT("Paid"), Bought.IsSet() && Bought->IsSuccess());
	const FFlockHttpRequest* Sent = F.LastTo(FlockEndpoints::ShopCheckout);
	TestTrue(TEXT("Opened with that gateway"), Sent != nullptr && Sent->JsonBody.Contains(TEXT("\"provider\":\"tap\""), ESearchCase::CaseSensitive));
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutIntervalTest, "CheckEverySecondsSetsHowOftenACheckoutIsRead")
bool FFlockWebCheckoutIntervalTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("pending"))));
	F.Connect(FString(), 10.f);
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	F.Tick(9.0);
	TestEqual(TEXT("Not read before ten seconds"), F.Reads(), 0);
	F.Tick(1.0);
	TestEqual(TEXT("Read at ten seconds"), F.Reads(), 1);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutTwiceTest, "ASecondPurchaseOfTheItemWhileOneWaitsIsRefusedAndOpensNoCheckout")
bool FFlockWebCheckoutTwiceTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("pending"))));
	F.Connect();
	TAnswer<TFlockResult<FFlockPurchaseResult>> First = F.Buy();
	TAnswer<TFlockResult<FFlockPurchaseResult>> Second = F.Buy();
	TestFalse(TEXT("The first still waits"), First.IsSet());
	TestTrue(TEXT("The second is refused"), Second.IsSet() && Second->Error.Type == EFlockErrorType::Validation && !Second->Error.Hint.IsEmpty());
	TestEqual(TEXT("One checkout"), F.Fake->CountTo(FlockEndpoints::ShopCheckout), 1);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutWalletItemTest, "AWalletItemIsBoughtFromTheWalletWithACheckoutConnected")
bool FFlockWebCheckoutWalletItemTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopTransaction, FFlockFakeTransport::Ok(TEXT("{\"purchase_id\":\"pur-w\",\"item_type\":\"standard\",\"status\":\"paid\",\"granted\":[]}")));
	F.Connect();
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy(TEXT("gold-item"));
	TestTrue(TEXT("Bought from the wallet"), Bought.IsSet() && Bought->IsSuccess() && Bought->Value.PurchaseId == TEXT("pur-w"));
	TestEqual(TEXT("No checkout"), F.Fake->CountTo(FlockEndpoints::ShopCheckout), 0);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutStoppedReadTest, "AReadStoppedByAFailureEndsTheWaitUnknownAndRestoreReadsTheCheckoutAgain")
bool FFlockWebCheckoutStoppedReadTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	// A proxy's 403 is not worth another try, so the reads stop; the page may still be paid.
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Status(403, TEXT("<html>Forbidden</html>")));
	F.Connect();
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	F.Tick();
	if (!TestTrue(TEXT("The wait ended"), Bought.IsSet()))
	{
		return false;
	}
	TestTrue(TEXT("Its outcome is unknown"), Bought->Error.bOutcomeUnknown);
	TestTrue(TEXT("The fix says it may still be paid"), Bought->Error.Hint.Contains(TEXT("may still be paid")));
	TestEqualSensitive(TEXT("A purchase failure"), Bought->Error.Operation, FString(TEXT("Purchase shop item")));
	// Paid meanwhile: a restore lists it still open (the list was answered first) and reads it again, and it is reported once.
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("paid"))));
	F.Fake->On(TEXT("shop/purchases?"), FFlockFakeTransport::Ok(OpenList({ Purchase(TEXT("pur-1"), TEXT("pending")) })));
	TAnswer<TFlockResult<FFlockStoreRestore>> Restored;
	F.Connected->Restore(Restored.Into());
	F.Tick();
	TestEqual(TEXT("Reported once paid"), F.Completed->Num(), 1);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutStoppedReadWalletTest, "AReadStoppedByAFailureDropsTheCachedWallet")
bool FFlockWebCheckoutStoppedReadWalletTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Status(403, TEXT("<html>Forbidden</html>")));
	F.Fake->On(FlockEndpoints::PlayerTemplate, FFlockFakeTransport::Ok(
		TEXT("{\"error\":{\"code\":null},\"response\":{\"message\":null,\"code\":null},\"result\":[{\"id\":\"tpl-wallet\",\"name\":\"currencies\",\"schema\":[],\"data\":[],\"tag\":\"currency\"}]}")));
	F.Fake->OnSequence(TEXT("player_data?page="), { FFlockFakeTransport::Ok(WalletPage(0)), FFlockFakeTransport::Ok(WalletPage(100)) });
	TAnswer<TFlockResult<FFlockWallet>> Before;
	F.Sdk->GetPlayerProvider()->GetWallet(Before.Into());
	F.Connect();
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	F.Tick();
	TestTrue(TEXT("Ended unknown"), Bought.IsSet() && Bought->Error.bOutcomeUnknown);
	TAnswer<TFlockResult<FFlockWallet>> After;
	F.Sdk->GetPlayerProvider()->GetWallet(After.Into());
	TestTrue(TEXT("The server's balance, not the cached 0"), After.IsSet() && After->IsSuccess() && After->Value.BalanceOf(TEXT("Gems")) == 100);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutUnknownStatusReadTest, "AStatusThisSdkDoesNotKnowKeepsTheCheckoutRead")
bool FFlockWebCheckoutUnknownStatusReadTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->OnSequence(TEXT("shop/purchases/pur-1"), { FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("processing"))),
		FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("paid"))) });
	F.Connect();
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	F.Tick();
	TestFalse(TEXT("A status added later does not end the wait"), Bought.IsSet());
	F.Tick();
	TestTrue(TEXT("Paid"), Bought.IsSet() && Bought->IsSuccess());
	TestEqual(TEXT("Reported once"), F.Completed->Num(), 1);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutOtherPurchaseTest, "AReadNamingAnotherPurchaseIsReadAgainAndNotReported")
bool FFlockWebCheckoutOtherPurchaseTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->OnSequence(TEXT("shop/purchases/pur-1"), { FFlockFakeTransport::Ok(Purchase(TEXT("pur-9"), TEXT("paid"))),
		FFlockFakeTransport::Ok(Purchase(TEXT("PUR-1"), TEXT("paid"))), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("pending"))) });
	F.Connect();
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	for (int32 Frame = 0; Frame < 6; ++Frame)
	{
		F.Tick();
	}
	TestFalse(TEXT("Another purchase's paid answer is not this checkout's"), Bought.IsSet());
	TestEqual(TEXT("Nothing reported"), F.Completed->Num(), 0);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutSameSignInTest, "ASecondSignInEventForTheSameSignInDoesNotReportAnEndedCheckoutAgain")
bool FFlockWebCheckoutSameSignInTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("paid"))));
	F.Connect();
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	F.Tick();
	TestTrue(TEXT("Paid"), Bought.IsSet() && Bought->IsSuccess());
	// The same sign-in raised again (a restore answered after the connect), its list still showing the checkout open.
	F.Fake->On(TEXT("shop/purchases?"), FFlockFakeTransport::Ok(OpenList({ Purchase(TEXT("pur-1"), TEXT("pending")) })));
	F.Sdk->GetEvents()->OnAuthenticated.Broadcast(FFlockAuthInfo());
	F.Tick();
	F.Tick();
	TestEqual(TEXT("Not read again"), F.Reads(), 1);
	TestEqual(TEXT("Reported once"), F.Completed->Num(), 1);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutBadIntervalTest, "AnIntervalThatIsNotANumberReadsEveryThreeSeconds")
bool FFlockWebCheckoutBadIntervalTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("pending"))));
	F.Connect(FString(), FMath::Sqrt(-1.f));
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	F.Tick();
	TestEqual(TEXT("Read at three seconds"), F.Reads(), 1);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutNewItemTest, "AnItemAddedAfterTheConnectIsBoughtAsItIsNow")
bool FFlockWebCheckoutNewItemTest::RunTest(const FString& Parameters)
{
	FFixture F({ GoldItem });
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("paid"))));
	F.Connect();
	// Added in the dashboard after the connect.
	F.ServeItem(Sword);
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	F.Tick();
	TestTrue(TEXT("Bought"), Bought.IsSet() && Bought->IsSuccess());
	TestTrue(TEXT("Reported with the item as read now"), F.Completed->Num() == 1 && (*F.Completed)[0] == TEXT("sword:pur-1"));
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutSwitchWhileReadTest, "APlayerSwitchWhileTheItemIsReadBuysNothing")
bool FFlockWebCheckoutSwitchWhileReadTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Connect();
	F.Fake->bDeferred = true;
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	F.Fake->bDeferred = false;
	F.SignIn(TEXT("player-b"));
	F.Fake->FlushPending();
	TestTrue(TEXT("Cancelled"), Bought.IsSet() && Bought->Error.Type == EFlockErrorType::Cancelled);
	TestEqual(TEXT("No checkout"), F.Fake->CountTo(FlockEndpoints::ShopCheckout), 0);
	TestEqual(TEXT("No page"), F.Opened->Num(), 0);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutStoreSwitchWhileReadTest, "AStoresOwnPurchaseBuysNothingWhenThePlayerSwitchesWhileTheItemIsRead")
bool FFlockWebCheckoutStoreSwitchWhileReadTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Connect();
	F.Fake->bDeferred = true;
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought;
	F.Connected->Purchase(TEXT("sword"), Bought.Into());
	F.Fake->bDeferred = false;
	F.SignIn(TEXT("player-b"));
	F.Fake->FlushPending();
	TestTrue(TEXT("Cancelled"), Bought.IsSet() && Bought->Error.Type == EFlockErrorType::Cancelled);
	TestEqual(TEXT("No checkout"), F.Fake->CountTo(FlockEndpoints::ShopCheckout), 0);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutExpiredSignInTest, "ASignInThatExpiresEndsTheWait")
bool FFlockWebCheckoutExpiredSignInTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Connect();
	TAnswer<TFlockResult<FFlockPurchaseResult>> Bought = F.Buy();
	// The token runs out and its renewal is refused: the sign-in ends.
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Status(401, TEXT("{\"detail\":{\"code\":\"auth.token_expired\",\"message\":\"Token expired\"}}")));
	F.Fake->On(FlockEndpoints::PlayerTokenRefresh, FFlockFakeTransport::Status(401, TEXT("{\"detail\":{\"code\":\"auth.invalid_refresh_token\",\"message\":\"Invalid refresh token\"}}")));
	F.Tick();
	TestTrue(TEXT("The wait ended"), Bought.IsSet() && !Bought->IsSuccess());
	const int32 Reads = F.Reads();
	F.Tick();
	F.Tick();
	TestEqual(TEXT("Nothing more read"), F.Reads(), Reads);
	return true;
}

// ── Price, restore, setup check ──

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutPriceTest, "GetPriceIsTheItemsOwnPriceInTheCurrencysSmallestUnit")
bool FFlockWebCheckoutPriceTest::RunTest(const FString& Parameters)
{
	struct FCase { const TCHAR* Currency; int32 Price; double Amount; int32 Digits; };
	for (const FCase& Case : { FCase{ TEXT("SAR"), 799, 7.99, 2 }, FCase{ TEXT("sar"), 199, 1.99, 2 }, FCase{ TEXT("KWD"), 1250, 1.250, 3 }, FCase{ TEXT("JPY"), 500, 500.0, 0 } })
	{
		const FString Item = FString::Printf(TEXT("{\"id\":\"sword\",\"name\":\"Sword\",\"type\":\"standard\",\"price\":%d,\"currency\":\"%s\",\"is_real_money\":true,\"store_skus\":{},\"status\":\"active\"}"),
			Case.Price, Case.Currency);
		FFixture F({ Item });
		F.Connect();
		const TFlockResult<FFlockStorePrice> Shown = F.Connected != nullptr ? F.Connected->GetPrice(TEXT("sword")) : TFlockResult<FFlockStorePrice>::Fail(FFlockError());
		if (!TestTrue(FString::Printf(TEXT("%s: priced"), Case.Currency), Shown.IsSuccess()))
		{
			continue;
		}
		FNumberFormattingOptions Format;
		Format.MinimumFractionalDigits = Case.Digits;
		Format.MaximumFractionalDigits = Case.Digits;
		const FString Currency = FString(Case.Currency).ToUpper();
		TestEqual(FString::Printf(TEXT("%s: the smallest unit"), Case.Currency), Shown.Value.AmountInSmallestUnit, static_cast<int64>(Case.Price));
		TestEqualSensitive(FString::Printf(TEXT("%s: the code"), Case.Currency), Shown.Value.CurrencyCode, Currency);
		TestEqualSensitive(FString::Printf(TEXT("%s: written out"), Case.Currency), Shown.Value.Text, Currency + TEXT(" ") + FText::AsNumber(Case.Amount, &Format).ToString());
		TestTrue(FString::Printf(TEXT("%s: found"), Case.Currency), Shown.Value.bFoundInTheStore);
	}
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutPriceRefusedTest, "GetPriceOfAnItemWithNoRealMoneyPriceIsRefused")
bool FFlockWebCheckoutPriceRefusedTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Connect();
	if (!TestNotNull(TEXT("Connected"), F.Connected))
	{
		return false;
	}
	const TFlockResult<FFlockStorePrice> GameCurrency = F.Connected->GetPrice(TEXT("gold-item"));
	const TFlockResult<FFlockStorePrice> Unknown = F.Connected->GetPrice(TEXT("shield"));
	TestTrue(TEXT("A game-currency item is refused"), !GameCurrency.IsSuccess() && GameCurrency.Error.Message.Contains(TEXT("game currency")));
	TestTrue(TEXT("An item not in the shops is refused"), !Unknown.IsSuccess() && Unknown.Error.Message.Contains(TEXT("not in the game's shops")));
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutRestoreFailsTest, "RestoreWhenTheOpenCheckoutsCannotBeListedFailsWithWhy")
bool FFlockWebCheckoutRestoreFailsTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	F.Fake->On(TEXT("shop/purchases?"), FFlockFakeTransport::Coded(422, TEXT("shop.invalid_purchase_status")));
	TAnswer<TFlockResult<FFlockStoreRestore>> Restored;
	F.Connected->Restore(Restored.Into());
	TestTrue(TEXT("Failed with the server's code"), Restored.IsSet() && Restored->Error.ErrorCode == EFlockErrorCode::ShopInvalidPurchaseStatus);
	F.Sdk->GetAuthProvider()->Logout();
	TAnswer<TFlockResult<FFlockStoreRestore>> SignedOut;
	F.Connected->Restore(SignedOut.Into());
	TestTrue(TEXT("Needs a signed-in player"), SignedOut.IsSet() && SignedOut->Error.Type == EFlockErrorType::Auth);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutSetupCheckTest, "CheckSetupSaysHowToCheckTheGatewayAndOpensNoCheckout")
bool FFlockWebCheckoutSetupCheckTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	TAnswer<TFlockResult<FFlockStoreSetupCheck>> Checked;
	F.Connected->CheckSetup(Checked.Into());
	if (!TestTrue(TEXT("Checked"), Checked.IsSet() && Checked->IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("Says how to check the gateway"), Checked->Value.NotChecked.Contains(TEXT("test card")));
	TestTrue(TEXT("Ready: nothing found to fix, and NotChecked says how to check the gateway by hand"), Checked->Value.IsReady());
	TestEqual(TEXT("No checkout"), F.Fake->CountTo(FlockEndpoints::ShopCheckout), 0);
	TestEqual(TEXT("No receipt"), F.Fake->CountTo(FlockEndpoints::ShopValidateReceipt), 0);
	return true;
}

// ── The connection ──

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutShopsUnreadTest, "AConnectWhoseShopsCannotBeReadIsRefusedAndListensToNothing")
bool FFlockWebCheckoutShopsUnreadTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F;
	F.Fake->On(TEXT("shop?page="), FFlockFakeTransport::Coded(401, TEXT("auth.invalid_api_key")));
	const TFlockResult<UFlockStore*> Result = F.Connect();
	TestFalse(TEXT("Refused"), Result.IsSuccess());
	TestNull(TEXT("Nothing held"), F.Sdk->GetStore());
	F.SignIn(TEXT("player-a"));
	TestEqual(TEXT("A sign-in lists nothing"), F.Fake->CountTo(TEXT("shop/purchases?")), 0);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutUnknownProviderTest, "AWayToPayFlockDoesNotKnowIsRefusedNamingBothKinds")
bool FFlockWebCheckoutUnknownProviderTest::RunTest(const FString& Parameters)
{
	FFixture F;
	const TFlockResult<UFlockStore*> Result = F.Connect(TEXT("paypal"));
	TestFalse(TEXT("Refused"), Result.IsSuccess());
	TestTrue(TEXT("The fix names the stores and the gateways"), Result.Error.Hint.Contains(TEXT("GooglePlay")) && Result.Error.Hint.Contains(TEXT("Stripe")));
	const TFlockResult<UFlockStore*> Spelled = F.Connect(TEXT("Stripe"));
	TestFalse(TEXT("A gateway is named letter for letter"), Spelled.IsSuccess());
	TestEqual(TEXT("Nothing read"), F.Fake->CountTo(TEXT("shop?page=")), 0);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutSteamTest, "SteamIsReadFromTheEngineDefaultOnlineService")
bool FFlockWebCheckoutSteamTest::RunTest(const FString& Parameters)
{
	FString Saved;
	const bool bHadOne = GConfig->GetString(TEXT("OnlineSubsystem"), TEXT("DefaultPlatformService"), Saved, GEngineIni);
	GConfig->SetString(TEXT("OnlineSubsystem"), TEXT("DefaultPlatformService"), TEXT("Steam"), GEngineIni);
	const bool bSteam = UFlockStore::IsSteamTheDefaultService();
	GConfig->SetString(TEXT("OnlineSubsystem"), TEXT("DefaultPlatformService"), TEXT("EOS"), GEngineIni);
	const bool bEpic = UFlockStore::IsSteamTheDefaultService();
	if (bHadOne)
	{
		GConfig->SetString(TEXT("OnlineSubsystem"), TEXT("DefaultPlatformService"), *Saved, GEngineIni);
	}
	else
	{
		GConfig->RemoveKey(TEXT("OnlineSubsystem"), TEXT("DefaultPlatformService"), GEngineIni);
	}
	TestTrue(TEXT("Steam is recognized"), bSteam);
	TestFalse(TEXT("Another service is not Steam"), bEpic);
	return true;
}

// ── Blueprint nodes over a web checkout ──

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutNodesEndToEndTest, "Nodes.ABlueprintOnlyGameConnectsPricesBuysReadsAndRestores")
bool FFlockWebCheckoutNodesEndToEndTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->OnSequence(TEXT("shop/purchases/pur-1"), { FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("pending"))),
		FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("paid"), Granted10Gold)) });
	FFlockStoreNodes Events;
	Events->Hear(F.Sdk->GetEvents());

	// Connect Store with the options a graph sets; the page opener is the test's, as a graph's default browser would be.
	FFlockStoreOptions Options;
	const TSharedRef<TArray<FString>> Pages = F.Opened;
	Options.OpenPaymentPage = [Pages](const FString& Url) { Pages->Add(Url); };
	FFlockStoreNodes Connect;
	Connect.Connect(UFlockConnectStoreAction::ConnectStore(nullptr, Options))->ActivateWithFlockForTesting(F.Sdk);
	if (!TestEqualSensitive(TEXT("Connect Store: the success pin, once"), Connect->FiredText(), FString(TEXT("success")))
		|| !TestTrue(TEXT("A web checkout Flock holds"), Connect->Store != nullptr && Connect->Store->IsWebCheckout() && F.Sdk->GetStore() == Connect->Store))
	{
		return false;
	}

	FFlockStorePrice Price;
	FFlockError PriceError;
	TestTrue(TEXT("Get Store Price"), UFlockStoreLibrary::GetStorePriceWithFlockForTesting(F.Sdk, TEXT("sword"), Price, PriceError));
	TestEqual(TEXT("The item's own price"), Price.AmountInSmallestUnit, static_cast<int64>(799));
	TestEqualSensitive(TEXT("In its currency"), Price.CurrencyCode, FString(TEXT("SAR")));

	FFlockStoreNodes Setup;
	Setup.Setup(UFlockCheckStoreSetupAction::CheckStoreSetup(nullptr))->ActivateWithFlockForTesting(F.Sdk);
	TestEqualSensitive(TEXT("Check Store Setup: the success pin, once"), Setup->FiredText(), FString(TEXT("success")));
	TestFalse(TEXT("Saying how to check the gateway"), Setup->SetupCheck.NotChecked.IsEmpty());
	TestTrue(TEXT("Ready: nothing found to fix"), UFlockStoreLibrary::IsStoreReady(Setup->SetupCheck));
	TestEqual(TEXT("Without opening a checkout"), F.Fake->CountTo(FlockEndpoints::ShopCheckout), 0);

	FFlockStoreNodes Buy;
	Buy.Buy(UFlockPurchaseAction::Purchase(nullptr, TEXT("sword"), FString()))->ActivateWithFlockForTesting(F.Sdk);
	TestEqual(TEXT("The payment page opened once"), F.Opened->Num(), 1);
	TestTrue(TEXT("No pin while the payment is waited for"), Buy->Fired.IsEmpty());
	F.Tick();
	TestTrue(TEXT("Still none while pending"), Buy->Fired.IsEmpty());
	F.Tick();
	TestEqualSensitive(TEXT("Flock Purchase: the success pin, once"), Buy->FiredText(), FString(TEXT("success")));
	TestTrue(TEXT("Paid"), Buy->PurchaseResult.IsPaid());
	TestEqual(TEXT("Flock's events heard it once"), Events->Completed, TArray<FString>{ TEXT("sword:pur-1") });
	F.Tick();
	TestEqual(TEXT("And no more"), Events->Completed.Num(), 1);

	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("paid"), Granted10Gold)));
	FFlockStoreNodes Read;
	Read.Read(UFlockGetPlayerPurchaseAction::GetPlayerPurchase(nullptr, TEXT("pur-1")))->ActivateWithFlockForTesting(F.Sdk);
	TestEqualSensitive(TEXT("Get Player Purchase: the success pin, once"), Read->FiredText(), FString(TEXT("success")));
	TestTrue(TEXT("Is Player Purchase Paid"), UFlockStoreLibrary::IsPlayerPurchasePaid(Read->Purchase));

	FFlockStoreNodes List;
	List.List(UFlockGetPlayerPurchasesAction::GetPlayerPurchases(nullptr, UFlockShopLibrary::PurchaseStatusPending(), 1, 100))->ActivateWithFlockForTesting(F.Sdk);
	TestEqualSensitive(TEXT("Get Player Purchases: the success pin, once"), List->FiredText(), FString(TEXT("success")));
	TestEqual(TEXT("Nothing left open"), List->Page.Items.Num(), 0);
	const FFlockHttpRequest* Listed = F.LastTo(TEXT("shop/purchases?"));
	TestTrue(TEXT("Asked for the pending ones"), Listed != nullptr && Listed->Url.Contains(TEXT("status=pending"), ESearchCase::CaseSensitive));

	FFlockStoreNodes Restore;
	Restore.Restore(UFlockRestorePurchasesAction::RestorePurchases(nullptr))->ActivateWithFlockForTesting(F.Sdk);
	TestEqualSensitive(TEXT("Restore Purchases: the success pin, once"), Restore->FiredText(), FString(TEXT("success")));
	TestEqual(TEXT("Both lists empty on a web checkout"), Restore->Restore.Restored.Num() + Restore->Restore.NotDelivered.Num(), 0);
	TestEqual(TEXT("Still heard once"), Events->Completed.Num(), 1);
	TestEqual(TEXT("One checkout in all"), F.Fake->CountTo(FlockEndpoints::ShopCheckout), 1);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutNodesExpiredTest, "Nodes.ACheckoutThatExpiresFiresThePurchaseNodesFailurePinWithTheExpiredReason")
bool FFlockWebCheckoutNodesExpiredTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("expired"))));
	F.Connect();
	FFlockStoreNodes Nodes;
	Nodes->Hear(F.Sdk->GetEvents());
	Nodes.Buy(UFlockPurchaseAction::Purchase(nullptr, TEXT("sword"), FString()))->ActivateWithFlockForTesting(F.Sdk);
	F.Tick();
	TestEqualSensitive(TEXT("The failure pin, once"), Nodes->FiredText(), FString(TEXT("failure")));
	TestEqualSensitive(TEXT("Its reason is what the Expired node spells"), Nodes->Error.StoreFailureReason, UFlockStoreLibrary::StoreFailureReasonExpired());
	TestFalse(TEXT("Nothing charged, so nothing in doubt"), Nodes->Error.bOutcomeUnknown);
	TestEqual(TEXT("Nothing heard"), Nodes->Completed.Num() + Nodes->NotDelivered.Num(), 0);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutNodesUnknownTest, "Nodes.AReadStoppedByAFailureFiresThePurchaseNodesFailurePinWithTheOutcomeUnknown")
bool FFlockWebCheckoutNodesUnknownTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
	// A proxy's 403 is not worth another try, so the reads stop; the page may still be paid.
	F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Status(403, TEXT("<html>Forbidden</html>")));
	F.Connect();
	FFlockStoreNodes Nodes;
	Nodes->Hear(F.Sdk->GetEvents());
	Nodes.Buy(UFlockPurchaseAction::Purchase(nullptr, TEXT("sword"), FString()))->ActivateWithFlockForTesting(F.Sdk);
	F.Tick();
	TestEqualSensitive(TEXT("The failure pin, once"), Nodes->FiredText(), FString(TEXT("failure")));
	TestEqual(TEXT("Never reported as not delivered on a web checkout"), Nodes->NotDelivered.Num(), 0);
	TestTrue(TEXT("Its outcome is unknown"), Nodes->Error.bOutcomeUnknown);
	TestTrue(TEXT("The fix says it may still be paid"), Nodes->Error.Hint.Contains(TEXT("may still be paid")));
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutNodesStartCheckoutTest, "Nodes.StartCheckoutOpensACheckoutAndOneWhoseAnswerNeverCameIsUnknownAndNotSentAgain")
bool FFlockWebCheckoutNodesStartCheckoutTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	{
		FFixture F;
		F.SignIn(TEXT("player-a"));
		F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
		FFlockStoreNodes Nodes;
		Nodes.Checkout(UFlockStartCheckoutAction::StartCheckout(nullptr, TEXT("sword"), UFlockShopLibrary::PaymentProviderMoyasar(),
			TEXT("https://game.example/done"), FString()))->ActivateWithFlockForTesting(F.Sdk);
		TestEqualSensitive(TEXT("The success pin, once"), Nodes->FiredText(), FString(TEXT("success")));
		TestEqualSensitive(TEXT("The page"), Nodes->Checkout.PaymentUrl, PaymentPage);
		TestEqualSensitive(TEXT("The purchase to read"), Nodes->Checkout.PurchaseId, FString(TEXT("pur-1")));
		const FFlockHttpRequest* Sent = F.LastTo(FlockEndpoints::ShopCheckout);
		TestTrue(TEXT("The pins sent as set, the empty one left out"), Sent != nullptr && Sent->JsonBody.Equals(
			TEXT("{\"shop_item_id\":\"sword\",\"provider\":\"moyasar\",\"return_url\":\"https://game.example/done\"}"), ESearchCase::CaseSensitive));
		TestEqual(TEXT("No page opened by Flock: the game shows it"), F.Opened->Num(), 0);
	}
	{
		FFixture F;
		F.SignIn(TEXT("player-a"));
		F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Timeout());
		FFlockStoreNodes Nodes;
		Nodes.Checkout(UFlockStartCheckoutAction::StartCheckout(nullptr, TEXT("sword"), FString(), FString(), FString()))->ActivateWithFlockForTesting(F.Sdk);
		for (int32 Frame = 0; Frame < 20 && Nodes->Fired.IsEmpty(); ++Frame)
		{
			FTSTicker::GetCoreTicker().Tick(1.f);
		}
		TestEqualSensitive(TEXT("No answer: the failure pin, once"), Nodes->FiredText(), FString(TEXT("failure")));
		TestTrue(TEXT("Its outcome is unknown"), Nodes->Error.bOutcomeUnknown);
		TestEqual(TEXT("Never sent again"), F.Fake->CountTo(FlockEndpoints::ShopCheckout), 1);
	}
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutNodesSignedOutTest, "Nodes.StartCheckoutGetPurchaseAndGetPurchasesWithNobodySignedInFireTheirFailurePins")
bool FFlockWebCheckoutNodesSignedOutTest::RunTest(const FString& Parameters)
{
	FFixture F;
	FFlockStoreNodes Opened;
	Opened.Checkout(UFlockStartCheckoutAction::StartCheckout(nullptr, TEXT("sword"), FString(), FString(), FString()))->ActivateWithFlockForTesting(F.Sdk);
	FFlockStoreNodes Read;
	Read.Read(UFlockGetPlayerPurchaseAction::GetPlayerPurchase(nullptr, TEXT("pur-1")))->ActivateWithFlockForTesting(F.Sdk);
	FFlockStoreNodes Listed;
	Listed.List(UFlockGetPlayerPurchasesAction::GetPlayerPurchases(nullptr, FString(), 1, 100))->ActivateWithFlockForTesting(F.Sdk);
	for (const FFlockStoreNodes* Nodes : { &Opened, &Read, &Listed })
	{
		TestEqualSensitive(TEXT("The failure pin, once"), (*Nodes)->FiredText(), FString(TEXT("failure")));
		TestEqual(TEXT("A sign-in failure"), (*Nodes)->Error.Type, EFlockErrorType::Auth);
		TestFalse(TEXT("With a fix"), (*Nodes)->Error.Hint.IsEmpty());
	}
	TestEqual(TEXT("Nothing sent"), F.Fake->CountTo(TEXT("shop/checkout")) + F.Fake->CountTo(TEXT("shop/purchases")), 0);
	return true;
}

FLOCK_WEB_CHECKOUT_TEST(FFlockWebCheckoutNodesKeptTest, "Nodes.APurchaseNodeNothingKeepsIsKeptByTheGameInstanceUntilItsPinFires")
bool FFlockWebCheckoutNodesKeptTest::RunTest(const FString& Parameters)
{
	// A web checkout waits up to an hour; the widget that bought may be closed meanwhile. The node, not the widget, answers.
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	UWorld* Level = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("FlockStoreNodesLevel"));
	Level->SetGameInstance(Instance.Get());
	ON_SCOPE_EXIT
	{
		Level->SetGameInstance(nullptr);
		Level->DestroyWorld(/*bInformEngineOfWorld*/ false);
	};
	auto Buy = [](FFixture& F, UObject* WorldContextObject, UFlockStoreNodeTestListener* Listener)
	{
		F.SignIn(TEXT("player-a"));
		F.Fake->On(FlockEndpoints::ShopCheckout, FFlockFakeTransport::Ok(Checkout));
		F.Fake->On(TEXT("shop/purchases/pur-1"), FFlockFakeTransport::Ok(Purchase(TEXT("pur-1"), TEXT("paid"))));
		F.Connect();
		UFlockPurchaseAction* Action = UFlockPurchaseAction::Purchase(WorldContextObject, TEXT("sword"), FString());
		Action->OnSuccess.AddDynamic(Listener, &UFlockStoreNodeTestListener::Bought);
		Action->OnFailure.AddDynamic(Listener, &UFlockStoreNodeTestListener::NotBought);
		Action->ActivateWithFlockForTesting(F.Sdk);
		return TWeakObjectPtr<UFlockPurchaseAction>(Action);
	};
	{
		// The control: a node nothing keeps is gone at the next collection, which is what the game instance prevents.
		FFixture F;
		const TStrongObjectPtr<UFlockStoreNodeTestListener> Listener(NewObject<UFlockStoreNodeTestListener>());
		const TWeakObjectPtr<UFlockPurchaseAction> Unkept = Buy(F, nullptr, Listener.Get());
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestFalse(TEXT("control: a node with no game instance is collected"), Unkept.IsValid());
		F.Tick();
		TestTrue(TEXT("control: and its pin never fires"), Listener->Fired.IsEmpty());
	}
	{
		FFixture F;
		const TStrongObjectPtr<UFlockStoreNodeTestListener> Listener(NewObject<UFlockStoreNodeTestListener>());
		const TWeakObjectPtr<UFlockPurchaseAction> Kept = Buy(F, Level, Listener.Get());
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestTrue(TEXT("kept while the payment is waited for"), Kept.IsValid());
		F.Tick();
		TestEqualSensitive(TEXT("and its pin fires once paid"), Listener->FiredText(), FString(TEXT("success")));
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestFalse(TEXT("let go once its pin has fired"), Kept.IsValid());
	}
	{
		// The store's own nodes are kept the same way.
		FFixture F;
		F.SignIn(TEXT("player-a"));
		F.Fake->bDeferred = true;
		const TStrongObjectPtr<UFlockStoreNodeTestListener> Listener(NewObject<UFlockStoreNodeTestListener>());
		UFlockGetPlayerPurchasesAction* Action = UFlockGetPlayerPurchasesAction::GetPlayerPurchases(Level, FString(), 1, 100);
		Action->OnSuccess.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::PageRead);
		Action->ActivateWithFlockForTesting(F.Sdk);
		const TWeakObjectPtr<UFlockGetPlayerPurchasesAction> Kept(Action);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestTrue(TEXT("a store node is kept while its call is out"), Kept.IsValid());
		F.Fake->bDeferred = false;
		F.Fake->FlushPending();
		TestEqualSensitive(TEXT("and its pin fires"), Listener->FiredText(), FString(TEXT("success")));
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestFalse(TEXT("let go once its success pin has fired"), Kept.IsValid());
	}
	{
		// Let go on the failure pin too.
		FFixture F;
		F.SignIn(TEXT("player-a"));
		F.Fake->bDeferred = true;
		F.Fake->On(TEXT("shop/purchases?"), FFlockFakeTransport::Coded(400, TEXT("shop.invalid_purchase_status")));
		const TStrongObjectPtr<UFlockStoreNodeTestListener> Listener(NewObject<UFlockStoreNodeTestListener>());
		UFlockGetPlayerPurchasesAction* Action = UFlockGetPlayerPurchasesAction::GetPlayerPurchases(Level, TEXT("unpaid"), 1, 100);
		Action->OnFailure.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::PageReadFailed);
		Action->ActivateWithFlockForTesting(F.Sdk);
		const TWeakObjectPtr<UFlockGetPlayerPurchasesAction> Kept(Action);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestTrue(TEXT("kept while its call is out"), Kept.IsValid());
		F.Fake->bDeferred = false;
		F.Fake->FlushPending();
		TestEqualSensitive(TEXT("its failure pin fires"), Listener->FiredText(), FString(TEXT("failure")));
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestFalse(TEXT("let go once its failure pin has fired"), Kept.IsValid());
	}
	{
		// A waiting node does not keep what called it, nor so its level, alive.
		FFixture F;
		F.SignIn(TEXT("player-a"));
		F.Fake->bDeferred = true;
		TWeakObjectPtr<UObject> Caller = NewObject<UFlockStoreNodeTestListener>(Level);
		const TStrongObjectPtr<UFlockStoreNodeTestListener> Listener(NewObject<UFlockStoreNodeTestListener>());
		UFlockGetPlayerPurchasesAction* Action = UFlockGetPlayerPurchasesAction::GetPlayerPurchases(Caller.Get(), FString(), 1, 100);
		Action->OnSuccess.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::PageRead);
		Action->ActivateWithFlockForTesting(F.Sdk);
		const TWeakObjectPtr<UFlockGetPlayerPurchasesAction> Kept(Action);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestTrue(TEXT("the node is kept"), Kept.IsValid());
		TestFalse(TEXT("its caller is not"), Caller.IsValid());
		F.Fake->bDeferred = false;
		F.Fake->FlushPending();
		TestEqualSensitive(TEXT("and its pin still fires"), Listener->FiredText(), FString(TEXT("success")));
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
