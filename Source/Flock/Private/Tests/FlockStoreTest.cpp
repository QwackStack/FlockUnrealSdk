// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Config/FlockConfig.h"
#include "Containers/Ticker.h"
#include "FlockEvents.h"
#include "FlockInitConfig.h"
#include "FlockSubsystem.h"
#include "Misc/Base64.h"
#include "Misc/ScopeExit.h"
#include "Providers/FlockAuthProvider.h"
#include "Providers/FlockShopProvider.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Store/FlockStore.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockRecordingLogger.h"
#include "Tests/Support/FlockTestSdk.h"
#include "Tests/Support/FlockTestSpelling.h"
#include "Tests/Support/FlockStoreNodeTestListener.h"
#include "Blueprint/FlockStoreLibrary.h"
#include "Http/FlockErrorHints.h"

// The store purchase rules, driven through FFlockShopProvider::Purchase and a store of the test's own: Flock delivers before the
// store's purchase is finished, a refused one stays unfinished, unfinished ones wait for a sign-in and go once a launch, and
// orders for products no Flock item sells are the game's.

#define FLOCK_STORE_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Store." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

namespace FlockStoreTesting
{
	/** A store of the test's own: every call recorded, every answer handed over by the test. */
	class FFakeStore : public IFlockStoreConnection
	{
	public:
		TArray<FString> Calls;
		TFunction<void(const FString&)> OnBuy;
		bool bConnectFails = false;
		bool bConnectWaits = false;
		TFunction<void(const FFlockError&)> PendingConnect;
		TMap<FString, FFlockStorePrice> Prices;
		TArray<FFlockStoreOrder> Owned;
		TArray<FFlockStoreOrder> Unfinished;
		bool bRestoreFails = false;
		FListener Listener;
		FString Provider = FlockPaymentProviders::GooglePlay;

		virtual FString GetPaymentProvider() const override { return Provider; }
		virtual void Listen(FListener InListener) override { Listener = MoveTemp(InListener); }
		virtual void Connect(TFunction<void(const FFlockError&)> OnDone) override
		{
			Calls.Add(TEXT("connect"));
			if (bConnectWaits)
			{
				PendingConnect = MoveTemp(OnDone);
				return;
			}
			OnDone(bConnectFails ? FFlockError::MakeWithFix(EFlockErrorType::Connection, TEXT("The store did not connect"), TEXT("Try again.")) : FFlockError());
		}
		virtual void RegisterProducts(const TArray<FFlockStoreProduct>& Products, TFunction<void()> OnDone) override
		{
			for (const FFlockStoreProduct& Product : Products)
			{
				Calls.Add(FString::Printf(TEXT("register:%s:%s"), *Product.ProductId, Product.bConsumable ? TEXT("consumable") : TEXT("one-time")));
			}
			OnDone();
		}
		virtual void Buy(const FString& ProductId) override
		{
			Calls.Add(TEXT("buy:") + ProductId);
			if (OnBuy)
			{
				OnBuy(ProductId);
			}
		}
		virtual void Finish(const FFlockStoreOrder& Order) override { Calls.Add(TEXT("finish:") + Order.TransactionId); }
		virtual void FetchUnfinishedPurchases() override { Calls.Add(TEXT("fetch")); }
		virtual FFlockStorePrice PriceOf(const FString& ProductId) const override
		{
			const FFlockStorePrice* Price = Prices.Find(ProductId);
			return Price ? *Price : FFlockStorePrice();
		}
		virtual void FetchOwnedPurchases(TFunction<void(TFlockResult<TArray<FFlockStoreOrder>>)> OnDone) override
		{
			Calls.Add(TEXT("restore"));
			if (bRestoreFails)
			{
				OnDone(TFlockResult<TArray<FFlockStoreOrder>>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Connection, TEXT("The store could not list"), TEXT("Try again."))));
				return;
			}
			// As the engine's store does: every unfinished purchase is handed over as paid before the owned ones come back.
			for (const FFlockStoreOrder& Order : TArray<FFlockStoreOrder>(Unfinished))
			{
				Pay(Order.ProductId, Order.TransactionId);
			}
			OnDone(TFlockResult<TArray<FFlockStoreOrder>>::Ok(Owned));
		}
		virtual void StopListening() override { Calls.Add(TEXT("stop")); Listener = FListener(); }

		/** A purchase the store hands back (unfinished at an earlier launch, or paid after its dialog closed). */
		void Pay(const FString& ProductId, const FString& TransactionId) { if (Listener.OnPaid) { Listener.OnPaid({ ProductId, TransactionId }); } }
		/** The purchase dialog's own answer: the store charged for the purchase Buy opened. */
		void PayFromTheDialog(const FString& ProductId, const FString& TransactionId)
		{
			FFlockStoreOrder Order{ ProductId, TransactionId };
			Order.bAnswersThePurchaseDialog = true;
			if (Listener.OnPaid) { Listener.OnPaid(Order); }
		}
		void Defer(const FString& ProductId)
		{
			FFlockStoreOrder Order{ ProductId, FString() };
			Order.bAnswersThePurchaseDialog = true;
			if (Listener.OnDeferred) { Listener.OnDeferred(Order); }
		}
		void Fail(const FString& ProductId, const FString& Reason, const FString& Text) { if (Listener.OnFailed) { Listener.OnFailed({ ProductId, Reason, Text }); } }
		void Own(const FString& ProductId, const FString& TransactionId) { Owned.Add({ ProductId, TransactionId }); }

		int32 Count(const FString& Call) const { return Calls.FilterByPredicate([&Call](const FString& C) { return C.Equals(Call, ESearchCase::CaseSensitive); }).Num(); }
		bool AnyStartingWith(const FString& Prefix) const { return Calls.ContainsByPredicate([&Prefix](const FString& C) { return C.StartsWith(Prefix, ESearchCase::CaseSensitive); }); }
	};

	const FString Gems = TEXT("com.flock.gems");
	const FString Sword = TEXT("com.flock.sword");
	const FString Twin = TEXT("com.flock.twin");

	inline FString Item(const FString& Id, const FString& Type, const FString& Skus, bool bRealMoney = true)
	{
		return FString::Printf(TEXT("{\"id\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"price\":199,\"currency\":\"USD\",\"is_real_money\":%s,\"store_skus\":%s}"),
			*Id, *Id, *Type, bRealMoney ? TEXT("true") : TEXT("false"), *Skus);
	}

	inline FString ShopOf(const TArray<FString>& Items)
	{
		return FString::Printf(TEXT("{\"id\":\"shop-1\",\"name\":\"main\",\"shop_items\":[%s]}"), *FString::Join(Items, TEXT(",")));
	}

	inline TArray<FString> CatalogItems()
	{
		return {
			Item(TEXT("gems"), TEXT("currency_pack"), FString::Printf(TEXT("{\"google_play\":\"%s\",\"apple_app_store\":\"com.flock.gems.ios\"}"), *Gems)),
			Item(TEXT("sword"), TEXT("standard"), FString::Printf(TEXT("{\"google_play\":\"%s\"}"), *Sword)),
			Item(TEXT("shield"), TEXT("standard"), TEXT("{}")),
			Item(TEXT("pass"), TEXT("subscription"), TEXT("{\"google_play\":\"com.flock.pass\"}")),
			Item(TEXT("twin-a"), TEXT("consumable"), FString::Printf(TEXT("{\"google_play\":\"%s\"}"), *Twin)),
			Item(TEXT("twin-b"), TEXT("consumable"), FString::Printf(TEXT("{\"google_play\":\"%s\"}"), *Twin)),
			Item(TEXT("gold-item"), TEXT("standard"), TEXT("{}"), /*bRealMoney*/ false),
		};
	}

	inline FString CatalogOf(const TArray<FString>& Items)
	{
		return FString::Printf(TEXT("{\"items\":[%s],\"total\":1,\"page\":1,\"limit\":100}"), *ShopOf(Items));
	}

	const FString Delivered =
		TEXT("{\"purchase_id\":\"pur-1\",\"item_type\":\"currency_pack\",\"status\":\"paid\",\"granted\":[{\"type\":\"currency\",\"code\":\"gold\",\"amount\":100}]}");

	const FString ReceiptInvalid =
		TEXT("{\"detail\":{\"code\":\"payment_provider.receipt_invalid\",\"message\":\"Receipt rejected: Google returned 400\"}}");

	const FString CredentialsRejected =
		TEXT("{\"detail\":{\"code\":\"payment_provider.store_credentials_rejected\",\"message\":\"Google rejected this game's store credentials (service account refused (HTTP 400)); the studio's payment settings need fixing before receipts can be verified\"}}");

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

	inline FFlockInitConfig Config(const FString& ApiKey = TEXT("secret"))
	{
		FFlockInitConfig Out;
		Out.ApiUrl = TEXT("https://api-flock.qwacks.com");
		Out.ApiKey = ApiKey;
		Out.GameId = TEXT("my-game");
		Out.GameVersion = TEXT("1.2.3");
		Out.GameVersionId = TEXT("ver-abc");
		return Out;
	}

	/** A whole SDK over a fake server that sells the catalog, with a store of the test's own. */
	/**
	 * Where a test keeps what the SDK answers. Flock ends every call still waiting as it shuts down, which the fixture's
	 * teardown does after the test's own locals are gone, so an answer kept in a local would be written into a dead frame
	 * (a heap the next test crashes on). This holds it in shared memory that outlives the frame.
	 */
	template <typename T>
	struct TAnswer
	{
		TSharedRef<TOptional<T>> Held = MakeShared<TOptional<T>>();

		bool IsSet() const { return Held->IsSet(); }
		const T& GetValue() const { return Held->GetValue(); }
		const T* operator->() const { return &Held->GetValue(); }
		void Set(const T& Value) const { *Held = Value; }

		/** The completion to hand the SDK: it writes into the shared answer, never into the test's frame. */
		TFunction<void(T)> Into() const
		{
			const TSharedRef<TOptional<T>> Answer = Held;
			return [Answer](T Value) { *Answer = MoveTemp(Value); };
		}
	};

	struct FFixture
	{
		FFlockTestSdk Test;
		UFlockSubsystem* Sdk = Test.Sdk;
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockMemoryTokenStore> Tokens = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFakeStore> Store = MakeShared<FFakeStore>();
		UFlockStore* Connected = nullptr;
		TArray<FString> Completed;
		TArray<FFlockUndeliveredPurchase> NotDelivered;

		explicit FFixture(const FFlockHttpResponse& Redemption = FFlockFakeTransport::Ok(Delivered), const TArray<FString>& Items = CatalogItems(),
			const FString& ApiKey = TEXT("secret"))
		{
			Sdk->SetHttpAdapterForTesting(Fake);
			Sdk->SetTokenStoreForTesting(Tokens);
			Serve(Items);
			Fake->On(TEXT("shop/validate-receipt"), Redemption);
			Sdk->InitializeWithConfig(Config(ApiKey));
		}

		/** Flock shuts down here, while the members its last answers and events write into are still alive. */
		~FFixture()
		{
			Sdk->ShutdownSdk();
		}

		/** The catalog read at connect, and the shop read just before the store's dialog. */
		void Serve(const TArray<FString>& Items)
		{
			Fake->On(TEXT("shop?page="), FFlockFakeTransport::Ok(CatalogOf(Items)));
			Fake->On(TEXT("shop/shop-1"), FFlockFakeTransport::Ok(ShopOf(Items)));
		}

		void SignIn(const FString& PlayerId)
		{
			Fake->On(TEXT("player/login/device"), FFlockFakeTransport::Ok(FString::Printf(
				TEXT("{\"player_id\":\"%s\",\"access_token\":\"%s\",\"refresh_token\":\"r-%s\"}"), *PlayerId, *Jwt(PlayerId), *PlayerId)));
			Sdk->GetAuthProvider()->LoginWithDevice(TEXT("device-") + PlayerId, nullptr);
		}

		bool Connect()
		{
			bool bConnected = false;
			UFlockStore::ConnectThrough(Sdk, Store, [this, &bConnected](TFlockResult<UFlockStore*> Result)
			{
				bConnected = Result.IsSuccess();
				Connected = Result.IsSuccess() ? Result.Value : nullptr;
			});
			if (Connected != nullptr)
			{
				Connected->OnPurchaseCompleted.AddLambda([this](const FFlockShopItem& Bought, const FFlockPurchaseResult& Result)
				{
					Completed.Add(Bought.Id + TEXT(":") + Result.PurchaseId);
				});
				Connected->OnPurchaseNotDelivered.AddLambda([this](const FFlockUndeliveredPurchase& Purchase) { NotDelivered.Add(Purchase); });
			}
			return bConnected;
		}

		TAnswer<TFlockResult<FFlockPurchaseResult>> Buy(const FString& ShopItemId)
		{
			TAnswer<TFlockResult<FFlockPurchaseResult>> Answer;
			Sdk->GetShopProvider()->Purchase(ShopItemId, Answer.Into());
			return Answer;
		}

		TArray<FString> RedemptionBodies() const
		{
			TArray<FString> Out;
			for (const FFlockHttpRequest& Request : Fake->Requests)
			{
				if (Request.Url.Contains(TEXT("shop/validate-receipt")))
				{
					Out.Add(Request.JsonBody);
				}
			}
			return Out;
		}
	};

	inline FString Field(const FString& Json, const FString& Path)
	{
		TSharedPtr<FJsonObject> Object;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object) || !Object.IsValid())
		{
			return FString();
		}
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("."));
		for (int32 Index = 0; Index + 1 < Parts.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Inner = nullptr;
			if (!Object->TryGetObjectField(Parts[Index], Inner))
			{
				return FString();
			}
			Object = *Inner;
		}
		FString Value;
		Object->TryGetStringField(Parts.Last(), Value);
		return Value;
	}
}

using namespace FlockStoreTesting;

FLOCK_STORE_TEST(FFlockStoreRegistersTest, "ConnectRegistersEachItemsProductForThisStoreWithItsKind")
bool FFlockStoreRegistersTest::RunTest(const FString& Parameters)
{
	FFixture F;
	TestTrue(TEXT("Connected"), F.Connect());
	TestTrue(TEXT("A currency pack is a consumable product."), F.Store->Calls.Contains(TEXT("register:") + Gems + TEXT(":consumable")));
	TestTrue(TEXT("A standard item is a one-time product."), F.Store->Calls.Contains(TEXT("register:") + Sword + TEXT(":one-time")));
	TestFalse(TEXT("A kind Flock's store does not sell is left out."), F.Store->AnyStartingWith(TEXT("register:com.flock.pass")));
	TestFalse(TEXT("Only this store's product ids."), F.Store->AnyStartingWith(TEXT("register:com.flock.gems.ios")));
	TestTrue(TEXT("The subsystem holds the connection"), F.Sdk->GetStore() == F.Connected);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreDeliveredTest, "ABuyDeliveredIsFinishedInTheStoreOnlyOnceFlockDelivered")
bool FFlockStoreDeliveredTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Store->OnBuy = [&F](const FString& ProductId) { F.Store->PayFromTheDialog(ProductId, TEXT("token-1")); };
	F.SignIn(TEXT("player-a"));
	F.Connect();
	F.Fake->bDeferred = true;
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer;
	F.Sdk->GetShopProvider()->Purchase(TEXT("gems"), Answer.Into());
	// The shop read before the dialog is answered; the store charges, and the redemption it sends is held.
	F.Fake->FlushPending();
	TestEqual(TEXT("Flock is asked"), F.RedemptionBodies().Num(), 1);
	TestFalse(TEXT("When Flock is asked, the store's purchase is still unfinished."), F.Store->AnyStartingWith(TEXT("finish:")));
	F.Fake->bDeferred = false;
	F.Fake->FlushPending();

	TestTrue(TEXT("Answered"), Answer.IsSet() && Answer->IsSuccess());
	TestTrue(TEXT("Paid"), Answer.IsSet() && Answer->Value.IsPaid());
	TestEqualSensitive(TEXT("The purchase"), Answer.IsSet() ? Answer->Value.PurchaseId : FString(), FString(TEXT("pur-1")));
	TestEqual(TEXT("Every delivery is reported, the one the call answers included."), F.Completed, TArray<FString>{ TEXT("gems:pur-1") });
	TestEqual(TEXT("Finished once Flock delivered."), F.Store->Count(TEXT("finish:token-1")), 1);
	const TArray<FString> Bodies = F.RedemptionBodies();
	TestEqual(TEXT("One redemption"), Bodies.Num(), 1);
	if (Bodies.Num() == 1)
	{
		TestEqualSensitive(TEXT("For the item"), Field(Bodies[0], TEXT("shop_item_id")), FString(TEXT("gems")));
		TestEqualSensitive(TEXT("From Google Play"), Field(Bodies[0], TEXT("provider")), FString(TEXT("google_play")));
		TestEqualSensitive(TEXT("Its product"), Field(Bodies[0], TEXT("receipt.product_id")), Gems);
		TestEqualSensitive(TEXT("Its token"), Field(Bodies[0], TEXT("receipt.purchase_token")), FString(TEXT("token-1")));
	}
	TestEqual(TEXT("The store's catalog names the item, so it is not read again."), F.Fake->CountTo(TEXT("shop_item/gems")), 0);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreRefusedTest, "AFlockRefusalStaysUnfinishedAndTheCallFailsWithTheCode")
bool FFlockStoreRefusedTest::RunTest(const FString& Parameters)
{
	// The SDK logs each refusal the server sends.
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F(FFlockFakeTransport::Status(422, CredentialsRejected));
	F.Store->OnBuy = [&F](const FString& ProductId) { F.Store->PayFromTheDialog(ProductId, TEXT("token-1")); };
	F.SignIn(TEXT("player-a"));
	F.Connect();

	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer = F.Buy(TEXT("gems"));

	TestTrue(TEXT("Failed"), Answer.IsSet() && !Answer->IsSuccess());
	TestTrue(TEXT("On its code"), Answer.IsSet() && Answer->Error.ErrorCode == EFlockErrorCode::PaymentProviderStoreCredentialsRejected);
	TestEqual(TEXT("Every paid purchase Flock did not deliver is reported, the one the call fails for included."), F.NotDelivered.Num(), 1);
	TestTrue(TEXT("With the code"), F.NotDelivered.Num() == 1 && F.NotDelivered[0].Error.ErrorCode == EFlockErrorCode::PaymentProviderStoreCredentialsRejected);
	TestFalse(TEXT("A refused purchase is never finished: the store hands it back."), F.Store->AnyStartingWith(TEXT("finish:")));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreEventsBeforeTheAnswerTest, "EachPurchasesEventIsRaisedBeforeTheCallIsAnswered")
bool FFlockStoreEventsBeforeTheAnswerTest::RunTest(const FString& Parameters)
{
	// The SDK logs the refusal the server sends.
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	for (const bool bDelivered : { true, false })
	{
		FFixture F(bDelivered ? FFlockFakeTransport::Ok(Delivered) : FFlockFakeTransport::Status(422, CredentialsRejected));
		F.Store->OnBuy = [&F](const FString& ProductId) { F.Store->PayFromTheDialog(ProductId, TEXT("token-1")); };
		F.SignIn(TEXT("player-a"));
		F.Connect();
		const TSharedRef<int32> EventsWhenAnswered = MakeShared<int32>(-1);
		F.Sdk->GetShopProvider()->Purchase(TEXT("gems"), [&F, EventsWhenAnswered](TFlockResult<FFlockPurchaseResult>)
		{
			*EventsWhenAnswered = F.Completed.Num() + F.NotDelivered.Num();
		});
		TestEqual(bDelivered ? TEXT("Delivered: reported before the call is answered") : TEXT("Refused: reported before the call is answered"),
			*EventsWhenAnswered, 1);
	}
	return true;
}

FLOCK_STORE_TEST(FFlockStoreCancelledTest, "APlayerCancelFailsStoreCancelledAndFlockIsNotAsked")
bool FFlockStoreCancelledTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Store->OnBuy = [&F](const FString& ProductId) { F.Store->Fail(ProductId, FlockStoreFailureReasons::Cancelled, TEXT("UserCancelled: closed the dialog")); };
	F.SignIn(TEXT("player-a"));
	F.Connect();

	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer = F.Buy(TEXT("gems"));

	TestTrue(TEXT("Failed as a store refusal"), Answer.IsSet() && Answer->Error.Type == EFlockErrorType::Store);
	TestEqualSensitive(TEXT("Cancelled"), Answer.IsSet() ? Answer->Error.StoreFailureReason : FString(), FString(FlockStoreFailureReasons::Cancelled));
	TestFalse(TEXT("With a fix"), !Answer.IsSet() || Answer->Error.Hint.IsEmpty());
	TestEqual(TEXT("Flock is not asked"), F.RedemptionBodies().Num(), 0);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreDeclinedTest, "AStoreDeclineFailsWithTheStoresReasonAndItsWords")
bool FFlockStoreDeclinedTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Store->OnBuy = [&F](const FString& ProductId) { F.Store->Fail(ProductId, FlockStoreFailureReasons::PaymentDeclined, TEXT("PaymentDeclined: card refused")); };
	F.SignIn(TEXT("player-a"));
	F.Connect();

	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer = F.Buy(TEXT("gems"));

	TestEqualSensitive(TEXT("The store's reason"), Answer.IsSet() ? Answer->Error.StoreFailureReason : FString(), FString(FlockStoreFailureReasons::PaymentDeclined));
	TestTrue(TEXT("And its words"), Answer.IsSet() && Answer->Error.Message.Contains(TEXT("PaymentDeclined: card refused")));
	TestTrue(TEXT("The fix names another payment method"), Answer.IsSet() && Answer->Error.Hint.Contains(TEXT("another payment method")));
	return true;
}

FLOCK_STORE_TEST(FFlockStorePendingTest, "AStoreWaitingForThePaymentAnswersPendingThenIsReportedCompleted")
bool FFlockStorePendingTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Store->OnBuy = [&F](const FString& ProductId) { F.Store->Defer(ProductId); };
	F.SignIn(TEXT("player-a"));
	F.Connect();

	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer = F.Buy(TEXT("gems"));
	TestTrue(TEXT("Answered, not failed"), Answer.IsSet() && Answer->IsSuccess());
	TestEqualSensitive(TEXT("Pending"), Answer.IsSet() ? Answer->Value.Status : FString(), FString(FlockPurchaseStatuses::Pending));
	TestEqual(TEXT("Nothing sent while pending"), F.RedemptionBodies().Num(), 0);

	F.Store->Pay(Gems, TEXT("token-late"));
	TestEqual(TEXT("Reported once paid"), F.Completed, TArray<FString>{ TEXT("gems:pur-1") });
	TestEqual(TEXT("And finished"), F.Store->Count(TEXT("finish:token-late")), 1);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreNotSignedInTest, "ABuyWithNobodySignedInFailsAndTheStoreIsNotAsked")
bool FFlockStoreNotSignedInTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Connect();
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer = F.Buy(TEXT("gems"));
	TestTrue(TEXT("Refused as a sign-in failure"), Answer.IsSet() && Answer->Error.Type == EFlockErrorType::Auth);
	TestFalse(TEXT("The store is not asked"), F.Store->AnyStartingWith(TEXT("buy:")));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreNotSoldHereTest, "AnItemThisStoreDoesNotSellIsRefusedAndTheStoreIsNotAsked")
bool FFlockStoreNotSoldHereTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	for (const TCHAR* ItemId : { TEXT("shield"), TEXT("pass") })
	{
		const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer = F.Buy(ItemId);
		TestTrue(FString::Printf(TEXT("%s refused"), ItemId), Answer.IsSet() && Answer->Error.Type == EFlockErrorType::Validation);
		TestTrue(FString::Printf(TEXT("%s: the fix names the dashboard's product ids"), ItemId), Answer.IsSet() && Answer->Error.Hint.Contains(TEXT("Store product IDs")));
	}
	TestFalse(TEXT("The store is not asked"), F.Store->AnyStartingWith(TEXT("buy:")));
	TestEqual(TEXT("Nor is anything bought from the wallet"), F.Fake->CountTo(TEXT("shop/transaction")), 0);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreHeldForSignInTest, "AnUnfinishedPurchaseBeforeASignInWaitsForItThenIsSentAndDelivered")
bool FFlockStoreHeldForSignInTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Connect();
	F.Store->Pay(Gems, TEXT("token-1"));
	TestEqual(TEXT("Nothing sent signed out"), F.RedemptionBodies().Num(), 0);
	TestFalse(TEXT("Nor finished"), F.Store->AnyStartingWith(TEXT("finish:")));

	F.SignIn(TEXT("player-a"));

	TestEqual(TEXT("Sent once signed in"), F.RedemptionBodies().Num(), 1);
	TestEqual(TEXT("Delivered and reported"), F.Completed, TArray<FString>{ TEXT("gems:pur-1") });
	TestEqual(TEXT("Then finished"), F.Store->Count(TEXT("finish:token-1")), 1);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreEachSignInFetchesTest, "EachSignInAsksTheStoreForUnfinishedPurchases")
bool FFlockStoreEachSignInFetchesTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Connect();
	TestEqual(TEXT("Signed out: nothing asked"), F.Store->Count(TEXT("fetch")), 0);
	F.SignIn(TEXT("player-a"));
	TestEqual(TEXT("Asked at the sign-in"), F.Store->Count(TEXT("fetch")), 1);
	F.Sdk->Logout();
	F.SignIn(TEXT("player-b"));
	TestEqual(TEXT("And at the next one"), F.Store->Count(TEXT("fetch")), 2);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreConnectedSignedInFetchesTest, "ConnectingWhileSignedInAsksTheStoreForUnfinishedPurchases")
bool FFlockStoreConnectedSignedInFetchesTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	TestEqual(TEXT("Asked once connected"), F.Store->Count(TEXT("fetch")), 1);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreOnceALaunchTest, "AnUnfinishedPurchaseIsSentOnceALaunchAndARefusalIsReported")
bool FFlockStoreOnceALaunchTest::RunTest(const FString& Parameters)
{
	// The SDK logs each refusal the server sends.
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F(FFlockFakeTransport::Status(422, CredentialsRejected));
	F.SignIn(TEXT("player-a"));
	F.Connect();

	F.Store->Pay(Gems, TEXT("token-1"));
	F.Store->Pay(Gems, TEXT("token-1"));

	TestEqual(TEXT("Sent once this launch"), F.RedemptionBodies().Num(), 1);
	TestEqual(TEXT("The refusal is reported once"), F.NotDelivered.Num(), 1);
	TestTrue(TEXT("With its item"), F.NotDelivered.Num() == 1 && F.NotDelivered[0].bHasItem && F.NotDelivered[0].Item.Id == TEXT("gems"));
	TestFalse(TEXT("Never finished"), F.Store->AnyStartingWith(TEXT("finish:")));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreGamesOwnProductTest, "AnOrderForAProductNoFlockItemSellsIsLeftToTheGame")
bool FFlockStoreGamesOwnProductTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	F.Store->Pay(TEXT("com.game.own"), TEXT("token-own"));
	TestEqual(TEXT("Not sent"), F.RedemptionBodies().Num(), 0);
	TestFalse(TEXT("Not finished: the game finishes its own"), F.Store->AnyStartingWith(TEXT("finish:")));
	TestEqual(TEXT("Not reported"), F.NotDelivered.Num(), 0);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreSoldTwiceTest, "AnUnfinishedPurchaseOfAProductTwoItemsSellIsReportedAndNotSent")
bool FFlockStoreSoldTwiceTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	F.Store->Pay(Twin, TEXT("token-twin"));
	TestEqual(TEXT("Not sent"), F.RedemptionBodies().Num(), 0);
	TestEqual(TEXT("Reported"), F.NotDelivered.Num(), 1);
	TestTrue(TEXT("Without an item"), F.NotDelivered.Num() == 1 && !F.NotDelivered[0].bHasItem);
	TestTrue(TEXT("Its fix: one product id each"), F.NotDelivered.Num() == 1 && F.NotDelivered[0].Error.Hint.Contains(TEXT("its own store product id")));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreBuyTwinTest, "ABuyOfAProductTwoItemsSellIsSentForTheItemTheCallNames")
bool FFlockStoreBuyTwinTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Store->OnBuy = [&F](const FString& ProductId) { F.Store->PayFromTheDialog(ProductId, TEXT("token-twin")); };
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer = F.Buy(TEXT("twin-b"));
	TestTrue(TEXT("Delivered"), Answer.IsSet() && Answer->IsSuccess());
	const TArray<FString> Bodies = F.RedemptionBodies();
	TestEqualSensitive(TEXT("For the item the call names"), Bodies.Num() == 1 ? Field(Bodies[0], TEXT("shop_item_id")) : FString(), FString(TEXT("twin-b")));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreHandedBackWhileConnectingTest, "APurchaseHandedBackWhileConnectingIsSentOnceConnectedAndSignedIn")
bool FFlockStoreHandedBackWhileConnectingTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Store->bConnectWaits = true;
	F.Connect();
	F.Store->Pay(Gems, TEXT("token-early"));
	TestEqual(TEXT("Held while connecting"), F.RedemptionBodies().Num(), 0);
	F.Store->bConnectWaits = false;
	F.Store->PendingConnect(FFlockError());
	TestEqual(TEXT("Sent once connected"), F.RedemptionBodies().Num(), 1);
	TestEqual(TEXT("And finished"), F.Store->Count(TEXT("finish:token-early")), 1);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreTwoBuysTest, "TwoBuysOfOneProductAtOnceTheSecondIsRefused")
bool FFlockStoreTwoBuysTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockPurchaseResult>> First = F.Buy(TEXT("gems"));
	TestFalse(TEXT("The first waits for the store"), First.IsSet());
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Second = F.Buy(TEXT("gems"));
	TestTrue(TEXT("The second is refused"), Second.IsSet() && Second->Error.Type == EFlockErrorType::Validation);
	TestEqual(TEXT("The store's dialog opened once"), F.Store->Count(TEXT("buy:") + Gems), 1);
	F.Store->Pay(Gems, TEXT("token-1"));
	TestEqual(TEXT("The first still ends with its delivery"), F.Completed, TArray<FString>{ TEXT("gems:pur-1") });
	return true;
}

FLOCK_STORE_TEST(FFlockStoreOnePurchaseAtATimeTest, "OneStorePurchaseAtATimeASecondProductIsRefusedWhileTheFirstIsUnderWay")
bool FFlockStoreOnePurchaseAtATimeTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockPurchaseResult>> First = F.Buy(TEXT("gems"));
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Second = F.Buy(TEXT("sword"));
	TestFalse(TEXT("The first waits for the store"), First.IsSet());
	TestTrue(TEXT("Another product is refused meanwhile"), Second.IsSet() && Second->Error.Type == EFlockErrorType::Validation);
	TestEqual(TEXT("One dialog opened"), F.Store->Calls.FilterByPredicate([](const FString& C) { return C.StartsWith(TEXT("buy:")); }).Num(), 1);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreHandedBackDuringTheDialogTest, "APurchaseHandedBackWhileTheDialogIsOpenDoesNotAnswerTheCall")
bool FFlockStoreHandedBackDuringTheDialogTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer;
	F.Sdk->GetShopProvider()->Purchase(TEXT("gems"), Answer.Into());
	// An older unfinished purchase of the same product, handed back while the dialog is open.
	F.Store->Pay(Gems, TEXT("token-old"));
	TestEqual(TEXT("It is delivered and reported"), F.Completed, TArray<FString>{ TEXT("gems:pur-1") });
	TestFalse(TEXT("But the call still waits for its own dialog"), Answer.IsSet());
	F.Store->Fail(Gems, FlockStoreFailureReasons::Cancelled, TEXT("UserCancelled: closed the dialog"));
	TestEqualSensitive(TEXT("And ends with the dialog's own answer"), Answer.IsSet() ? Answer->Error.StoreFailureReason : FString(),
		FString(FlockStoreFailureReasons::Cancelled));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreDialogForTheBuyerTest, "ADialogPaidAfterItsCallEndedWaitsForThePlayerWhoOpenedIt")
bool FFlockStoreDialogForTheBuyerTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer;
	F.Sdk->GetShopProvider()->Purchase(TEXT("gems"), Answer.Into());
	F.Sdk->Logout();
	TestTrue(TEXT("The call ended with the sign-in"), Answer.IsSet() && Answer->Error.Type == EFlockErrorType::Cancelled);
	F.SignIn(TEXT("player-b"));
	const TAnswer<TFlockResult<FFlockPurchaseResult>> WhileOpen = F.Buy(TEXT("sword"));
	TestTrue(TEXT("Signed in, a new purchase is still refused while that dialog is open"), WhileOpen.IsSet() && WhileOpen->Error.Type == EFlockErrorType::Validation);
	TestEqual(TEXT("And no second dialog opened"), F.Store->Calls.FilterByPredicate([](const FString& C) { return C.StartsWith(TEXT("buy:")); }).Num(), 1);
	F.Store->PayFromTheDialog(Gems, TEXT("token-a"));
	TestEqual(TEXT("Not sent as another player"), F.RedemptionBodies().Num(), 0);
	F.Sdk->Logout();
	F.SignIn(TEXT("player-a"));
	TestEqual(TEXT("Sent when the player who bought it signs in again"), F.RedemptionBodies().Num(), 1);
	TestEqual(TEXT("And delivered"), F.Completed, TArray<FString>{ TEXT("gems:pur-1") });
	return true;
}

FLOCK_STORE_TEST(FFlockStoreConnectAgainTest, "AConnectionMadeAgainDoesNotSendAPurchaseAlreadySentThisRun")
bool FFlockStoreConnectAgainTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F(FFlockFakeTransport::Status(422, CredentialsRejected));
	F.SignIn(TEXT("player-a"));
	F.Connect();
	F.Store->Pay(Gems, TEXT("token-1"));
	F.Connected->Disconnect();
	F.Store = MakeShared<FFakeStore>();
	F.Connect();
	F.Store->Pay(Gems, TEXT("token-1"));
	TestEqual(TEXT("Sent once this run of the game"), F.RedemptionBodies().Num(), 1);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreCancelledSendTest, "APurchaseWhoseSendEndedWithItsSignInIsSentAtTheNextSignIn")
bool FFlockStoreCancelledSendTest::RunTest(const FString& Parameters)
{
	// The SDK logs the refused renewal and the redemption it ended.
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("refresh"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F;
	// The first answer says the token expired, and the renewal is refused: the sign-in is over, and nothing was redeemed.
	F.Fake->OnSequence(TEXT("shop/validate-receipt"), {
		FFlockFakeTransport::Status(401, TEXT("{\"detail\":{\"code\":\"player.expired_token\",\"message\":\"Expired token.\"}}")),
		FFlockFakeTransport::Ok(Delivered) });
	F.SignIn(TEXT("player-a"));
	F.Connect();
	F.Fake->On(TEXT("player/token/refresh"), FFlockFakeTransport::Coded(401, TEXT("player.invalid_refresh_token")));
	F.Store->Pay(Gems, TEXT("token-1"));
	TestEqual(TEXT("The first attempt went out"), F.RedemptionBodies().Num(), 1);
	TestEqual(TEXT("Nothing reported: it never reached the redemption"), F.NotDelivered.Num(), 0);
	TestFalse(TEXT("Not finished"), F.Store->AnyStartingWith(TEXT("finish:")));
	F.SignIn(TEXT("player-a"));
	// The store hands it back at the sign-in's fetch.
	F.Store->Pay(Gems, TEXT("token-1"));
	TestEqual(TEXT("Sent again"), F.RedemptionBodies().Num(), 2);
	TestEqual(TEXT("Delivered once"), F.Completed, TArray<FString>{ TEXT("gems:pur-1") });
	return true;
}

FLOCK_STORE_TEST(FFlockStoreOneRestoreTest, "OneRestoreAtATimeAndADisconnectEndsTheOneUnderWay")
bool FFlockStoreOneRestoreTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Store->Own(Sword, TEXT("token-sword"));
	F.SignIn(TEXT("player-a"));
	F.Connect();
	F.Fake->bDeferred = true;
	const TAnswer<TFlockResult<FFlockStoreRestore>> First;
	F.Connected->Restore(First.Into());
	const TAnswer<TFlockResult<FFlockStoreRestore>> Second;
	F.Connected->Restore(Second.Into());
	TestTrue(TEXT("A second restore is refused"), Second.IsSet() && Second->Error.Type == EFlockErrorType::Validation);
	F.Connected->Disconnect();
	TestTrue(TEXT("The one under way ends Cancelled"), First.IsSet() && First->Error.Type == EFlockErrorType::Cancelled);
	F.Fake->bDeferred = false;
	F.Fake->FlushPending();
	TestTrue(TEXT("Answered once"), First.IsSet());
	return true;
}

FLOCK_STORE_TEST(FFlockStoreSetupAcceptsMadeUpTest, "TheSetupCheckReportsAServerThatAcceptsAMadeUpPurchase")
bool FFlockStoreSetupAcceptsMadeUpTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockStoreSetupCheck>> Answer;
	F.Connected->CheckSetup(Answer.Into());
	TestTrue(TEXT("A server problem"), Answer.IsSet() && Answer->Value.bHasServerProblem && !Answer->Value.ServerProblem.Hint.IsEmpty());
	TestFalse(TEXT("Not one that checks purchases"), Answer.IsSet() && Answer->Value.bServerCanCheckPurchases);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreSoldBeforeNoCacheTest, "ProductsSoldBeforeAreKeptWithTheOfflineCacheOff")
bool FFlockStoreSoldBeforeNoCacheTest::RunTest(const FString& Parameters)
{
	UFlockConfig* Settings = GetMutableDefault<UFlockConfig>();
	const bool bWas = Settings->bEnableOfflineCache;
	Settings->bEnableOfflineCache = false;
	ON_SCOPE_EXIT { Settings->bEnableOfflineCache = bWas; };
	FFixture F;
	F.Connect();
	F.Sdk->ShutdownSdk();
	TArray<FString> Rest = CatalogItems();
	Rest.RemoveAt(0);
	F.Serve(Rest);
	F.Sdk->InitializeWithConfig(Config());
	F.Store = MakeShared<FFakeStore>();
	F.Connect();
	TestTrue(TEXT("The product sold before is still registered"), F.Store->Calls.Contains(TEXT("register:") + Gems + TEXT(":consumable")));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreAnotherPlayersReceiptTest, "AReceiptAnotherPlayerHoldsIsReportedAndNotSentAgainThisRun")
bool FFlockStoreAnotherPlayersReceiptTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F(FFlockFakeTransport::Coded(403, TEXT("payment_provider.receipt_belongs_to_another_player")));
	F.SignIn(TEXT("player-a"));
	F.Connect();
	F.Store->Pay(Gems, TEXT("token-theirs"));
	TestEqual(TEXT("Reported, on its code"), F.NotDelivered.Num(), 1);
	TestTrue(TEXT("The server's own code"), F.NotDelivered.Num() == 1
		&& F.NotDelivered[0].Error.ErrorCode == EFlockErrorCode::PaymentProviderReceiptBelongsToAnotherPlayer);
	F.Sdk->Logout();
	F.SignIn(TEXT("player-a"));
	F.Store->Pay(Gems, TEXT("token-theirs"));
	TestEqual(TEXT("Not sent again this run"), F.RedemptionBodies().Num(), 1);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreBuyerHoldFetchedAgainTest, "ADialogPaidForAnotherPlayerIsNotSentWhenTheStoreHandsItBackAgain")
bool FFlockStoreBuyerHoldFetchedAgainTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	F.Sdk->GetShopProvider()->Purchase(TEXT("gems"), [](TFlockResult<FFlockPurchaseResult>) {});
	F.Sdk->Logout();
	F.SignIn(TEXT("player-b"));
	F.Store->PayFromTheDialog(Gems, TEXT("token-a"));
	// The next fetch, or the store's notice of a payment, hands the same purchase back, not as the dialog's answer.
	F.Store->Pay(Gems, TEXT("token-a"));
	TestEqual(TEXT("Not sent as player b"), F.RedemptionBodies().Num(), 0);
	F.Sdk->Logout();
	F.SignIn(TEXT("player-a"));
	TestEqual(TEXT("Sent once player a signs in"), F.RedemptionBodies().Num(), 1);
	F.Store->Pay(Gems, TEXT("token-a"));
	TestEqual(TEXT("And only once"), F.RedemptionBodies().Num(), 1);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreBuyAgainFromTheEventTest, "AGameCanBuyAgainFromInsideThePurchasesEvent")
bool FFlockStoreBuyAgainFromTheEventTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Store->OnBuy = [&F](const FString& ProductId)
	{
		if (ProductId == Gems)
		{
			F.Store->PayFromTheDialog(ProductId, TEXT("token-1"));
		}
	};
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Again;
	F.Connected->OnPurchaseCompleted.AddLambda([&F, Again](const FFlockShopItem& Item, const FFlockPurchaseResult&)
	{
		if (Item.Id == TEXT("gems") && !Again.IsSet())
		{
			F.Sdk->GetShopProvider()->Purchase(TEXT("sword"), Again.Into());
			if (!Again.IsSet())
			{
				Again.Set(TFlockResult<FFlockPurchaseResult>::Ok(FFlockPurchaseResult()));
			}
		}
	});
	F.Buy(TEXT("gems"));
	TestTrue(TEXT("The second purchase started"), F.Store->Calls.Contains(TEXT("buy:") + Sword));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreOwnIdNamedTest, "ARealMoneyItemBoughtNamingTheSignedInPlayerGoesThroughTheStore")
bool FFlockStoreOwnIdNamedTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Fake->On(TEXT("shop_item/gems"), FFlockFakeTransport::Ok(CatalogItems()[0]));
	F.SignIn(TEXT("player-a"));
	F.Connect();
	F.Sdk->GetShopProvider()->Purchase(TEXT("gems"), TEXT("player-a"), [](TFlockResult<FFlockPurchaseResult>) {});
	TestTrue(TEXT("Through the store"), F.Store->Calls.Contains(TEXT("buy:") + Gems));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreConnectFailsTest, "AConnectThatFailsStopsListeningAndLeavesNothingConnected")
bool FFlockStoreConnectFailsTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Store->bConnectFails = true;
	TestFalse(TEXT("Not connected"), F.Connect());
	TestEqual(TEXT("Stopped listening"), F.Store->Count(TEXT("stop")), 1);
	TestNull(TEXT("Nothing held"), F.Sdk->GetStore());
	F.Store->bConnectFails = false;
	TestTrue(TEXT("A later connect works"), F.Connect());
	return true;
}

FLOCK_STORE_TEST(FFlockStoreSecondConnectTest, "ASecondConnectWhileTheFirstIsConnectedIsRefusedUntilTheFirstIsDisconnected")
bool FFlockStoreSecondConnectTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Connect();
	UFlockStore* First = F.Connected;
	TOptional<FFlockError> Refused;
	UFlockStore::ConnectThrough(F.Sdk, MakeShared<FFakeStore>(), [&Refused](TFlockResult<UFlockStore*> Result) { Refused = Result.Error; });
	TestTrue(TEXT("Refused"), Refused.IsSet() && Refused->Type == EFlockErrorType::Validation);
	TestTrue(TEXT("The first stays"), F.Sdk->GetStore() == First);
	First->Disconnect();
	TestNull(TEXT("Disconnected"), F.Sdk->GetStore());
	TestTrue(TEXT("Connects again"), F.Connect());
	return true;
}

FLOCK_STORE_TEST(FFlockStoreWalletItemTest, "AWalletItemWithTheStoreConnectedIsBoughtFromTheWalletAndTheStoreIsNotAsked")
bool FFlockStoreWalletItemTest::RunTest(const FString& Parameters)
{
	// The canonical suite's catalog and answer, verbatim: the gems and a potion priced in Gold.
	FFixture F(FFlockFakeTransport::Ok(Delivered), {
		Item(TEXT("gems"), TEXT("currency_pack"), FString::Printf(TEXT("{\"google_play\":\"%s\"}"), *Gems)),
		TEXT("{\"id\":\"potion\",\"name\":\"potion\",\"type\":\"consumable\",\"price\":20,\"currency\":\"Gold\",\"is_real_money\":false,\"store_skus\":{}}") });
	F.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Ok(TEXT("{\"purchase_id\":\"pur-wallet\",\"item_type\":\"consumable\"}")));
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer = F.Buy(TEXT("potion"));
	TestTrue(TEXT("Bought"), Answer.IsSet() && Answer->IsSuccess() && Answer->Value.PurchaseId == TEXT("pur-wallet"));
	TestEqual(TEXT("From the wallet"), F.Fake->CountTo(TEXT("shop/transaction")), 1);
	TestFalse(TEXT("The store is never asked about a wallet item"), F.Store->AnyStartingWith(TEXT("buy:")));
	TestEqual(TEXT("No receipt is redeemed"), F.RedemptionBodies().Num(), 0);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreNoStoreTest, "ARealMoneyItemWithNoStoreConnectedIsRefusedBeforeAnythingIsSent")
bool FFlockStoreNoStoreTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Fake->On(TEXT("shop_item/gems"), FFlockFakeTransport::Ok(CatalogItems()[0]));
	F.SignIn(TEXT("player-a"));
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer = F.Buy(TEXT("gems"));
	TestTrue(TEXT("Refused"), Answer.IsSet() && Answer->Error.Type == EFlockErrorType::Validation);
	TestTrue(TEXT("The fix says to connect the store"), Answer.IsSet() && Answer->Error.Hint.Contains(TEXT("UFlockStore::Connect")));
	TestEqual(TEXT("Nothing bought from the wallet"), F.Fake->CountTo(TEXT("shop/transaction")), 0);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreAnotherPlayerNamedTest, "ARealMoneyItemBoughtForAnotherNamedPlayerIsRefusedBeforeAnythingIsSent")
bool FFlockStoreAnotherPlayerNamedTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Fake->On(TEXT("shop_item/gems"), FFlockFakeTransport::Ok(CatalogItems()[0]));
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer;
	F.Sdk->GetShopProvider()->Purchase(TEXT("gems"), TEXT("player-b"), Answer.Into());
	TestTrue(TEXT("Refused"), Answer.IsSet() && Answer->Error.Type == EFlockErrorType::Validation);
	TestFalse(TEXT("The store is not asked"), F.Store->AnyStartingWith(TEXT("buy:")));
	TestEqual(TEXT("Nothing bought from the wallet"), F.Fake->CountTo(TEXT("shop/transaction")), 0);
	return true;
}

FLOCK_STORE_TEST(FFlockStorePriceTest, "TheStorePriceIsTheStoresOwnOrNotFound")
bool FFlockStorePriceTest::RunTest(const FString& Parameters)
{
	FFixture F;
	FFlockStorePrice Price;
	Price.bFoundInTheStore = true;
	Price.Text = TEXT("SAR 3.99");
	Price.AmountInSmallestUnit = 399;
	Price.CurrencyCode = TEXT("SAR");
	F.Store->Prices.Add(Gems, Price);
	F.Connect();
	const TFlockResult<FFlockStorePrice> Found = F.Connected->GetPrice(TEXT("gems"));
	TestTrue(TEXT("Found"), Found.IsSuccess() && Found.Value.bFoundInTheStore);
	TestEqualSensitive(TEXT("The store's text"), Found.Value.Text, FString(TEXT("SAR 3.99")));
	TestEqual(TEXT("The store's amount"), Found.Value.AmountInSmallestUnit, static_cast<int64>(399));
	const TFlockResult<FFlockStorePrice> Missing = F.Connected->GetPrice(TEXT("sword"));
	TestTrue(TEXT("Not found in the store"), Missing.IsSuccess() && !Missing.Value.bFoundInTheStore);
	TestFalse(TEXT("An item with no product id is refused"), F.Connected->GetPrice(TEXT("shield")).IsSuccess());
	TestFalse(TEXT("An unknown item is refused"), F.Connected->GetPrice(TEXT("nothing")).IsSuccess());
	return true;
}

FLOCK_STORE_TEST(FFlockStoreRestoreTest, "RestoreHasFlockDeliverEachOwnedPurchaseOfItsItemsAndFinishesNothing")
bool FFlockStoreRestoreTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Store->Own(Sword, TEXT("token-sword"));
	F.Store->Own(TEXT("com.game.own"), TEXT("token-own"));
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockStoreRestore>> Answer;
	F.Connected->Restore(Answer.Into());
	TestTrue(TEXT("Restored"), Answer.IsSet() && Answer->IsSuccess());
	TestTrue(TEXT("The sword"), Answer.IsSet() && Answer->Value.Restored.Num() == 1 && Answer->Value.Restored[0].Id == TEXT("sword"));
	TestEqual(TEXT("Only Flock's item is sent"), F.RedemptionBodies().Num(), 1);
	TestFalse(TEXT("Nothing finished: owned purchases are"), F.Store->AnyStartingWith(TEXT("finish:")));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreRestoreRefusalTest, "RestoreListsARefusalAndStillDeliversTheRest")
bool FFlockStoreRestoreRefusalTest::RunTest(const FString& Parameters)
{
	// The SDK logs each refusal the server sends.
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F;
	F.Fake->OnSequence(TEXT("shop/validate-receipt"), { FFlockFakeTransport::Coded(403, TEXT("payment_provider.receipt_belongs_to_another_player")), FFlockFakeTransport::Ok(Delivered) });
	F.Store->Own(Sword, TEXT("token-sword"));
	F.Store->Own(Gems, TEXT("token-gems"));
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockStoreRestore>> Answer;
	F.Connected->Restore(Answer.Into());
	TestTrue(TEXT("Restored"), Answer.IsSet() && Answer->IsSuccess());
	TestTrue(TEXT("The refusal is listed"), Answer.IsSet() && Answer->Value.NotDelivered.Num() == 1
		&& Answer->Value.NotDelivered[0].Error.ErrorCode == EFlockErrorCode::PaymentProviderReceiptBelongsToAnotherPlayer);
	TestTrue(TEXT("The rest delivered"), Answer.IsSet() && Answer->Value.Restored.Num() == 1);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreRestoreSendsAgainTest, "RestoreSendsAgainAnUnfinishedPurchaseWhoseSendFailedThisLaunch")
bool FFlockStoreRestoreSendsAgainTest::RunTest(const FString& Parameters)
{
	// The SDK logs each refusal the server sends.
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F;
	F.Fake->OnSequence(TEXT("shop/validate-receipt"), { FFlockFakeTransport::Status(422, CredentialsRejected), FFlockFakeTransport::Ok(Delivered) });
	F.SignIn(TEXT("player-a"));
	F.Connect();
	F.Store->Pay(Gems, TEXT("token-1"));
	TestEqual(TEXT("The first send refused"), F.NotDelivered.Num(), 1);
	F.Store->Unfinished.Add({ Gems, TEXT("token-1") });
	const TAnswer<TFlockResult<FFlockStoreRestore>> Answer;
	F.Connected->Restore(Answer.Into());
	TestEqual(TEXT("Sent again by the restore"), F.RedemptionBodies().Num(), 2);
	TestEqual(TEXT("Delivered and finished once"), F.Store->Count(TEXT("finish:token-1")), 1);
	TestEqual(TEXT("And reported"), F.Completed, TArray<FString>{ TEXT("gems:pur-1") });
	return true;
}

FLOCK_STORE_TEST(FFlockStoreRestoreSignInChangedTest, "RestoreStopsWhenAnotherPlayerSignsInAndSendsNoneOfTheRest")
bool FFlockStoreRestoreSignInChangedTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Store->Own(Sword, TEXT("token-sword"));
	F.Store->Own(Gems, TEXT("token-gems"));
	F.SignIn(TEXT("player-a"));
	F.Connect();
	F.Fake->bDeferred = true;
	const TAnswer<TFlockResult<FFlockStoreRestore>> Answer;
	F.Connected->Restore(Answer.Into());
	TestEqual(TEXT("The first is sent"), F.RedemptionBodies().Num(), 1);
	F.Fake->bDeferred = false;
	F.Sdk->Logout();
	F.SignIn(TEXT("player-b"));
	F.Fake->FlushPending();
	TestTrue(TEXT("Ended Cancelled"), Answer.IsSet() && Answer->Error.Type == EFlockErrorType::Cancelled);
	TestEqual(TEXT("None of the rest sent as the next player"), F.RedemptionBodies().Num(), 1);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreRestoreLeavesASendTest, "RestoreLeavesAPurchaseStillBeingSentToItsSendAndItIsSentOnce")
bool FFlockStoreRestoreLeavesASendTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	F.Fake->bDeferred = true;
	F.Store->Pay(Sword, TEXT("token-1"));
	F.Store->Unfinished.Add({ Sword, TEXT("token-1") });
	F.Store->Own(Sword, TEXT("token-1"));
	F.Connected->Restore([](TFlockResult<FFlockStoreRestore>) {});
	F.Fake->bDeferred = false;
	F.Fake->FlushPending();
	TestEqual(TEXT("A send under way is not started a second time."), F.RedemptionBodies().Num(), 1);
	TestEqual(TEXT("Finished once, by the send under way"), F.Store->Count(TEXT("finish:token-1")), 1);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreRestoreSoldBeforeTest, "RestoreDeliversAnOwnedPurchaseOfAnItemTheShopsNoLongerList")
bool FFlockStoreRestoreSoldBeforeTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Connect();
	F.Sdk->ShutdownSdk();
	TArray<FString> Rest = CatalogItems();
	Rest.RemoveAt(1);
	F.Serve(Rest);
	F.Fake->On(TEXT("shop_item/sword"), FFlockFakeTransport::Ok(CatalogItems()[1]));
	F.Sdk->InitializeWithConfig(Config());
	F.Store = MakeShared<FFakeStore>();
	F.Store->Own(Sword, TEXT("token-owned"));
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockStoreRestore>> Answer;
	F.Connected->Restore(Answer.Into());
	TestTrue(TEXT("Restored"), Answer.IsSet() && Answer->IsSuccess() && Answer->Value.Restored.Num() == 1 && Answer->Value.Restored[0].Id == TEXT("sword"));
	const TArray<FString> Bodies = F.RedemptionBodies();
	TestEqualSensitive(TEXT("For the item it was sold for"), Bodies.Num() == 1 ? Field(Bodies[0], TEXT("shop_item_id")) : FString(), FString(TEXT("sword")));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreAnotherPlayerWhileWaitingTest, "AnotherPlayerSigningInWhileTheStoreIsWaitedForEndsTheCall")
bool FFlockStoreAnotherPlayerWhileWaitingTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer;
	F.Sdk->GetShopProvider()->Purchase(TEXT("gems"), Answer.Into());
	TestFalse(TEXT("Waiting"), Answer.IsSet());
	// Another player signs in with no sign-out between, as a second login does.
	F.SignIn(TEXT("player-b"));
	TestTrue(TEXT("Ended Cancelled"), Answer.IsSet() && Answer->Error.Type == EFlockErrorType::Cancelled);
	return true;
}

FLOCK_STORE_TEST(FFlockStorePaidAfterShutdownTest, "APurchasePaidAfterFlockShutDownIsNotSent")
bool FFlockStorePaidAfterShutdownTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	TSharedRef<FFakeStore> Store = F.Store;
	IFlockStoreConnection::FListener Kept = Store->Listener;
	F.Sdk->ShutdownSdk();
	// A store that hands the purchase over even so: nothing is sent, and nothing is finished.
	if (Kept.OnPaid)
	{
		Kept.OnPaid({ Gems, TEXT("token-late") });
	}
	TestEqual(TEXT("Nothing sent"), F.RedemptionBodies().Num(), 0);
	TestFalse(TEXT("Nothing finished"), Store->AnyStartingWith(TEXT("finish:")));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreRestoreSignedOutTest, "RestoreWithNobodySignedInFailsAndTheStoreIsNotAsked")
bool FFlockStoreRestoreSignedOutTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Connect();
	const TAnswer<TFlockResult<FFlockStoreRestore>> Answer;
	F.Connected->Restore(Answer.Into());
	TestTrue(TEXT("Refused"), Answer.IsSet() && Answer->Error.Type == EFlockErrorType::Auth);
	TestEqual(TEXT("The store is not asked"), F.Store->Count(TEXT("restore")), 0);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreRestoreFailsTest, "RestoreFailsWhenTheStoreCannotListThePurchases")
bool FFlockStoreRestoreFailsTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Store->bRestoreFails = true;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockStoreRestore>> Answer;
	F.Connected->Restore(Answer.Into());
	TestTrue(TEXT("Failed"), Answer.IsSet() && !Answer->IsSuccess());
	TestEqual(TEXT("Nothing sent"), F.RedemptionBodies().Num(), 0);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreSetupCheckTest, "TheSetupCheckListsWhatTheStoreDidNotFindAndItemsWithoutAProductId")
bool FFlockStoreSetupCheckTest::RunTest(const FString& Parameters)
{
	// The made-up receipt's refusal is logged at debug: no error to expect (APassingSetupCheckLogsItsMadeUpReceiptsRefusalAtDebug...).
	FFixture F(FFlockFakeTransport::Status(422, ReceiptInvalid));
	FFlockStorePrice Found;
	Found.bFoundInTheStore = true;
	F.Store->Prices.Add(Gems, Found);
	F.Store->Prices.Add(Twin, Found);
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockStoreSetupCheck>> Answer;
	F.Connected->CheckSetup(Answer.Into());
	TestTrue(TEXT("Checked"), Answer.IsSet() && Answer->IsSuccess());
	if (!Answer.IsSet() || !Answer->IsSuccess())
	{
		return false;
	}
	const FFlockStoreSetupCheck& Check = Answer->Value;
	TestTrue(TEXT("The sword was not found"), Check.NotFoundInTheStore.Num() == 1 && Check.NotFoundInTheStore[0].Id == TEXT("sword"));
	TestTrue(TEXT("The shield has no product id; a kind not sold through the store (the pass) is not listed"),
		Check.MissingProductId.Num() == 1 && Check.MissingProductId[0].Id == TEXT("shield"));
	TestTrue(TEXT("A receipt the store calls invalid means the keys work"), Check.bServerCanCheckPurchases);
	TestFalse(TEXT("No server problem"), Check.bHasServerProblem);
	TestFalse(TEXT("Not ready"), Check.IsReady());
	const TArray<FString> Asked = F.RedemptionBodies();
	TestTrue(TEXT("Only a made-up purchase is sent"), Asked.Num() == 1 && Asked[0].Contains(TEXT("\"purchase_token\":\"flock-setup-check\""), ESearchCase::CaseSensitive));
	TestFalse(TEXT("Nothing is bought"), F.Store->AnyStartingWith(TEXT("buy:")));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreSetupCheckKeysTest, "TheSetupCheckReportsTheStoreRefusingFlocksKeysAsTheServerProblem")
bool FFlockStoreSetupCheckKeysTest::RunTest(const FString& Parameters)
{
	// The SDK logs each refusal the server sends.
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F(FFlockFakeTransport::Status(422, CredentialsRejected));
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockStoreSetupCheck>> Answer;
	F.Connected->CheckSetup(Answer.Into());
	TestTrue(TEXT("The server problem"), Answer.IsSet() && Answer->Value.bHasServerProblem
		&& Answer->Value.ServerProblem.ErrorCode == EFlockErrorCode::PaymentProviderStoreCredentialsRejected);
	TestFalse(TEXT("The server cannot check"), Answer.IsSet() && Answer->Value.bServerCanCheckPurchases);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreSetupReadyTest, "TheSetupCheckWithEveryProductFoundAndTheServerAbleToCheckIsReady")
bool FFlockStoreSetupReadyTest::RunTest(const FString& Parameters)
{
	// The made-up receipt's refusal is logged at debug: no error to expect (APassingSetupCheckLogsItsMadeUpReceiptsRefusalAtDebug...).
	FFixture F(FFlockFakeTransport::Coded(422, TEXT("payment_provider.receipt_invalid")),
		{ Item(TEXT("gems"), TEXT("currency_pack"), FString::Printf(TEXT("{\"google_play\":\"%s\"}"), *Gems)) });
	FFlockStorePrice Found;
	Found.bFoundInTheStore = true;
	F.Store->Prices.Add(Gems, Found);
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockStoreSetupCheck>> Answer;
	F.Connected->CheckSetup(Answer.Into());
	TestTrue(TEXT("Ready"), Answer.IsSet() && Answer->Value.IsReady());
	return true;
}

FLOCK_STORE_TEST(FFlockStoreSetupQuietTest, "APassingSetupCheckLogsItsMadeUpReceiptsRefusalAtDebugAndARealRefusalStillWarns")
bool FFlockStoreSetupQuietTest::RunTest(const FString& Parameters)
{
	FFixture F(FFlockFakeTransport::Status(422, ReceiptInvalid),
		{ Item(TEXT("gems"), TEXT("currency_pack"), FString::Printf(TEXT("{\"google_play\":\"%s\"}"), *Gems)) });
	const TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
	F.Sdk->SetLogger(Log);
	FFlockStorePrice Found;
	Found.bFoundInTheStore = true;
	F.Store->Prices.Add(Gems, Found);
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockStoreSetupCheck>> Answer;
	F.Connected->CheckSetup(Answer.Into());
	TestTrue(TEXT("Ready"), Answer.IsSet() && Answer->Value.IsReady());
	TestEqual(TEXT("No warning: the refusal is the answer the check asked for"), Log->Warnings.Num(), 0);
	TestEqual(TEXT("No error"), Log->Errors.Num(), 0);
	TestTrue(TEXT("The HTTP line is at debug"), FFlockRecordingLogger::AnyContains(Log->Debugs, TEXT("the answer the call asked for")));
	TestTrue(TEXT("The provider's line is at debug"), FFlockRecordingLogger::AnyContains(Log->Debugs, TEXT("Redeem store receipt [C++] failed")));
	TestTrue(TEXT("The scope closed with the check"), FFlockExpectedAnswer::Get().IsEmpty());

	// Counter-case: a game's own receipt refused the same way is a failed purchase, and says so.
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Redeemed;
	F.Sdk->GetShopProvider()->RedeemStoreReceipt(TEXT("gems"), FFlockStoreReceipt::FromGooglePlay(Gems, TEXT("token-1")), Redeemed.Into());
	TestTrue(TEXT("Refused"), Redeemed.IsSet() && Redeemed->Error.ErrorCode == EFlockErrorCode::PaymentProviderReceiptInvalid);
	TestTrue(TEXT("The HTTP line warns"), FFlockRecordingLogger::AnyContains(Log->Warnings, TEXT("<- 422 POST")));
	TestTrue(TEXT("The provider's line is an error"), FFlockRecordingLogger::AnyContains(Log->Errors, TEXT("Redeem store receipt [C++] failed")));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreSetupSignedOutTest, "TheSetupCheckWithNobodySignedInFailsAndSendsNothing")
bool FFlockStoreSetupSignedOutTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Connect();
	const TAnswer<TFlockResult<FFlockStoreSetupCheck>> Answer;
	F.Connected->CheckSetup(Answer.Into());
	TestTrue(TEXT("Refused"), Answer.IsSet() && Answer->Error.Type == EFlockErrorType::Auth);
	TestEqual(TEXT("Nothing sent"), F.RedemptionBodies().Num(), 0);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreDisconnectTest, "AfterDisconnectEveryCallIsRefusedTheStoreIsNotAskedAndItStoppedListening")
bool FFlockStoreDisconnectTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Fake->On(TEXT("shop_item/gems"), FFlockFakeTransport::Ok(CatalogItems()[0]));
	F.SignIn(TEXT("player-a"));
	F.Connect();
	UFlockStore* Store = F.Connected;
	Store->Disconnect();
	TestEqual(TEXT("Stopped listening"), F.Store->Count(TEXT("stop")), 1);
	TestFalse(TEXT("No price"), Store->GetPrice(TEXT("gems")).IsSuccess());
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Bought;
	Store->Purchase(TEXT("gems"), Bought.Into());
	TestTrue(TEXT("No purchase"), Bought.IsSet() && !Bought->IsSuccess());
	const TAnswer<TFlockResult<FFlockStoreRestore>> Restored;
	Store->Restore(Restored.Into());
	TestTrue(TEXT("No restore"), Restored.IsSet() && !Restored->IsSuccess());
	const TAnswer<TFlockResult<FFlockPurchaseResult>> ThroughTheShop = F.Buy(TEXT("gems"));
	TestTrue(TEXT("The shop no longer buys real money through it"), ThroughTheShop.IsSet() && ThroughTheShop->Error.Hint.Contains(TEXT("UFlockStore::Connect")));
	TestFalse(TEXT("The store is not asked"), F.Store->AnyStartingWith(TEXT("buy:")) || F.Store->Count(TEXT("restore")) > 0);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreDisconnectEndsWaitTest, "DisconnectEndsAPurchaseWaitingForTheStoreAsCancelled")
bool FFlockStoreDisconnectEndsWaitTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer;
	F.Sdk->GetShopProvider()->Purchase(TEXT("gems"), Answer.Into());
	TestFalse(TEXT("Waiting"), Answer.IsSet());
	F.Connected->Disconnect();
	TestTrue(TEXT("Ended Cancelled"), Answer.IsSet() && Answer->Error.Type == EFlockErrorType::Cancelled);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreShutdownTest, "AShutdownDisconnectsTheStoreAndEndsAPurchaseWaitingForIt")
bool FFlockStoreShutdownTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer;
	F.Sdk->GetShopProvider()->Purchase(TEXT("gems"), Answer.Into());
	UFlockStore* Store = F.Connected;
	F.Sdk->ShutdownSdk();
	TestTrue(TEXT("Ended Cancelled"), Answer.IsSet() && Answer->Error.Type == EFlockErrorType::Cancelled);
	TestNull(TEXT("Nothing held"), F.Sdk->GetStore());
	TestFalse(TEXT("Disconnected"), Store->IsConnected());
	TestEqual(TEXT("Stopped listening"), F.Store->Count(TEXT("stop")), 1);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreNotForSaleTest, "AnItemSwitchedOffSinceTheStoreConnectedIsRefusedBeforeTheStoreTakesPayment")
bool FFlockStoreNotForSaleTest::RunTest(const FString& Parameters)
{
	for (const bool bTheWholeShop : { false, true })
	{
		FFixture F;
		F.SignIn(TEXT("player-a"));
		F.Connect();
		if (bTheWholeShop)
		{
			F.Fake->On(TEXT("shop/shop-1"), FFlockFakeTransport::Coded(404, TEXT("shop.shop_not_found")));
		}
		else
		{
			TArray<FString> Rest = CatalogItems();
			Rest.RemoveAt(0);
			F.Fake->On(TEXT("shop/shop-1"), FFlockFakeTransport::Ok(ShopOf(Rest)));
		}
		const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer = F.Buy(TEXT("gems"));
		const FString Case = bTheWholeShop ? TEXT("the shop switched off") : TEXT("the item switched off");
		TestTrue(Case + TEXT(": refused on its code"), Answer.IsSet() && Answer->Error.ErrorCode == EFlockErrorCode::ShopItemNotForSale);
		TestFalse(Case + TEXT(": the store is not asked"), F.Store->AnyStartingWith(TEXT("buy:")));
	}
	return true;
}

FLOCK_STORE_TEST(FFlockStoreSignInChangedTest, "AnotherPlayerSigningInWhileTheShopIsReadMeansTheStoreIsNotAsked")
bool FFlockStoreSignInChangedTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	F.Fake->bDeferred = true;
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer;
	F.Sdk->GetShopProvider()->Purchase(TEXT("gems"), Answer.Into());
	F.Fake->bDeferred = false;
	F.Sdk->Logout();
	F.SignIn(TEXT("player-b"));
	F.Fake->FlushPending();
	TestTrue(TEXT("Ended Cancelled"), Answer.IsSet() && Answer->Error.Type == EFlockErrorType::Cancelled);
	TestFalse(TEXT("The store is not asked"), F.Store->AnyStartingWith(TEXT("buy:")));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreSignOutWhileWaitingTest, "ASignOutWhileTheStoreIsWaitedForEndsTheCallAndThePaymentIsDeliveredAtTheNextSignIn")
bool FFlockStoreSignOutWhileWaitingTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Connect();
	const TAnswer<TFlockResult<FFlockPurchaseResult>> Answer;
	F.Sdk->GetShopProvider()->Purchase(TEXT("gems"), Answer.Into());
	F.Sdk->Logout();
	TestTrue(TEXT("Ended Cancelled"), Answer.IsSet() && Answer->Error.Type == EFlockErrorType::Cancelled);
	F.Store->Pay(Gems, TEXT("token-1"));
	TestEqual(TEXT("Held while signed out"), F.RedemptionBodies().Num(), 0);
	F.SignIn(TEXT("player-a"));
	TestEqual(TEXT("Delivered at the next sign-in"), F.Completed, TArray<FString>{ TEXT("gems:pur-1") });
	return true;
}

FLOCK_STORE_TEST(FFlockStoreSoldBeforeTest, "AnUnfinishedPurchaseOfAnItemTheShopsNoLongerListIsDeliveredAtTheNextLaunch")
bool FFlockStoreSoldBeforeTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Connect();
	F.Sdk->ShutdownSdk();
	// The next launch: the gems are switched off, so the shops no longer list them; the item's own read still finds it.
	TArray<FString> Rest = CatalogItems();
	Rest.RemoveAt(0);
	F.Serve(Rest);
	F.Fake->On(TEXT("shop_item/gems"), FFlockFakeTransport::Ok(CatalogItems()[0]));
	F.Sdk->InitializeWithConfig(Config());
	F.Store = MakeShared<FFakeStore>();
	F.SignIn(TEXT("player-a"));
	F.Connect();
	TestTrue(TEXT("The product sold before is still registered, so the store hands its purchases back"), F.Store->Calls.Contains(TEXT("register:") + Gems + TEXT(":consumable")));
	F.Store->Pay(Gems, TEXT("token-paid"));
	const TArray<FString> Sent = F.RedemptionBodies();
	TestTrue(TEXT("Sent as the item it was bought as"), Sent.Num() == 1 && Sent[0].Equals(FString::Printf(
		TEXT("{\"shop_item_id\":\"gems\",\"provider\":\"google_play\",\"receipt\":{\"product_id\":\"%s\",\"purchase_token\":\"token-paid\"}}"), *Gems),
		ESearchCase::CaseSensitive));
	TestTrue(TEXT("Finished once Flock delivered"), F.Store->Calls.Contains(TEXT("finish:token-paid")));
	TestEqual(TEXT("Delivered"), F.Completed, TArray<FString>{ TEXT("gems:pur-1") });
	return true;
}

FLOCK_STORE_TEST(FFlockStoreSoldBeforeGoneTest, "AnUnfinishedPurchaseOfAnItemNoLongerFoundIsReportedAndStaysUnfinished")
bool FFlockStoreSoldBeforeGoneTest::RunTest(const FString& Parameters)
{
	// The SDK logs each refusal the server sends.
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F;
	F.Connect();
	F.Sdk->ShutdownSdk();
	TArray<FString> Rest = CatalogItems();
	Rest.RemoveAt(0);
	F.Serve(Rest);
	F.Fake->On(TEXT("shop_item/gems"), FFlockFakeTransport::Coded(404, TEXT("shop_item.shop_item_not_found")));
	F.Sdk->InitializeWithConfig(Config());
	F.Store = MakeShared<FFakeStore>();
	F.SignIn(TEXT("player-a"));
	F.Connect();
	F.Store->Pay(Gems, TEXT("token-1"));
	TestEqual(TEXT("Reported"), F.NotDelivered.Num(), 1);
	TestTrue(TEXT("On the server's code"), F.NotDelivered.Num() == 1 && F.NotDelivered[0].Error.ErrorCode == EFlockErrorCode::ShopItemShopItemNotFound);
	TestFalse(TEXT("Not finished"), F.Store->AnyStartingWith(TEXT("finish:")));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreSoldBeforePerGameTest, "ProductsSoldBeforeAreKeptForTheirOwnGame")
bool FFlockStoreSoldBeforePerGameTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Connect();
	F.Sdk->ShutdownSdk();
	// Another game on the same machine: its catalog sells nothing of the first game's.
	F.Serve({ Item(TEXT("other"), TEXT("consumable"), TEXT("{\"google_play\":\"com.other\"}")) });
	F.Sdk->InitializeWithConfig(Config(TEXT("another-game")));
	F.Store = MakeShared<FFakeStore>();
	F.Connect();
	TestFalse(TEXT("Another game's key: the first game's products are not registered"), F.Store->AnyStartingWith(TEXT("register:") + Gems));
	F.Sdk->ShutdownSdk();
	FFlockInitConfig AnotherBackend = Config();
	AnotherBackend.ApiUrl = TEXT("https://another-backend.invalid");
	F.Sdk->InitializeWithConfig(AnotherBackend);
	F.Store = MakeShared<FFakeStore>();
	F.Connect();
	TestFalse(TEXT("Another backend with the same key: nor are they there"), F.Store->AnyStartingWith(TEXT("register:") + Gems));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreConnectRefusalsTest, "AConnectToNoStoreOrWithoutAStoreLibraryIsRefusedWithItsFix")
bool FFlockStoreConnectRefusalsTest::RunTest(const FString& Parameters)
{
	FFixture F;
	// Flock's store module's library, is put back whatever happens below.
	const TFunction<TSharedPtr<IFlockStoreConnection>()> Registered = UFlockStore::GetStoreLibrary();
	ON_SCOPE_EXIT { UFlockStore::SetStoreLibrary(Registered); };
	auto ConnectWith = [&F](const FString& Provider)
	{
		FFlockStoreOptions Options;
		Options.PaymentProvider = Provider;
		TOptional<FFlockError> Error;
		UFlockStore::Connect(F.Sdk, Options, [&Error](TFlockResult<UFlockStore*> Result) { Error = Result.IsSuccess() ? FFlockError() : Result.Error; });
		return Error;
	};
	UFlockStore::ClearStoreLibrary();
	const TOptional<FFlockError> NoLibrary = ConnectWith(FlockPaymentProviders::GooglePlay);
	TestTrue(TEXT("Without a store library: refused, naming the plugin to keep enabled"), NoLibrary.IsSet() && NoLibrary->Hint.Contains(TEXT("Online Subsystem plugin")));
	const TOptional<FFlockError> NotAStore = ConnectWith(TEXT("paypal"));
	TestTrue(TEXT("Neither a store nor a gateway: refused"), NotAStore.IsSet() && NotAStore->Type == EFlockErrorType::Validation
		&& NotAStore->Message.Contains(TEXT("not a store or a payment gateway")));
	if (UFlockStore::PaymentProviderOfThisPlatform().IsEmpty())
	{
		// A platform with no store of its own pays through a web checkout, without a store library.
		const TOptional<FFlockError> NoneHere = ConnectWith(FString());
		TestTrue(TEXT("A platform with no store of its own: a web checkout"), NoneHere.IsSet() && NoneHere->Type == EFlockErrorType::None
			&& F.Sdk->GetStore() != nullptr && F.Sdk->GetStore()->IsWebCheckout());
		if (F.Sdk->GetStore() != nullptr)
		{
			F.Sdk->GetStore()->Disconnect();
		}
	}
	const TSharedRef<FFakeStore> Apple = MakeShared<FFakeStore>();
	Apple->Provider = FlockPaymentProviders::AppleAppStore;
	UFlockStore::SetStoreLibrary([Apple]() { return Apple; });
	const TOptional<FFlockError> OtherStore = ConnectWith(FlockPaymentProviders::GooglePlay);
	TestTrue(TEXT("A library reaching another store: refused"), OtherStore.IsSet() && OtherStore->Message.Contains(TEXT("not 'google_play'")));
	const TOptional<FFlockError> ItsOwn = ConnectWith(FlockPaymentProviders::AppleAppStore);
	TestTrue(TEXT("Its own store connects through the library"), ItsOwn.IsSet() && ItsOwn->Type == EFlockErrorType::None);
	TestEqual(TEXT("Connected through it"), Apple->Count(TEXT("connect")), 1);
	return true;
}

// ── Blueprint nodes over a store ──

FLOCK_STORE_TEST(FFlockStoreNodeConnectTest, "Nodes.ConnectStoreConnectsThroughTheStoreLibraryAndASecondIsRefused")
bool FFlockStoreNodeConnectTest::RunTest(const FString& Parameters)
{
	FFixture F;
	// Flock's store module's library is put back whatever happens below.
	const TFunction<TSharedPtr<IFlockStoreConnection>()> Registered = UFlockStore::GetStoreLibrary();
	ON_SCOPE_EXIT { UFlockStore::SetStoreLibrary(Registered); };
	const TSharedRef<FFakeStore> Library = F.Store;
	UFlockStore::SetStoreLibrary([Library]() { return Library; });
	FFlockStoreOptions Options;
	Options.PaymentProvider = FlockPaymentProviders::GooglePlay;

	FFlockStoreNodes First;
	First.Connect(UFlockConnectStoreAction::ConnectStore(nullptr, Options))->ActivateWithFlockForTesting(F.Sdk);
	TestEqualSensitive(TEXT("The success pin, once"), First->FiredText(), FString(TEXT("success")));
	TestTrue(TEXT("It carries the store Flock holds"), First->Store != nullptr && First->Store == F.Sdk->GetStore());
	TestEqualSensitive(TEXT("Through the library's store"), First->Store != nullptr ? First->Store->GetPaymentProvider() : FString(),
		FString(FlockPaymentProviders::GooglePlay));
	TestEqual(TEXT("Connected through the library"), F.Store->Count(TEXT("connect")), 1);

	FFlockStoreNodes Second;
	Second.Connect(UFlockConnectStoreAction::ConnectStore(nullptr, Options))->ActivateWithFlockForTesting(F.Sdk);
	TestEqualSensitive(TEXT("A second connect: the failure pin, once"), Second->FiredText(), FString(TEXT("failure")));
	TestTrue(TEXT("No store on it"), Second->Store == nullptr);
	TestFalse(TEXT("With a fix"), Second->Error.Hint.IsEmpty());
	TestTrue(TEXT("The first is still held"), F.Sdk->GetStore() == First->Store);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreNodePurchaseTest, "Nodes.ThePurchaseNodeBuysARealMoneyItemAndFlocksEventsHearItOnce")
bool FFlockStoreNodePurchaseTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Store->OnBuy = [&F](const FString& ProductId) { F.Store->PayFromTheDialog(ProductId, TEXT("token-1")); };
	F.SignIn(TEXT("player-a"));
	FFlockStoreNodes Nodes;
	Nodes->Hear(F.Sdk->GetEvents());
	F.Connect();

	Nodes.Buy(UFlockPurchaseAction::Purchase(nullptr, TEXT("gems"), FString()))->ActivateWithFlockForTesting(F.Sdk);

	TestEqualSensitive(TEXT("The success pin, once"), Nodes->FiredText(), FString(TEXT("success")));
	TestTrue(TEXT("Paid"), Nodes->PurchaseResult.IsPaid());
	TestEqualSensitive(TEXT("The purchase"), Nodes->PurchaseResult.PurchaseId, FString(TEXT("pur-1")));
	TestEqual(TEXT("Flock's events heard the delivery once"), Nodes->Completed, TArray<FString>{ TEXT("gems:pur-1") });
	TestEqual(TEXT("As the store's own event did"), F.Completed, Nodes->Completed);
	TestEqual(TEXT("Nothing reported as not delivered"), Nodes->NotDelivered.Num(), 0);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreNodeCancelTest, "Nodes.APlayerCancelFiresThePurchaseNodesFailurePinWithTheCancelledReason")
bool FFlockStoreNodeCancelTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Store->OnBuy = [&F](const FString& ProductId) { F.Store->Fail(ProductId, FlockStoreFailureReasons::Cancelled, TEXT("UserCancelled: closed the dialog")); };
	F.SignIn(TEXT("player-a"));
	FFlockStoreNodes Nodes;
	Nodes->Hear(F.Sdk->GetEvents());
	F.Connect();

	Nodes.Buy(UFlockPurchaseAction::Purchase(nullptr, TEXT("gems"), FString()))->ActivateWithFlockForTesting(F.Sdk);

	TestEqualSensitive(TEXT("The failure pin, once"), Nodes->FiredText(), FString(TEXT("failure")));
	TestEqual(TEXT("A store refusal"), Nodes->Error.Type, EFlockErrorType::Store);
	TestEqualSensitive(TEXT("Its reason is what the Cancelled node spells"), Nodes->Error.StoreFailureReason, UFlockStoreLibrary::StoreFailureReasonCancelled());
	TestFalse(TEXT("Nothing charged, so nothing in doubt"), Nodes->Error.bOutcomeUnknown);
	TestEqual(TEXT("Nothing heard"), Nodes->Completed.Num() + Nodes->NotDelivered.Num(), 0);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreNodeNotDeliveredTest, "Nodes.FlocksEventsHearAPaidPurchaseFlockDidNotDeliverOnce")
bool FFlockStoreNodeNotDeliveredTest::RunTest(const FString& Parameters)
{
	// The SDK logs each refusal the server sends.
	AddExpectedError(TEXT("] failed:"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F(FFlockFakeTransport::Status(422, CredentialsRejected));
	F.Store->OnBuy = [&F](const FString& ProductId) { F.Store->PayFromTheDialog(ProductId, TEXT("token-1")); };
	F.SignIn(TEXT("player-a"));
	FFlockStoreNodes Nodes;
	Nodes->Hear(F.Sdk->GetEvents());
	F.Connect();

	Nodes.Buy(UFlockPurchaseAction::Purchase(nullptr, TEXT("gems"), FString()))->ActivateWithFlockForTesting(F.Sdk);

	TestEqualSensitive(TEXT("The failure pin, once"), Nodes->FiredText(), FString(TEXT("failure")));
	TestEqual(TEXT("On its code"), Nodes->Error.ErrorCode, EFlockErrorCode::PaymentProviderStoreCredentialsRejected);
	TestEqual(TEXT("Heard once"), Nodes->NotDelivered.Num(), 1);
	TestTrue(TEXT("With its item and code"), Nodes->NotDelivered.Num() == 1 && Nodes->NotDelivered[0].bHasItem
		&& Nodes->NotDelivered[0].Item.Id == TEXT("gems") && Nodes->NotDelivered[0].Error.ErrorCode == EFlockErrorCode::PaymentProviderStoreCredentialsRejected);
	TestEqual(TEXT("As the store's own event did"), F.NotDelivered.Num(), 1);
	TestEqual(TEXT("Nothing heard as delivered"), Nodes->Completed.Num(), 0);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreNodeHeardAcrossConnectionsTest, "Nodes.FlocksEventsBoundOnceHearTheNextConnectionToo")
bool FFlockStoreNodeHeardAcrossConnectionsTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Store->OnBuy = [&F](const FString& ProductId) { F.Store->PayFromTheDialog(ProductId, TEXT("token-1")); };
	F.SignIn(TEXT("player-a"));
	FFlockStoreNodes Nodes;
	Nodes->Hear(F.Sdk->GetEvents());
	F.Connect();
	F.Buy(TEXT("gems"));
	F.Connected->Disconnect();

	// The next connection's store hands back a purchase an earlier launch left unfinished.
	const TSharedRef<FFakeStore> Next = MakeShared<FFakeStore>();
	bool bConnected = false;
	UFlockStore::ConnectThrough(F.Sdk, Next, [&bConnected](TFlockResult<UFlockStore*> Result) { bConnected = Result.IsSuccess(); });
	TestTrue(TEXT("Connected again"), bConnected);
	Next->Pay(Gems, TEXT("token-2"));

	TestEqual(TEXT("Both connections' deliveries heard, each once"), Nodes->Completed, TArray<FString>{ TEXT("gems:pur-1"), TEXT("gems:pur-1") });
	TestEqual(TEXT("The handed-back one finished in the next store"), Next->Count(TEXT("finish:token-2")), 1);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreNodePriceTest, "Nodes.GetStorePriceReadsTheStoresPriceAndWithNoStoreConnectedSaysHowToConnectOne")
bool FFlockStoreNodePriceTest::RunTest(const FString& Parameters)
{
	FFixture F;
	FFlockStorePrice Price;
	FFlockError Error;
	TestFalse(TEXT("No store connected: false"), UFlockStoreLibrary::GetStorePriceWithFlockForTesting(F.Sdk, TEXT("gems"), Price, Error));
	TestEqualSensitive(TEXT("Saying no store is connected"), Error.Message, FString(TEXT("No store is connected")));
	TestEqualSensitive(TEXT("And how to connect one"), Error.Hint, FFlockErrorHints::ConnectAStoreFirst());

	FFlockStorePrice Stores;
	Stores.bFoundInTheStore = true;
	Stores.Text = TEXT("SAR 4.59");
	Stores.AmountInSmallestUnit = 459;
	Stores.CurrencyCode = TEXT("SAR");
	F.Store->Prices.Add(Gems, Stores);
	F.Connect();
	TestTrue(TEXT("Connected: true"), UFlockStoreLibrary::GetStorePriceWithFlockForTesting(F.Sdk, TEXT("gems"), Price, Error));
	TestEqual(TEXT("No error"), Error.Type, EFlockErrorType::None);
	TestEqualSensitive(TEXT("The store's own text"), Price.Text, FString(TEXT("SAR 4.59")));
	TestEqual(TEXT("In the smallest unit"), Price.AmountInSmallestUnit, static_cast<int64>(459));

	TestFalse(TEXT("An item with no product id for this store: false"), UFlockStoreLibrary::GetStorePriceWithFlockForTesting(F.Sdk, TEXT("shield"), Price, Error));
	TestEqual(TEXT("Refused as the store refuses it"), Error.Type, EFlockErrorType::Validation);
	TestFalse(TEXT("With its fix"), Error.Hint.IsEmpty());
	TestFalse(TEXT("And no price left from the call before"), Price.bFoundInTheStore);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreNodeRestoreAndSetupTest, "Nodes.RestoreAndCheckSetupAnswerThroughTheConnectedStoreAndRefuseWithNone")
bool FFlockStoreNodeRestoreAndSetupTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Store->Own(Sword, TEXT("token-sword"));
	F.SignIn(TEXT("player-a"));

	FFlockStoreNodes NoStoreRestore;
	NoStoreRestore.Restore(UFlockRestorePurchasesAction::RestorePurchases(nullptr))->ActivateWithFlockForTesting(F.Sdk);
	FFlockStoreNodes NoStoreSetup;
	NoStoreSetup.Setup(UFlockCheckStoreSetupAction::CheckStoreSetup(nullptr))->ActivateWithFlockForTesting(F.Sdk);
	TestEqualSensitive(TEXT("No store: Restore's failure pin"), NoStoreRestore->FiredText(), FString(TEXT("failure")));
	TestEqualSensitive(TEXT("Saying how to connect one"), NoStoreRestore->Error.Hint, FFlockErrorHints::ConnectAStoreFirst());
	TestEqualSensitive(TEXT("No store: Check Setup's failure pin"), NoStoreSetup->FiredText(), FString(TEXT("failure")));
	TestEqualSensitive(TEXT("Saying how to connect one"), NoStoreSetup->Error.Hint, FFlockErrorHints::ConnectAStoreFirst());
	TestEqual(TEXT("Nothing asked of the store"), F.Store->Count(TEXT("restore")), 0);

	F.Connect();
	FFlockStoreNodes Restore;
	Restore.Restore(UFlockRestorePurchasesAction::RestorePurchases(nullptr))->ActivateWithFlockForTesting(F.Sdk);
	TestEqualSensitive(TEXT("Restore's success pin, once"), Restore->FiredText(), FString(TEXT("success")));
	TestTrue(TEXT("The owned item restored"), Restore->Restore.Restored.Num() == 1 && Restore->Restore.Restored[0].Id == TEXT("sword"));
	TestEqual(TEXT("Asked the store once"), F.Store->Count(TEXT("restore")), 1);

	FFlockStoreNodes Setup;
	Setup.Setup(UFlockCheckStoreSetupAction::CheckStoreSetup(nullptr))->ActivateWithFlockForTesting(F.Sdk);
	TestEqualSensitive(TEXT("Check Setup's success pin, once"), Setup->FiredText(), FString(TEXT("success")));
	TestTrue(TEXT("The item with no product id here"), Setup->SetupCheck.MissingProductId.ContainsByPredicate([](const FFlockShopItem& Item) { return Item.Id == TEXT("shield"); }));
	TestFalse(TEXT("Not ready"), UFlockStoreLibrary::IsStoreReady(Setup->SetupCheck));
	return true;
}

FLOCK_STORE_TEST(FFlockStoreNodeSignedOutTest, "Nodes.RestoreWithNobodySignedInFiresTheFailurePinWithItsFix")
bool FFlockStoreNodeSignedOutTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Store->Own(Sword, TEXT("token-sword"));
	F.Connect();
	FFlockStoreNodes Nodes;
	Nodes.Restore(UFlockRestorePurchasesAction::RestorePurchases(nullptr))->ActivateWithFlockForTesting(F.Sdk);
	TestEqualSensitive(TEXT("The failure pin, once"), Nodes->FiredText(), FString(TEXT("failure")));
	TestEqual(TEXT("A sign-in failure"), Nodes->Error.Type, EFlockErrorType::Auth);
	TestFalse(TEXT("With a fix"), Nodes->Error.Hint.IsEmpty());
	TestEqual(TEXT("Nothing redeemed"), F.RedemptionBodies().Num(), 0);
	return true;
}

FLOCK_STORE_TEST(FFlockStoreNodeRedeemTest, "Nodes.RedeemStoreReceiptRedeemsTheReceiptAsTheSignedInPlayer")
bool FFlockStoreNodeRedeemTest::RunTest(const FString& Parameters)
{
	FFixture F;
	FFlockStoreNodes SignedOut;
	SignedOut.Redeem(UFlockRedeemStoreReceiptAction::RedeemStoreReceipt(nullptr, TEXT("gems"),
		UFlockStoreLibrary::MakeGooglePlayReceipt(Gems, TEXT("token-1"))))->ActivateWithFlockForTesting(F.Sdk);
	TestEqualSensitive(TEXT("Signed out: the failure pin, once"), SignedOut->FiredText(), FString(TEXT("failure")));
	TestEqual(TEXT("A sign-in failure"), SignedOut->Error.Type, EFlockErrorType::Auth);
	TestEqual(TEXT("Nothing sent"), F.RedemptionBodies().Num(), 0);

	F.SignIn(TEXT("player-a"));
	FFlockStoreNodes Nodes;
	Nodes.Redeem(UFlockRedeemStoreReceiptAction::RedeemStoreReceipt(nullptr, TEXT("gems"),
		UFlockStoreLibrary::MakeGooglePlayReceipt(Gems, TEXT("token-1"))))->ActivateWithFlockForTesting(F.Sdk);
	TestEqualSensitive(TEXT("The success pin, once"), Nodes->FiredText(), FString(TEXT("success")));
	TestEqualSensitive(TEXT("The purchase"), Nodes->PurchaseResult.PurchaseId, FString(TEXT("pur-1")));
	const TArray<FString> Bodies = F.RedemptionBodies();
	TestEqual(TEXT("One redemption"), Bodies.Num(), 1);
	if (Bodies.Num() == 1)
	{
		TestEqualSensitive(TEXT("For the item"), Field(Bodies[0], TEXT("shop_item_id")), FString(TEXT("gems")));
		TestEqualSensitive(TEXT("From Google Play"), Field(Bodies[0], TEXT("provider")), FString(TEXT("google_play")));
		TestEqualSensitive(TEXT("Its product"), Field(Bodies[0], TEXT("receipt.product_id")), Gems);
		TestEqualSensitive(TEXT("Its token"), Field(Bodies[0], TEXT("receipt.purchase_token")), FString(TEXT("token-1")));
	}
	return true;
}

FLOCK_STORE_TEST(FFlockStoreNodeGetAndDisconnectTest, "Nodes.GetStoreIsTheHeldStoreAndDisconnectStoreEndsIt")
bool FFlockStoreNodeGetAndDisconnectTest::RunTest(const FString& Parameters)
{
	FFixture F;
	TestTrue(TEXT("None connected: Get Store is none"), UFlockStoreLibrary::GetStoreWithFlockForTesting(F.Sdk) == nullptr);
	UFlockStoreLibrary::DisconnectStoreWithFlockForTesting(F.Sdk);
	TestEqual(TEXT("None connected: Disconnect Store does nothing"), F.Store->Count(TEXT("stop")), 0);

	F.Connect();
	TestTrue(TEXT("Get Store is the store Flock holds"), F.Connected != nullptr && UFlockStoreLibrary::GetStoreWithFlockForTesting(F.Sdk) == F.Connected);
	UFlockStoreLibrary::DisconnectStoreWithFlockForTesting(F.Sdk);
	TestFalse(TEXT("Disconnect Store disconnects it"), F.Connected != nullptr && F.Connected->IsConnected());
	TestEqual(TEXT("It stops listening to the store"), F.Store->Count(TEXT("stop")), 1);
	TestTrue(TEXT("Get Store is none again"), UFlockStoreLibrary::GetStoreWithFlockForTesting(F.Sdk) == nullptr);
	FFlockStorePrice Price;
	FFlockError Error;
	TestFalse(TEXT("And Get Store Price says none is connected"), UFlockStoreLibrary::GetStorePriceWithFlockForTesting(F.Sdk, TEXT("gems"), Price, Error));
	TestEqualSensitive(TEXT("With how to connect one"), Error.Hint, FFlockErrorHints::ConnectAStoreFirst());
	return true;
}

#endif // WITH_AUTOMATION_TESTS
