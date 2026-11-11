// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockOnlineStoreConnection.h"

#include "Dom/JsonObject.h"
#include "Interfaces/OnlinePurchaseInterface.h"
#include "Interfaces/OnlineStoreInterfaceV2.h"
#include "Containers/Ticker.h"
#include "Misc/Base64.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FlockEngineCompat.h"
#include "OnlineError.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogFlockPlatformStore, Log, All);

namespace
{
	const FName GooglePlaySubsystem(TEXT("GOOGLEPLAY"));
	const FName AppStoreSubsystem(TEXT("IOS"));

	/** The marker that makes the engine's Google Play subsystem acknowledge a purchase rather than consume it. */
	const TCHAR* const AcknowledgeMarker = TEXT("isSubscription");

	/** The engine's stores keep the user id they are handed; no platform sign-in is needed to buy. */
	FUniqueNetIdRef Buyer(const IOnlineSubsystem& Subsystem)
	{
		return FUniqueNetIdString::Create(TEXT("flock-store"), Subsystem.GetSubsystemName());
	}

	FString ProductOf(const FPurchaseReceipt& Receipt)
	{
		return Receipt.ReceiptOffers.Num() > 0 ? Receipt.ReceiptOffers[0].OfferId : FString();
	}

	FString ValidationInfoOf(const FPurchaseReceipt& Receipt)
	{
		for (const FPurchaseReceipt::FReceiptOfferEntry& Offer : Receipt.ReceiptOffers)
		{
			for (const FPurchaseReceipt::FLineItemInfo& Line : Offer.LineItems)
			{
				if (!Line.ValidationInfo.IsEmpty())
				{
					return Line.ValidationInfo;
				}
			}
		}
		return FString();
	}
}

FFlockOnlineStoreConnection::~FFlockOnlineStoreConnection()
{
	StopListening();
}

IOnlineSubsystem* FFlockOnlineStoreConnection::Subsystem() const
{
	return IOnlineSubsystem::Get();
}

FString FFlockOnlineStoreConnection::PaymentProviderOf(const FName& SubsystemName)
{
	if (SubsystemName == GooglePlaySubsystem)
	{
		return FlockPaymentProviders::GooglePlay;
	}
	if (SubsystemName == AppStoreSubsystem)
	{
		return FlockPaymentProviders::AppleAppStore;
	}
	return FString();
}

FString FFlockOnlineStoreConnection::GetPaymentProvider() const
{
	const IOnlineSubsystem* Online = Subsystem();
	return Online != nullptr ? PaymentProviderOf(Online->GetSubsystemName()) : FString();
}

void FFlockOnlineStoreConnection::Listen(FListener InListener)
{
	Listener = MoveTemp(InListener);
}

void FFlockOnlineStoreConnection::Connect(TFunction<void(const FFlockError& Error)> OnDone)
{
	IOnlineSubsystem* Online = Subsystem();
	const FString Provider = GetPaymentProvider();
	if (Online == nullptr || Provider.IsEmpty())
	{
		OnDone(FFlockError::MakeWithFix(EFlockErrorType::Validation,
			FString::Printf(TEXT("The engine's online subsystem here is %s, not a store Flock pays through, so nothing was connected"),
				Online != nullptr ? *Online->GetSubsystemName().ToString() : TEXT("not running")),
			TEXT("Connect on Android (the GooglePlay online subsystem) or iOS (IOS); both are the engine's defaults there. Check DefaultPlatformService under [OnlineSubsystem] in the platform's Engine.ini if it was changed.")));
		return;
	}
#if PLATFORM_ANDROID && !FLOCK_ENGINE_GOOGLE_PLAY_BILLING_IS_CURRENT
	OnDone(FFlockError::MakeWithFix(EFlockErrorType::Validation,
		TEXT("This engine's Google Play subsystem uses a Billing Library Google no longer takes new builds with, so nothing was connected"),
		TEXT("Build the game with UE 5.8 or later (Billing Library 8), or buy with billing code of the game's own and redeem each purchase with FFlockShopProvider::RedeemStoreReceipt.")));
	return;
#else
	if (!Online->GetPurchaseInterface().IsValid() || !Online->GetStoreV2Interface().IsValid())
	{
		OnDone(FFlockError::MakeWithFix(EFlockErrorType::Validation,
			TEXT("The platform's online subsystem has no store in this build, so nothing was connected"),
			Provider == FlockPaymentProviders::GooglePlay
				? TEXT("Turn on Google Play support (Project Settings > Android > Google Play Services: Enable Google Play Support), add the com.android.vending.BILLING permission and the game's Google Play license key, and keep bSupportsInAppPurchasing=true under [OnlineSubsystemGooglePlay.Store].")
				: TEXT("Turn on StoreKit support (Project Settings > iOS > Online: Enable StoreKit Support) and add the In-App Purchase capability to the app.")));
		return;
	}
	// The engine's own finishing turned off for the whole game: a purchase Flock delivered would never be acknowledged or
	// consumed, so Google would refund it after three days while the player keeps the item.
	bool bFinishingTurnedOff = false;
	if (Provider == FlockPaymentProviders::GooglePlay && GConfig != nullptr)
	{
		GConfig->GetBool(TEXT("OnlineSubsystemGooglePlay.Store"), TEXT("bDisableLocalAcknowledgeAndConsume"), bFinishingTurnedOff, GEngineIni);
	}
	if (bFinishingTurnedOff)
	{
		OnDone(FFlockError::MakeWithFix(EFlockErrorType::Validation,
			TEXT("bDisableLocalAcknowledgeAndConsume is on, so the engine would finish no purchase Flock delivers, and nothing was connected"),
			TEXT("Remove bDisableLocalAcknowledgeAndConsume=true from [OnlineSubsystemGooglePlay.Store] in the Android Engine.ini: Flock finishes each purchase once it has delivered it.")));
		return;
	}
	bConnected = true;
	bStopped = false;
	// A payment that completes outside a checkout (pending, then paid) arrives with no receipt: ask the store again.
	TWeakPtr<FFlockOnlineStoreConnection> WeakSelf = AsShared();
	UnexpectedPurchaseHandle = Online->GetPurchaseInterface()->AddOnUnexpectedPurchaseReceiptDelegate_Handle(
		FOnUnexpectedPurchaseReceiptDelegate::CreateLambda([WeakSelf](const FUniqueNetId&)
		{
			if (const TSharedPtr<FFlockOnlineStoreConnection> Self = WeakSelf.Pin())
			{
				Self->FetchUnfinishedPurchases();
			}
		}));
	OnDone(FFlockError());
#endif
}

void FFlockOnlineStoreConnection::RegisterProducts(const TArray<FFlockStoreProduct>& Products, TFunction<void()> OnDone)
{
	IOnlineSubsystem* Online = Subsystem();
	const IOnlineStoreV2Ptr Store = Online != nullptr ? Online->GetStoreV2Interface() : nullptr;
	TArray<FUniqueOfferId> OfferIds;
	for (const FFlockStoreProduct& Product : Products)
	{
		ConsumableByProduct.Add(Product.ProductId, Product.bConsumable);
		OfferIds.Add(Product.ProductId);
	}
	if (!Store.IsValid() || OfferIds.Num() == 0)
	{
		OnDone();
		return;
	}
	Store->QueryOffersById(*Buyer(*Online), OfferIds, FOnQueryOnlineStoreOffersComplete::CreateLambda(
		[OnDone, Asked = OfferIds.Num()](bool bSucceeded, const TArray<FUniqueOfferId>& Found, const FString& Error)
		{
			if (!bSucceeded || Found.Num() < Asked)
			{
				UE_LOG(LogFlockPlatformStore, Warning, TEXT("[Flock SDK] The store found %d of Flock's %d products (%s). UFlockStore::CheckSetup lists the ones it did not find."),
					Found.Num(), Asked, Error.IsEmpty() ? TEXT("no reason given") : *Error);
			}
			OnDone();
		}));
}

void FFlockOnlineStoreConnection::Buy(const FString& ProductId)
{
	IOnlineSubsystem* Online = Subsystem();
	const IOnlinePurchasePtr Purchase = Online != nullptr ? Online->GetPurchaseInterface() : nullptr;
	if (!bConnected || !Purchase.IsValid())
	{
		const TFunction<void(const FFlockStoreFailure&)> OnFailed = Listener.OnFailed;
		if (OnFailed)
		{
			OnFailed({ ProductId, FlockStoreFailureReasons::StoreUnavailable, TEXT("the store is not connected") });
		}
		return;
	}
	const bool* Consumable = ConsumableByProduct.Find(ProductId);
	// No account id (Owner-34): the engine hashes it before Google sees it, so Flock would refuse the receipt as another player's.
	FPurchaseCheckoutRequest Request;
	Request.AddPurchaseOffer(FString(), ProductId, 1, Consumable != nullptr && *Consumable);
	TWeakPtr<FFlockOnlineStoreConnection> WeakSelf = AsShared();
	Purchase->Checkout(*Buyer(*Online), Request, FOnPurchaseCheckoutComplete::CreateLambda(
		[WeakSelf, ProductId](const FOnlineError& Result, const TSharedRef<FPurchaseReceipt>& Receipt)
		{
			const TSharedPtr<FFlockOnlineStoreConnection> Self = WeakSelf.Pin();
			if (!Self.IsValid() || Self->bStopped)
			{
				return;
			}
			const FString Code = Result.GetErrorCode();
			// Each callback is copied before it runs: a handler that disconnects the store clears the listener.
			if (Receipt->TransactionState == EPurchaseTransactionState::Deferred || IsWaitingForPayment(Code))
			{
				const TFunction<void(const FFlockStoreOrder&)> OnDeferred = Self->Listener.OnDeferred;
				if (OnDeferred)
				{
					FFlockStoreOrder Waiting{ ProductId, FString() };
					Waiting.bAnswersThePurchaseDialog = true;
					OnDeferred(Waiting);
				}
				return;
			}
			if (Result.bSucceeded && IsPaid(*Receipt) && !Receipt->TransactionId.IsEmpty())
			{
				const TFunction<void(const FFlockStoreOrder&)> OnPaid = Self->Listener.OnPaid;
				if (OnPaid)
				{
					const FString Bought = ProductOf(*Receipt);
					FFlockStoreOrder Paid{ Bought.IsEmpty() ? ProductId : Bought, Receipt->TransactionId };
					Paid.bAnswersThePurchaseDialog = true;
					OnPaid(Paid);
				}
				return;
			}
			const TFunction<void(const FFlockStoreFailure&)> OnFailed = Self->Listener.OnFailed;
			if (OnFailed)
			{
				const FString Reason = Receipt->TransactionState == EPurchaseTransactionState::Canceled ? FString(FlockStoreFailureReasons::Cancelled) : ReasonOf(Code);
				OnFailed({ ProductId, Reason, FString::Printf(TEXT("%s (%s)"), *Result.GetErrorMessage().ToString(), *Code) });
			}
		}));
}

void FFlockOnlineStoreConnection::Finish(const FFlockStoreOrder& Order)
{
	IOnlineSubsystem* Online = Subsystem();
	const IOnlinePurchasePtr Purchase = Online != nullptr ? Online->GetPurchaseInterface() : nullptr;
	if (!Purchase.IsValid())
	{
		return;
	}
	// A product this launch did not register is finished as one-time: acknowledging a consumable leaves it to be finished
	// again, while consuming a one-time item would lose it for good.
	const bool* Consumable = ConsumableByProduct.Find(Order.ProductId);
	Purchase->FinalizePurchase(*Buyer(*Online), Order.TransactionId, FinishInfoFor(GetPaymentProvider(), Consumable != nullptr && *Consumable));
}

void FFlockOnlineStoreConnection::FetchUnfinishedPurchases()
{
	TWeakPtr<FFlockOnlineStoreConnection> WeakSelf = AsShared();
	ReadReceipts(/*bRestore*/ false, [WeakSelf](bool bListed, const FString& Error)
	{
		if (!bListed)
		{
			UE_LOG(LogFlockPlatformStore, Warning, TEXT("[Flock SDK] The store could not list its unfinished purchases (%s); they are asked for again at the next sign-in, and the store hands them back at the next launch."), *Error);
		}
	});
}

void FFlockOnlineStoreConnection::FetchOwnedPurchases(TFunction<void(TFlockResult<TArray<FFlockStoreOrder>>)> OnDone)
{
	TWeakPtr<FFlockOnlineStoreConnection> WeakSelf = AsShared();
	ReadReceipts(/*bRestore*/ true, [WeakSelf, OnDone](bool bListed, const FString& Error)
	{
		const TSharedPtr<FFlockOnlineStoreConnection> Self = WeakSelf.Pin();
		if (!bListed || !Self.IsValid())
		{
			OnDone(TFlockResult<TArray<FFlockStoreOrder>>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Connection,
				FString::Printf(TEXT("The store could not list this account's purchases: %s"), *Error),
				TEXT("Check the device is online and signed in to the store (on iOS, sign in when the App Store asks), then restore again."))));
			return;
		}
		IOnlineSubsystem* Online = Self->Subsystem();
		TArray<FPurchaseReceipt> Receipts;
		if (Online != nullptr && Online->GetPurchaseInterface().IsValid())
		{
			Online->GetPurchaseInterface()->GetReceipts(*Buyer(*Online), Receipts);
		}
		const FString Provider = Self->GetPaymentProvider();
		TArray<FFlockStoreOrder> Owned;
		for (const FPurchaseReceipt& Receipt : Receipts)
		{
			if (IsOwnedAndFinished(Receipt, Provider))
			{
				Owned.Add(OrderOf(Receipt));
			}
		}
		OnDone(TFlockResult<TArray<FFlockStoreOrder>>::Ok(Owned));
	});
}

void FFlockOnlineStoreConnection::ReadReceipts(bool bRestore, TFunction<void(bool bListed, const FString& Error)> OnDone)
{
	IOnlineSubsystem* Online = Subsystem();
	const IOnlinePurchasePtr Purchase = Online != nullptr ? Online->GetPurchaseInterface() : nullptr;
	if (!bConnected || bStopped || !Purchase.IsValid())
	{
		OnDone(false, TEXT("the store is not connected"));
		return;
	}
	if (bReadingReceipts)
	{
		WaitingReads.Emplace(bRestore, MoveTemp(OnDone));
		return;
	}
	bReadingReceipts = true;
	TWeakPtr<FFlockOnlineStoreConnection> WeakSelf = AsShared();
	Purchase->QueryReceipts(*Buyer(*Online), bRestore, FOnQueryReceiptsComplete::CreateLambda([WeakSelf, OnDone](const FOnlineError& Result)
	{
		const TSharedPtr<FFlockOnlineStoreConnection> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			OnDone(false, TEXT("the store connection was stopped"));
			return;
		}
		Self->bReadingReceipts = false;
		if (Self->bStopped)
		{
			OnDone(false, TEXT("the store connection was stopped"));
		}
		else if (!Result.bSucceeded)
		{
			OnDone(false, Result.ToLogString());
		}
		else
		{
			TArray<FPurchaseReceipt> Receipts;
			if (IOnlineSubsystem* Online = Self->Subsystem())
			{
				if (Online->GetPurchaseInterface().IsValid())
				{
					Online->GetPurchaseInterface()->GetReceipts(*Buyer(*Online), Receipts);
				}
			}
			Self->HandUnfinishedOver(Receipts);
			OnDone(true, FString());
		}
		// The next read waits for the next frame: the engine is still inside this read's completion (Google Play clears its
		// busy flag after it returns, and iOS unbinds the delegate a read started now would have set).
		if (Self->WaitingReads.Num() > 0)
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakSelf](float)
			{
				const TSharedPtr<FFlockOnlineStoreConnection> Again = WeakSelf.Pin();
				if (Again.IsValid() && !Again->bReadingReceipts && Again->WaitingReads.Num() > 0)
				{
					TPair<bool, TFunction<void(bool, const FString&)>> Next = MoveTemp(Again->WaitingReads[0]);
					Again->WaitingReads.RemoveAt(0);
					Again->ReadReceipts(Next.Key, MoveTemp(Next.Value));
				}
				return false;
			}));
		}
	}));
}

void FFlockOnlineStoreConnection::HandUnfinishedOver(const TArray<FPurchaseReceipt>& Receipts)
{
	const FString Provider = GetPaymentProvider();
	for (const FPurchaseReceipt& Receipt : Receipts)
	{
		const TFunction<void(const FFlockStoreOrder&)> OnPaid = Listener.OnPaid;
		if (IsUnfinished(Receipt, Provider) && OnPaid)
		{
			OnPaid(OrderOf(Receipt));
		}
	}
}

FFlockStorePrice FFlockOnlineStoreConnection::PriceOf(const FString& ProductId) const
{
	FFlockStorePrice Price;
	IOnlineSubsystem* Online = Subsystem();
	const IOnlineStoreV2Ptr Store = Online != nullptr ? Online->GetStoreV2Interface() : nullptr;
	const TSharedPtr<FOnlineStoreOffer> Offer = Store.IsValid() ? Store->GetOffer(ProductId) : nullptr;
	if (!Offer.IsValid())
	{
		return Price;
	}
	Price.bFoundInTheStore = true;
	Price.Text = Offer->GetDisplayPrice().ToString();
	Price.AmountInSmallestUnit = Offer->NumericPrice;
	Price.CurrencyCode = Offer->CurrencyCode;
	return Price;
}

void FFlockOnlineStoreConnection::StopListening()
{
	bStopped = true;
	// A read waiting for its turn is never sent: each is told the connection stopped.
	TArray<TPair<bool, TFunction<void(bool, const FString&)>>> Waiting = MoveTemp(WaitingReads);
	WaitingReads.Reset();
	for (TPair<bool, TFunction<void(bool, const FString&)>>& Read : Waiting)
	{
		Read.Value(false, TEXT("the store connection was stopped"));
	}
	Listener = FListener();
	if (UnexpectedPurchaseHandle.IsValid())
	{
		if (IOnlineSubsystem* Online = Subsystem())
		{
			if (const IOnlinePurchasePtr Purchase = Online->GetPurchaseInterface())
			{
				Purchase->ClearOnUnexpectedPurchaseReceiptDelegate_Handle(UnexpectedPurchaseHandle);
			}
		}
		UnexpectedPurchaseHandle.Reset();
	}
}

FString FFlockOnlineStoreConnection::ReasonOf(const FString& EngineErrorCode)
{
	auto Has = [&EngineErrorCode](const TCHAR* Part) { return EngineErrorCode.Contains(Part, ESearchCase::CaseSensitive); };
	if (Has(TEXT("user_cancelled")) || Has(TEXT("UserCancelled")))
	{
		return FlockStoreFailureReasons::Cancelled;
	}
	if (Has(TEXT("ItemAlreadyOwned")))
	{
		return FlockStoreFailureReasons::AlreadyOwned;
	}
	if (Has(TEXT("ItemUnavailable")))
	{
		return FlockStoreFailureReasons::ProductUnavailable;
	}
	// Google Play answers a declined card with BILLING_UNAVAILABLE too, which is why StoreUnavailable's fix names the payment method.
	if (Has(TEXT("BillingUnavailable")) || Has(TEXT("ServiceUnavailable")) || Has(TEXT("ServiceDisconnected")) || Has(TEXT("NetworkError")))
	{
		return FlockStoreFailureReasons::StoreUnavailable;
	}
	return FlockStoreFailureReasons::Unknown;
}

bool FFlockOnlineStoreConnection::IsWaitingForPayment(const FString& EngineErrorCode)
{
	return EngineErrorCode.Contains(TEXT("com.epicgames.purchase.deferred"), ESearchCase::CaseSensitive);
}

FString FFlockOnlineStoreConnection::FinishInfoFor(const FString& PaymentProvider, bool bConsumable)
{
	return PaymentProvider.Equals(FlockPaymentProviders::GooglePlay, ESearchCase::CaseSensitive) && !bConsumable
		? FString(AcknowledgeMarker)
		: FString();
}

bool FFlockOnlineStoreConnection::IsAcknowledged(const FString& ValidationInfo)
{
	TSharedPtr<FJsonObject> Info;
	FString ReceiptData;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ValidationInfo), Info) || !Info.IsValid()
		|| !Info->TryGetStringField(TEXT("receiptData"), ReceiptData))
	{
		return false;
	}
	FString PurchaseJson;
	if (!FBase64::Decode(ReceiptData, PurchaseJson))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Purchase;
	bool bAcknowledged = false;
	return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PurchaseJson), Purchase) && Purchase.IsValid()
		&& Purchase->TryGetBoolField(TEXT("acknowledged"), bAcknowledged) && bAcknowledged;
}

bool FFlockOnlineStoreConnection::IsUnfinished(const FPurchaseReceipt& Receipt, const FString& PaymentProvider)
{
	if (Receipt.TransactionState != EPurchaseTransactionState::Purchased || Receipt.TransactionId.IsEmpty())
	{
		return false;
	}
	if (!PaymentProvider.Equals(FlockPaymentProviders::GooglePlay, ESearchCase::CaseSensitive))
	{
		return true;
	}
	// A payment still pending has no validation info yet; an acknowledged one-time item was finished at an earlier launch,
	// and the engine hands it back at every launch.
	const FString Info = ValidationInfoOf(Receipt);
	return !Info.IsEmpty() && !IsAcknowledged(Info);
}

bool FFlockOnlineStoreConnection::IsOwnedAndFinished(const FPurchaseReceipt& Receipt, const FString& PaymentProvider)
{
	if (!IsPaid(Receipt) || Receipt.TransactionId.IsEmpty())
	{
		return false;
	}
	// On Google Play the acknowledged ones (a consumed purchase is not listed at all); on the App Store the restored ones,
	// which it finishes by itself.
	return PaymentProvider.Equals(FlockPaymentProviders::GooglePlay, ESearchCase::CaseSensitive)
		? IsAcknowledged(ValidationInfoOf(Receipt))
		: Receipt.TransactionState == EPurchaseTransactionState::Restored;
}

FFlockStoreOrder FFlockOnlineStoreConnection::OrderOf(const FPurchaseReceipt& Receipt)
{
	return { ProductOf(Receipt), Receipt.TransactionId };
}

bool FFlockOnlineStoreConnection::IsPaid(const FPurchaseReceipt& Receipt)
{
	return Receipt.TransactionState == EPurchaseTransactionState::Purchased || Receipt.TransactionState == EPurchaseTransactionState::Restored;
}
