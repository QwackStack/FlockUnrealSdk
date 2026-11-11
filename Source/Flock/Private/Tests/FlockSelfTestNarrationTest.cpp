// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS && !UE_BUILD_SHIPPING

#include "FlockSelfTestNarration.h"
#include "Models/FlockShopModels.h"
#include "Tests/Support/FlockRecordingLogger.h"

// The self-test's purchase narration, covered here rather than left to the live run.
//
// Every state below is one the live run could not produce when they were written: reward granting was
// failing server-side, so the backend had never serialized a successful purchase result. Without these, the first
// time anyone saw this output would be the first time the backend fix worked — and a wrong branch would
// read as a backend problem rather than an SDK one.
//
// These assert what the self-test *says*. That is a real requirement here: the shop leg's log is the
// only place the 1.7.0 response shape gets proven end to end, so a line that silently stops printing a
// grant would take the evidence with it.

namespace FlockSelfTestNarrationTestHelpers
{
	inline FFlockShopItemReward Reward(const FString& Code, int32 Amount, const FString& Type = FString())
	{
		FFlockShopItemReward R;
		R.Code = Code;
		R.Amount = Amount;
		if (!Type.IsEmpty())
		{
			R.Type = Type;
		}
		return R;
	}

	/** A reward-bearing purchase: a row, two grants, and the wallet afterwards. */
	inline FFlockPurchaseResult FullResult()
	{
		FFlockPurchaseResult Result;
		Result.PurchaseId = TEXT("pur-1");
		Result.ItemType = TEXT("currency_pack");
		Result.Inventory.Id = TEXT("inv-1");
		Result.Granted.Add(Reward(TEXT("GOLD"), 500));
		Result.Granted.Add(Reward(TEXT("GEMS"), 10));
		Result.Wallet.Id = TEXT("pd-1");
		return Result;
	}
}

using namespace FlockSelfTestNarrationTestHelpers;

// ── A full purchase names the id, the type, the row, every grant, and the wallet ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSelfTestNarrationFullTest, "Flock.SelfTest.Narration.ReportsGrantsAndWallet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSelfTestNarrationFullTest::RunTest(const FString& Parameters)
{
	FFlockRecordingLogger Log;
	FlockNarratePurchaseResult(FullResult(), Log);

	TestTrue(TEXT("the purchase id is reported"), FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("purchase_id=pur-1")));
	TestTrue(TEXT("the item type is reported"), FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("item_type=currency_pack")));
	TestTrue(TEXT("the inventory row is named"), FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("inventory entry inv-1")));

	// One line per grant, each carrying amount, code and type — this is the evidence the response
	// shape is right, so a grant that stops printing has to fail a test rather than pass quietly.
	TestTrue(TEXT("the first grant is itemised"), FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("granted 500 x GOLD")));
	TestTrue(TEXT("the second grant is itemised"), FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("granted 10 x GEMS")));
	TestFalse(TEXT("and it does not also claim nothing was granted"),
		FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("granted nothing")));

	TestTrue(TEXT("the wallet is named"), FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("player data row pd-1")));

	// Narration, not a fault: an ordinary purchase must not reach the log as an error or a warning,
	// because a log that cries wolf on success is a log people stop reading.
	TestEqual(TEXT("nothing is logged as an error"), Log.Errors.Num(), 0);
	TestEqual(TEXT("nothing is logged as a warning"), Log.Warnings.Num(), 0);

	return true;
}

// ── A currency pack creates no row, and the log has to say so rather than print an empty id ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSelfTestNarrationNoRowTest, "Flock.SelfTest.Narration.NamesTheMissingInventoryRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSelfTestNarrationNoRowTest::RunTest(const FString& Parameters)
{
	FFlockPurchaseResult Result = FullResult();
	Result.Inventory = FFlockPlayerInventory();   // the wire sends `inventory: null`

	FFlockRecordingLogger Log;
	FlockNarratePurchaseResult(Result, Log);

	// The failure this guards against is a line reading "inventory entry " with nothing after it, which
	// looks like a truncated log rather than the legitimate no-row case it actually is.
	TestTrue(TEXT("the absent row is named, not left blank"),
		FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("(none - item granted its contents outright)")));
	TestFalse(TEXT("and no stale row id is printed"),
		FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("inventory entry inv-1")));

	// The grant is the whole point of a purchase that produced no row, so it still has to be reported.
	TestTrue(TEXT("the grant is still itemised"), FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("granted 500 x GOLD")));

	return true;
}

// ── A plain item grants nothing and moves no currency: both absences are stated, not skipped ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSelfTestNarrationPlainTest, "Flock.SelfTest.Narration.StatesEmptyGrantAndWallet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSelfTestNarrationPlainTest::RunTest(const FString& Parameters)
{
	FFlockPurchaseResult Result;
	Result.PurchaseId = TEXT("pur-2");
	Result.ItemType = TEXT("standard");
	Result.Inventory.Id = TEXT("inv-9");
	// Granted and Wallet left at their defaults — the server omits both keys for a plain item.

	FFlockRecordingLogger Log;
	FlockNarratePurchaseResult(Result, Log);

	TestTrue(TEXT("the row is reported"), FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("inventory entry inv-9")));

	// Silence would be ambiguous: it reads the same as narration that failed to run. Say it explicitly.
	TestTrue(TEXT("an empty grant list is stated"),
		FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("granted nothing")));
	TestFalse(TEXT("and no phantom grant line appears"),
		FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("granted 0 x")));

	TestTrue(TEXT("an absent wallet is stated as moving no currency"),
		FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("moved no currency")));
	TestFalse(TEXT("and no empty wallet row is printed"),
		FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("player data row")));

	return true;
}

// ── A reward kind the server adds later prints as itself ──
// The reason Type is a string and not a UENUM: a new kind must survive to the log readably rather than
// arriving as "unknown", which would make a server-side addition look like an SDK failure.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSelfTestNarrationUnknownTypeTest, "Flock.SelfTest.Narration.PrintsUnknownRewardTypeVerbatim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSelfTestNarrationUnknownTypeTest::RunTest(const FString& Parameters)
{
	FFlockPurchaseResult Result;
	Result.PurchaseId = TEXT("pur-3");
	Result.ItemType = TEXT("bundle");
	Result.Granted.Add(Reward(TEXT("SWORD"), 1, TEXT("item")));

	FFlockRecordingLogger Log;
	FlockNarratePurchaseResult(Result, Log);

	TestTrue(TEXT("an unrecognised reward kind is printed verbatim"),
		FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("granted 1 x SWORD (type=item)")));
	TestEqual(TEXT("and it is not treated as a fault"), Log.Errors.Num(), 0);

	// The default is what makes the common case readable; an explicit kind must override it.
	const FFlockShopItemReward Defaulted = Reward(TEXT("GOLD"), 5);
	TestEqual(TEXT("an omitted type still defaults to currency"),
		Defaulted.Type, FString(FlockShopItemRewardTypes::Currency));

	return true;
}

// ── The payment leg: a real-money item names every store's product id, in store order, or that it has none ──
// Values from the store suite's catalog (FlockStoreTest.cpp), verbatim.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSelfTestNarrationRealMoneyItemTest, "Flock.SelfTest.Narration.RealMoneyItem_NamesEachStoresProductId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSelfTestNarrationRealMoneyItemTest::RunTest(const FString& Parameters)
{
	FFlockShopItem Gems;
	Gems.Id = TEXT("gems");
	Gems.Name = TEXT("gems");
	Gems.Price = 199;
	Gems.Currency = TEXT("USD");
	Gems.IsRealMoney = true;
	Gems.StoreProductIds.Add(TEXT("google_play"), TEXT("com.flock.gems"));
	Gems.StoreProductIds.Add(TEXT("apple_app_store"), TEXT("com.flock.gems.ios"));
	FFlockShopItem Shield = Gems;
	Shield.Id = TEXT("shield");
	Shield.Name = TEXT("shield");
	Shield.StoreProductIds.Reset();

	FFlockRecordingLogger Log;
	FlockNarrateRealMoneyItem(Gems, Log);
	FlockNarrateRealMoneyItem(Shield, Log);
	TestTrue(TEXT("the item's name, id and Flock price"), FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("real-money item 'gems' (gems) 199 USD (in the currency's smallest unit)")));
	TestTrue(TEXT("each store's product id, by store name in order"),
		FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("store product ids: apple_app_store=com.flock.gems.ios, google_play=com.flock.gems")));
	TestTrue(TEXT("an item with none says it is sold through a web checkout only"),
		FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("'shield' (shield) 199 USD (in the currency's smallest unit); store product ids: (none - sold through a web checkout only)")));
	TestEqual(TEXT("nothing is logged as a warning"), Log.Warnings.Num(), 0);
	return true;
}

// ── The payment leg: a setup check names each item to fix and the server's problem with its fix; a web checkout's says how to check ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSelfTestNarrationSetupCheckTest, "Flock.SelfTest.Narration.StoreSetupCheck_NamesWhatToFix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSelfTestNarrationSetupCheckTest::RunTest(const FString& Parameters)
{
	FFlockShopItem Sword;
	Sword.Id = TEXT("sword");
	Sword.Name = TEXT("sword");
	FFlockShopItem Shield;
	Shield.Id = TEXT("shield");
	Shield.Name = TEXT("shield");
	FFlockStoreSetupCheck Store;
	Store.NotFoundInTheStore.Add(Sword);
	Store.MissingProductId.Add(Shield);
	Store.bHasServerProblem = true;
	Store.ServerProblem = FFlockError::Make(EFlockErrorType::Validation,
		TEXT("Google rejected this game's store credentials (service account refused (HTTP 400)); the studio's payment settings need fixing before receipts can be verified"),
		400, FString(), TEXT("payment_provider.store_credentials_rejected"));

	FFlockRecordingLogger Log;
	FlockNarrateStoreSetupCheck(Store, Log);
	TestTrue(TEXT("not ready"), FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("store setup check -> not ready")));
	TestTrue(TEXT("the item the store did not find"), FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("the store did not find 'sword' (sword)")));
	TestTrue(TEXT("the item with no product id"), FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("'shield' (shield) has no product id for this store")));
	TestTrue(TEXT("the server's problem, its cause"), FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("Google rejected this game's store credentials")));
	TestTrue(TEXT("and its fix"), !Store.ServerProblem.Hint.IsEmpty() && FFlockRecordingLogger::AnyContains(Log.Infos, Store.ServerProblem.Hint));
	TestFalse(TEXT("no claim the server can check purchases"), FFlockRecordingLogger::AnyContains(Log.Infos, TEXT("can ask the store about purchases with")));

	FFlockStoreSetupCheck Web;
	Web.NotChecked = TEXT("The payment gateway: buy a real-money item with the gateway's test card.");
	FFlockRecordingLogger WebLog;
	FlockNarrateStoreSetupCheck(Web, WebLog);
	TestTrue(TEXT("a web checkout with nothing found is ready"), FFlockRecordingLogger::AnyContains(WebLog.Infos, TEXT("store setup check -> ready")));
	TestTrue(TEXT("and says how to check the gateway"), FFlockRecordingLogger::AnyContains(WebLog.Infos, TEXT("not checked: The payment gateway: buy a real-money item")));
	TestEqual(TEXT("nothing is logged as a warning"), Log.Warnings.Num() + WebLog.Warnings.Num(), 0);
	return true;
}

#endif // WITH_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
