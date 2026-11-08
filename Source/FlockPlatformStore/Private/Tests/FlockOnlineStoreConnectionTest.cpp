// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockOnlineStoreConnection.h"
#include "Misc/Base64.h"
#include "Misc/FlockEngineCompat.h"
#include "Interfaces/OnlinePurchaseInterface.h"
#include "OnlineSubsystem.h"

// What the online subsystem's store hands over, read the way the engine writes it: its error codes for a checkout
// (OnlinePurchaseGooglePlay.cpp, OnlinePurchaseIOS.cpp, UE 5.4 to 5.8), Google Play's receipt, and the finish per kind.

#define FLOCK_PLATFORM_STORE_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.PlatformStore." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

namespace FlockPlatformStoreTesting
{
	/** Google Play's validation info as the engine writes it: its purchase JSON in Base64 under receiptData. */
	inline FString GoogleValidationInfo(const FString& PurchaseJson)
	{
		return FString::Printf(TEXT("{\"receiptData\":\"%s\",\"signature\":\"sig\"}"), *FBase64::Encode(PurchaseJson));
	}

	/** A receipt as the engine's store lists it: one product, its state, its store id and its validation info. */
	inline FPurchaseReceipt Receipt(EPurchaseTransactionState State, const FString& TransactionId, const FString& ValidationInfo)
	{
		FPurchaseReceipt Out;
		Out.TransactionState = State;
		Out.TransactionId = TransactionId;
		FPurchaseReceipt::FReceiptOfferEntry Offer(FString(), TEXT("com.flock.sword"), 1);
		FPurchaseReceipt::FLineItemInfo Line;
		Line.ValidationInfo = ValidationInfo;
		Offer.LineItems.Add(Line);
		Out.AddReceiptOffer(Offer);
		return Out;
	}

	const FString NotYetAcknowledged = TEXT("{\"orderId\":\"GPA.2\",\"acknowledged\":false}");
	const FString Acknowledged = TEXT("{\"orderId\":\"GPA.1\",\"acknowledged\":true}");
}

using namespace FlockPlatformStoreTesting;

FLOCK_PLATFORM_STORE_TEST(FFlockPlatformStoreReasonsTest, "EachEngineCheckoutFailureReadsAsItsStoreReason")
bool FFlockPlatformStoreReasonsTest::RunTest(const FString& Parameters)
{
	struct FCase { const TCHAR* Code; const TCHAR* Reason; };
	const FCase Cases[] = {
		{ TEXT("com.epicgames.catalog_helper.user_cancelled"), FlockStoreFailureReasons::Cancelled },
		{ TEXT("com.epicgames.purchase.googleplay.invalid[7:ItemAlreadyOwned]"), FlockStoreFailureReasons::AlreadyOwned },
		{ TEXT("com.epicgames.purchase.googleplay.failure[4:ItemUnavailable]"), FlockStoreFailureReasons::ProductUnavailable },
		// A declined card on Google Play arrives as BillingUnavailable, with the engine's suffix.
		{ TEXT("com.epicgames.purchase.googleplay.failure[3:BillingUnavailable].noDelayedRedeemAndError"), FlockStoreFailureReasons::StoreUnavailable },
		{ TEXT("com.epicgames.purchase.googleplay.failure[2:ServiceUnavailable]"), FlockStoreFailureReasons::StoreUnavailable },
		{ TEXT("com.epicgames.purchase.googleplay.failure[-1:ServiceDisconnected]"), FlockStoreFailureReasons::StoreUnavailable },
		{ TEXT("com.epicgames.purchase.googleplay.failure[12:NetworkError]"), FlockStoreFailureReasons::StoreUnavailable },
		{ TEXT("com.epicgames.purchase.googleplay.failure[5:DeveloperError]"), FlockStoreFailureReasons::Unknown },
		{ TEXT("com.epicgames.purchase.failure"), FlockStoreFailureReasons::Unknown },
		{ TEXT(""), FlockStoreFailureReasons::Unknown },
	};
	for (const FCase& Case : Cases)
	{
		TestEqualSensitive(FString::Printf(TEXT("'%s'"), Case.Code), FFlockOnlineStoreConnection::ReasonOf(Case.Code), FString(Case.Reason));
	}
	TestTrue(TEXT("A deferred checkout waits for its payment"), FFlockOnlineStoreConnection::IsWaitingForPayment(TEXT("com.epicgames.purchase.deferred")));
	TestFalse(TEXT("A failed one does not"), FFlockOnlineStoreConnection::IsWaitingForPayment(TEXT("com.epicgames.purchase.googleplay.failure[6:Error]")));
	return true;
}

FLOCK_PLATFORM_STORE_TEST(FFlockPlatformStoreFinishTest, "GooglePlayAcknowledgesAOneTimeProductAndConsumesAConsumable")
bool FFlockPlatformStoreFinishTest::RunTest(const FString& Parameters)
{
	TestEqualSensitive(TEXT("Google Play, one-time: acknowledged (the engine's subscription marker)"),
		FFlockOnlineStoreConnection::FinishInfoFor(FlockPaymentProviders::GooglePlay, /*bConsumable*/ false), FString(TEXT("isSubscription")));
	TestTrue(TEXT("Google Play, consumable: consumed"), FFlockOnlineStoreConnection::FinishInfoFor(FlockPaymentProviders::GooglePlay, true).IsEmpty());
	TestTrue(TEXT("App Store: finished as the engine finishes it"), FFlockOnlineStoreConnection::FinishInfoFor(FlockPaymentProviders::AppleAppStore, false).IsEmpty());
	return true;
}

FLOCK_PLATFORM_STORE_TEST(FFlockPlatformStoreAcknowledgedTest, "AnAcknowledgedGooglePlayReceiptIsKnownSoItIsNotSentAgainAtEveryLaunch")
bool FFlockPlatformStoreAcknowledgedTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Acknowledged"), FFlockOnlineStoreConnection::IsAcknowledged(GoogleValidationInfo(
		TEXT("{\"orderId\":\"GPA.1\",\"productId\":\"sword\",\"purchaseState\":0,\"acknowledged\":true}"))));
	TestFalse(TEXT("Not yet acknowledged"), FFlockOnlineStoreConnection::IsAcknowledged(GoogleValidationInfo(
		TEXT("{\"orderId\":\"GPA.2\",\"productId\":\"potion\",\"purchaseState\":0,\"acknowledged\":false}"))));
	TestFalse(TEXT("No acknowledged member"), FFlockOnlineStoreConnection::IsAcknowledged(GoogleValidationInfo(TEXT("{\"orderId\":\"GPA.3\"}"))));
	TestFalse(TEXT("Empty (a payment still pending)"), FFlockOnlineStoreConnection::IsAcknowledged(FString()));
	TestFalse(TEXT("Not the engine's shape"), FFlockOnlineStoreConnection::IsAcknowledged(TEXT("not json")));
	return true;
}

FLOCK_PLATFORM_STORE_TEST(FFlockPlatformStoreSubsystemsTest, "OnlyTheGooglePlayAndIosSubsystemsAreStores")
bool FFlockPlatformStoreSubsystemsTest::RunTest(const FString& Parameters)
{
	TestEqualSensitive(TEXT("GOOGLEPLAY"), FFlockOnlineStoreConnection::PaymentProviderOf(FName(TEXT("GOOGLEPLAY"))), FString(FlockPaymentProviders::GooglePlay));
	TestEqualSensitive(TEXT("IOS"), FFlockOnlineStoreConnection::PaymentProviderOf(FName(TEXT("IOS"))), FString(FlockPaymentProviders::AppleAppStore));
	TestTrue(TEXT("NULL"), FFlockOnlineStoreConnection::PaymentProviderOf(FName(TEXT("NULL"))).IsEmpty());
	TestTrue(TEXT("STEAM"), FFlockOnlineStoreConnection::PaymentProviderOf(FName(TEXT("STEAM"))).IsEmpty());
	return true;
}

FLOCK_PLATFORM_STORE_TEST(FFlockPlatformStoreNotAStoreHereTest, "ConnectingWhereTheOnlineSubsystemIsNotAStoreIsRefusedWithItsFix")
bool FFlockPlatformStoreNotAStoreHereTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FFlockOnlineStoreConnection> Connection = MakeShared<FFlockOnlineStoreConnection>();
	if (!Connection->GetPaymentProvider().IsEmpty())
	{
		AddInfo(TEXT("This platform's subsystem is a store; the refusal is for one that is not."));
		return true;
	}
	TOptional<FFlockError> Answer;
	Connection->Connect([&Answer](const FFlockError& Error) { Answer = Error; });
	TestTrue(TEXT("Refused"), Answer.IsSet() && Answer->Type == EFlockErrorType::Validation);
	TestTrue(TEXT("Naming the subsystem"), Answer.IsSet() && Answer->Message.Contains(TEXT("online subsystem")));
	TestFalse(TEXT("With a fix"), !Answer.IsSet() || Answer->Hint.IsEmpty());
	return true;
}

FLOCK_PLATFORM_STORE_TEST(FFlockPlatformStoreRegisteredTest, "ThePluginHandsFlockItsStoreLibrary")
bool FFlockPlatformStoreRegisteredTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Registered as the module started"), UFlockStore::HasStoreLibrary());
	const TSharedPtr<IFlockStoreConnection> Made = UFlockStore::HasStoreLibrary() ? UFlockStore::GetStoreLibrary()() : nullptr;
	TestTrue(TEXT("Making the online subsystem's connection"), Made.IsValid());
	return true;
}

FLOCK_PLATFORM_STORE_TEST(FFlockPlatformStoreUnfinishedTest, "OnlyAPaidUnacknowledgedPurchaseIsHandedToFlockAtALaunch")
bool FFlockPlatformStoreUnfinishedTest::RunTest(const FString& Parameters)
{
	const FString Google = FlockPaymentProviders::GooglePlay;
	TestTrue(TEXT("Google Play, paid and not yet acknowledged: sent"),
		FFlockOnlineStoreConnection::IsUnfinished(Receipt(EPurchaseTransactionState::Purchased, TEXT("token-1"), GoogleValidationInfo(NotYetAcknowledged)), Google));
	TestFalse(TEXT("Google Play, acknowledged at an earlier launch: not sent again"),
		FFlockOnlineStoreConnection::IsUnfinished(Receipt(EPurchaseTransactionState::Purchased, TEXT("token-1"), GoogleValidationInfo(Acknowledged)), Google));
	TestFalse(TEXT("Google Play, payment pending (no validation info yet): not sent"),
		FFlockOnlineStoreConnection::IsUnfinished(Receipt(EPurchaseTransactionState::Purchased, TEXT("token-1"), FString()), Google));
	TestFalse(TEXT("Deferred: not sent"),
		FFlockOnlineStoreConnection::IsUnfinished(Receipt(EPurchaseTransactionState::Deferred, TEXT("token-1"), GoogleValidationInfo(NotYetAcknowledged)), Google));
	TestFalse(TEXT("No store id: not sent"),
		FFlockOnlineStoreConnection::IsUnfinished(Receipt(EPurchaseTransactionState::Purchased, FString(), GoogleValidationInfo(NotYetAcknowledged)), Google));
	TestTrue(TEXT("App Store, purchased: sent"),
		FFlockOnlineStoreConnection::IsUnfinished(Receipt(EPurchaseTransactionState::Purchased, TEXT("1000"), TEXT("base64 app receipt")), FlockPaymentProviders::AppleAppStore));
	const FFlockStoreOrder Order = FFlockOnlineStoreConnection::OrderOf(Receipt(EPurchaseTransactionState::Purchased, TEXT("token-1"), FString()));
	TestEqualSensitive(TEXT("The order's product"), Order.ProductId, FString(TEXT("com.flock.sword")));
	TestEqualSensitive(TEXT("The order's store id"), Order.TransactionId, FString(TEXT("token-1")));
	return true;
}

FLOCK_PLATFORM_STORE_TEST(FFlockPlatformStoreOwnedTest, "ARestoreListsOnlyPurchasesTheStoreHasFinished")
bool FFlockPlatformStoreOwnedTest::RunTest(const FString& Parameters)
{
	const FString Google = FlockPaymentProviders::GooglePlay;
	const FString Apple = FlockPaymentProviders::AppleAppStore;
	TestTrue(TEXT("Google Play, acknowledged: owned"),
		FFlockOnlineStoreConnection::IsOwnedAndFinished(Receipt(EPurchaseTransactionState::Purchased, TEXT("token-1"), GoogleValidationInfo(Acknowledged)), Google));
	TestFalse(TEXT("Google Play, not yet acknowledged: still to be finished, sent as unfinished instead"),
		FFlockOnlineStoreConnection::IsOwnedAndFinished(Receipt(EPurchaseTransactionState::Purchased, TEXT("token-1"), GoogleValidationInfo(NotYetAcknowledged)), Google));
	TestTrue(TEXT("App Store, restored: owned"),
		FFlockOnlineStoreConnection::IsOwnedAndFinished(Receipt(EPurchaseTransactionState::Restored, TEXT("1000"), FString()), Apple));
	TestFalse(TEXT("App Store, purchased and unfinished: not owned yet"),
		FFlockOnlineStoreConnection::IsOwnedAndFinished(Receipt(EPurchaseTransactionState::Purchased, TEXT("1000"), FString()), Apple));
	TestFalse(TEXT("Failed: never"),
		FFlockOnlineStoreConnection::IsOwnedAndFinished(Receipt(EPurchaseTransactionState::Failed, TEXT("1000"), FString()), Apple));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
