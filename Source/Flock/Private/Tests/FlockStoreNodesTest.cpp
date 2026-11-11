// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Blueprint/FlockShopAsyncActions.h"
#include "Blueprint/FlockStoreAsyncActions.h"
#include "Blueprint/FlockStoreLibrary.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "FlockEvents.h"
#include "FlockSubsystem.h"
#include "Http/FlockErrorHints.h"
#include "Store/FlockStore.h"
#include "Tests/Support/FlockStoreNodeTestListener.h"
#include "Tests/Support/FlockTestSpelling.h"
#include "UObject/UnrealType.h"

// The store's Blueprint nodes without a store behind them: every node with no Flock, the failure-reason nodes against the
// spellings Flock uses, the setup check's verdict, and the nodes finding the running game's Flock (under -game). The nodes over
// a store and over a web checkout are tested beside their fixtures, in FlockStoreTest.cpp and FlockWebCheckoutTest.cpp.

#define FLOCK_STORE_NODES_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Store.Nodes." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

FLOCK_STORE_NODES_TEST(FFlockStoreNodesNoFlockTest, "EveryNodeWithoutFlockFiresItsFailurePinSayingHowToInitializeIt")
bool FFlockStoreNodesNoFlockTest::RunTest(const FString& Parameters)
{
	// One listener per node: each node fires its own failure pin once, with the fix for no Flock.
	TArray<TPair<FString, TSharedRef<FFlockStoreNodes>>> Each;
	auto Node = [&Each](const TCHAR* Name) -> FFlockStoreNodes&
	{
		Each.Emplace(Name, MakeShared<FFlockStoreNodes>());
		return *Each.Last().Value;
	};
	Node(TEXT("Connect Store")).Connect(UFlockConnectStoreAction::ConnectStore(nullptr, FFlockStoreOptions()))->ActivateWithFlockForTesting(nullptr);
	Node(TEXT("Restore Purchases")).Restore(UFlockRestorePurchasesAction::RestorePurchases(nullptr))->ActivateWithFlockForTesting(nullptr);
	Node(TEXT("Check Store Setup")).Setup(UFlockCheckStoreSetupAction::CheckStoreSetup(nullptr))->ActivateWithFlockForTesting(nullptr);
	Node(TEXT("Start Checkout")).Checkout(UFlockStartCheckoutAction::StartCheckout(nullptr, TEXT("sword"), FString(), FString(), FString()))->ActivateWithFlockForTesting(nullptr);
	Node(TEXT("Get Player Purchase")).Read(UFlockGetPlayerPurchaseAction::GetPlayerPurchase(nullptr, TEXT("pur-1")))->ActivateWithFlockForTesting(nullptr);
	Node(TEXT("Get Player Purchases")).List(UFlockGetPlayerPurchasesAction::GetPlayerPurchases(nullptr, FString(), 1, 100))->ActivateWithFlockForTesting(nullptr);
	Node(TEXT("Redeem Store Receipt")).Redeem(UFlockRedeemStoreReceiptAction::RedeemStoreReceipt(nullptr, TEXT("gems"),
		UFlockStoreLibrary::MakeGooglePlayReceipt(TEXT("com.flock.gems"), TEXT("token-1"))))->ActivateWithFlockForTesting(nullptr);
	Node(TEXT("Purchase")).Buy(UFlockPurchaseAction::Purchase(nullptr, TEXT("sword"), FString()))->ActivateWithFlockForTesting(nullptr);
	for (const TPair<FString, TSharedRef<FFlockStoreNodes>>& Entry : Each)
	{
		const FFlockStoreNodes& Nodes = *Entry.Value;
		TestEqualSensitive(Entry.Key + TEXT(": its failure pin, once"), Nodes->FiredText(), FString(TEXT("failure")));
		TestEqualSensitive(Entry.Key + TEXT(": says how to initialize Flock"), Nodes->Error.Hint, FFlockErrorHints::InitializeFlockFirst());
	}

	FFlockStorePrice Price;
	FFlockError Error;
	TestFalse(TEXT("Get Store Price: false"), UFlockStoreLibrary::GetStorePriceWithFlockForTesting(nullptr, TEXT("sword"), Price, Error));
	TestEqualSensitive(TEXT("Saying how to initialize Flock"), Error.Hint, FFlockErrorHints::InitializeFlockFirst());

	// A World Context outside any game's world finds no Flock: the nodes' own lookup, with the same answers.
	FFlockStoreNodes Lookup;
	Lookup.Restore(UFlockRestorePurchasesAction::RestorePurchases(nullptr))->Activate();
	TestEqualSensitive(TEXT("No World Context: the failure pin"), Lookup->FiredText(), FString(TEXT("failure")));
	TestEqualSensitive(TEXT("Saying how to initialize Flock"), Lookup->Error.Hint, FFlockErrorHints::InitializeFlockFirst());
	TestTrue(TEXT("Get Store: none"), UFlockStoreLibrary::GetStore(nullptr) == nullptr);
	UFlockStoreLibrary::DisconnectStore(nullptr);
	return true;
}

FLOCK_STORE_NODES_TEST(FFlockStoreNodesReasonsTest, "TheFailureReasonNodesSpellEachReasonAsFlockDoes")
bool FFlockStoreNodesReasonsTest::RunTest(const FString& Parameters)
{
	using L = UFlockStoreLibrary;
	const TArray<TPair<FString, const TCHAR*>> Pairs = {
		{ L::StoreFailureReasonCancelled(), FlockStoreFailureReasons::Cancelled },
		{ L::StoreFailureReasonPaymentDeclined(), FlockStoreFailureReasons::PaymentDeclined },
		{ L::StoreFailureReasonAlreadyOwned(), FlockStoreFailureReasons::AlreadyOwned },
		{ L::StoreFailureReasonAlreadyPending(), FlockStoreFailureReasons::AlreadyPending },
		{ L::StoreFailureReasonProductUnavailable(), FlockStoreFailureReasons::ProductUnavailable },
		{ L::StoreFailureReasonStoreUnavailable(), FlockStoreFailureReasons::StoreUnavailable },
		{ L::StoreFailureReasonNotSignedInToTheStore(), FlockStoreFailureReasons::NotSignedInToTheStore },
		{ L::StoreFailureReasonExpired(), FlockStoreFailureReasons::Expired },
		{ L::StoreFailureReasonUnknown(), FlockStoreFailureReasons::Unknown },
	};
	for (const TPair<FString, const TCHAR*>& Pair : Pairs)
	{
		TestEqualSensitive(Pair.Value, Pair.Key, FString(Pair.Value));
	}
	// One node per reason: the count of the library's string nodes, so a node added without its line here fails.
	int32 StringNodes = 0;
	for (TFieldIterator<UFunction> It(UFlockStoreLibrary::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		StringNodes += CastField<FStrProperty>((*It)->GetReturnProperty()) != nullptr ? 1 : 0;
	}
	TestEqual(TEXT("every string node checked here"), StringNodes, Pairs.Num());
	return true;
}

FLOCK_STORE_NODES_TEST(FFlockStoreNodesReceiptsTest, "TheReceiptNodesMakeEachStoresReceiptAsFlockSpellsIt")
bool FFlockStoreNodesReceiptsTest::RunTest(const FString& Parameters)
{
	const FFlockStoreReceipt Apple = UFlockStoreLibrary::MakeAppStoreReceipt(TEXT("2000000123456789"));
	TestEqualSensitive(TEXT("App Store: the store"), Apple.Store, FString(FlockPaymentProviders::AppleAppStore));
	TestEqualSensitive(TEXT("App Store: the transaction id, exactly"), Apple.TransactionId, FString(TEXT("2000000123456789")));
	TestTrue(TEXT("App Store: nothing else"), Apple.ProductId.IsEmpty() && Apple.PurchaseToken.IsEmpty());
	const FFlockStoreReceipt Google = UFlockStoreLibrary::MakeGooglePlayReceipt(TEXT("com.flock.Gems"), TEXT(" token "));
	TestEqualSensitive(TEXT("Google Play: the store"), Google.Store, FString(FlockPaymentProviders::GooglePlay));
	TestEqualSensitive(TEXT("Google Play: the product id, letter case kept"), Google.ProductId, FString(TEXT("com.flock.Gems")));
	TestEqualSensitive(TEXT("Google Play: the token, never trimmed"), Google.PurchaseToken, FString(TEXT(" token ")));
	TestTrue(TEXT("Google Play: no transaction id"), Google.TransactionId.IsEmpty());
	return true;
}

FLOCK_STORE_NODES_TEST(FFlockStoreNodesPlayerPurchaseTest, "IsPlayerPurchasePaidAndPendingReadTheStatusLetterForLetter")
bool FFlockStoreNodesPlayerPurchaseTest::RunTest(const FString& Parameters)
{
	FFlockPlayerPurchase Purchase;
	Purchase.Status = FlockPurchaseStatuses::Paid;
	TestTrue(TEXT("paid: paid"), UFlockStoreLibrary::IsPlayerPurchasePaid(Purchase));
	TestFalse(TEXT("paid: not pending"), UFlockStoreLibrary::IsPlayerPurchasePending(Purchase));
	Purchase.Status = FlockPurchaseStatuses::Pending;
	TestTrue(TEXT("pending: pending"), UFlockStoreLibrary::IsPlayerPurchasePending(Purchase));
	TestFalse(TEXT("pending: not paid"), UFlockStoreLibrary::IsPlayerPurchasePaid(Purchase));
	for (const TCHAR* Ended : { FlockPurchaseStatuses::Failed, FlockPurchaseStatuses::Expired, FlockPurchaseStatuses::Refunded })
	{
		Purchase.Status = Ended;
		TestFalse(FString(Ended) + TEXT(": neither"), UFlockStoreLibrary::IsPlayerPurchasePaid(Purchase) || UFlockStoreLibrary::IsPlayerPurchasePending(Purchase));
	}
	Purchase.Status = TEXT("Paid");
	TestFalse(TEXT("another spelling is not paid"), UFlockStoreLibrary::IsPlayerPurchasePaid(Purchase));
	return true;
}

FLOCK_STORE_NODES_TEST(FFlockStoreNodesReadyTest, "IsStoreReadyIsTheSetupChecksOwnVerdict")
bool FFlockStoreNodesReadyTest::RunTest(const FString& Parameters)
{
	FFlockStoreSetupCheck Check;
	Check.bServerCanCheckPurchases = true;
	TestTrue(TEXT("Nothing to fix: ready"), UFlockStoreLibrary::IsStoreReady(Check));
	FFlockStoreSetupCheck Missing = Check;
	Missing.MissingProductId.AddDefaulted();
	TestFalse(TEXT("An item with no product id: not ready"), UFlockStoreLibrary::IsStoreReady(Missing));
	FFlockStoreSetupCheck NotFound = Check;
	NotFound.NotFoundInTheStore.AddDefaulted();
	TestFalse(TEXT("An item the store did not find: not ready"), UFlockStoreLibrary::IsStoreReady(NotFound));
	FFlockStoreSetupCheck CannotAsk = Check;
	CannotAsk.bServerCanCheckPurchases = false;
	TestFalse(TEXT("Flock's server cannot ask the store: not ready"), UFlockStoreLibrary::IsStoreReady(CannotAsk));

	// A web checkout: the gateway cannot be checked without a checkout, so nothing found to fix is ready.
	FFlockStoreSetupCheck Web;
	Web.NotChecked = TEXT("The payment gateway: buy with its test keys.");
	TestTrue(TEXT("web checkout, nothing found: ready"), UFlockStoreLibrary::IsStoreReady(Web));
	Web.NotFoundInTheStore.AddDefaulted();
	TestFalse(TEXT("web checkout, something found: not ready"), UFlockStoreLibrary::IsStoreReady(Web));
	return true;
}

FLOCK_STORE_NODES_TEST(FFlockStoreNodesRunningGameTest, "TheNodesFindTheRunningGamesFlock")
bool FFlockStoreNodesRunningGameTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::Game && Context.World() != nullptr)
		{
			World = Context.World();
		}
	}
	UFlockSubsystem* Flock = World != nullptr ? UFlockSubsystem::Get(World) : nullptr;
	if (Flock == nullptr || Flock->GetShopProvider() == nullptr)
	{
		AddInfo(TEXT("No running game with Flock here (the editor's pass): the nodes' own lookup runs under -game."));
		return true;
	}
	if (Flock->GetStore() != nullptr)
	{
		AddInfo(TEXT("The running game has a store connected: its refusals with none are not checked here."));
		return true;
	}
	// With no store connected, each answers from the running game's own Flock without reaching any server.
	TestTrue(TEXT("Get Store: none"), UFlockStoreLibrary::GetStore(World) == nullptr);
	FFlockStorePrice Price;
	FFlockError Error;
	TestFalse(TEXT("Get Store Price: false"), UFlockStoreLibrary::GetStorePrice(World, TEXT("sword"), Price, Error));
	TestEqualSensitive(TEXT("Flock found, no store connected"), Error.Hint, FFlockErrorHints::ConnectAStoreFirst());
	FFlockStoreNodes Nodes;
	Nodes.Restore(UFlockRestorePurchasesAction::RestorePurchases(World))->Activate();
	Nodes.Setup(UFlockCheckStoreSetupAction::CheckStoreSetup(World))->Activate();
	TestEqualSensitive(TEXT("Restore and Check Setup: their failure pins"), Nodes->FiredText(), FString(TEXT("failure,failure")));
	TestEqualSensitive(TEXT("Flock found, no store connected"), Nodes->Error.Hint, FFlockErrorHints::ConnectAStoreFirst());
	UFlockStoreLibrary::DisconnectStore(World);

	// The purchase node finds it too: an empty item id is refused by the running game's own Flock before anything is sent.
	FFlockStoreNodes Buy;
	Buy.Buy(UFlockPurchaseAction::Purchase(World, FString(), FString()))->Activate();
	TestEqualSensitive(TEXT("Flock Purchase: the failure pin"), Buy->FiredText(), FString(TEXT("failure")));
	TestTrue(TEXT("Flock's own answer, not that Flock is not running"), Buy->Error.Hint != FFlockErrorHints::InitializeFlockFirst());

	// A Blueprint binds the store's events on the running game's events hub.
	UFlockEvents* Events = Flock->GetEvents();
	if (TestNotNull(TEXT("The running game's events"), Events))
	{
		Buy->Hear(Events);
		TestTrue(TEXT("Bound"), Events->OnStorePurchaseCompleted.IsAlreadyBound(Buy.Listener.Get(), &UFlockStoreNodeTestListener::HeardCompleted));
		Events->OnStorePurchaseCompleted.RemoveAll(Buy.Listener.Get());
		Events->OnStorePurchaseNotDelivered.RemoveAll(Buy.Listener.Get());
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
