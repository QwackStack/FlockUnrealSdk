// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockStoreAsyncActions.h"
#include "Blueprint/FlockBlueprintCallOrigin.h"
#include "Blueprint/FlockBlueprintNodeLife.h"
#include "Blueprint/FlockBlueprintShop.h"
#include "Http/FlockProviderBase.h"

#include "FlockSubsystem.h"
#include "Providers/FlockShopProvider.h"

// ─────────────────────────────── Every store node ───────────────────────────

void UFlockStoreAsyncAction::Activate()
{
	ActivateWithFlockForTesting(UFlockSubsystem::Get(Context.Get()));
}

void UFlockStoreAsyncAction::ActivateWithFlockForTesting(UFlockSubsystem* Flock)
{
	// Kept by the game instance until its pin fires (Finish lets it go), so a call that waits still answers when the Blueprint
	// that started it is gone.
	RegisterWithGameInstance(FlockBlueprintNodeLife::GameInstanceOf(Context.Get()));
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(Context.Get()));
	Start(Flock);
}

// ─────────────────────────────── Connect Store ──────────────────────────────

UFlockConnectStoreAction* UFlockConnectStoreAction::ConnectStore(UObject* WorldContextObject, const FFlockStoreOptions& Options)
{
	UFlockConnectStoreAction* Action = Make<UFlockConnectStoreAction>(WorldContextObject);
	Action->Options = Options;
	return Action;
}

void UFlockConnectStoreAction::Start(UFlockSubsystem* Flock)
{
	if (Flock == nullptr)
	{
		Finish(TFlockResult<UFlockStore*>::Fail(FlockBlueprintShop::NoFlock()), OnSuccess, OnFailure);
		return;
	}
	TWeakObjectPtr<UFlockConnectStoreAction> WeakThis(this);
	UFlockStore::Connect(Flock, Options, [WeakThis](TFlockResult<UFlockStore*> Result)
	{
		if (UFlockConnectStoreAction* Self = WeakThis.Get())
		{
			Self->Finish(Result, Self->OnSuccess, Self->OnFailure);
		}
	});
}

// ────────────────────────────── Restore Purchases ───────────────────────────

UFlockRestorePurchasesAction* UFlockRestorePurchasesAction::RestorePurchases(UObject* WorldContextObject)
{
	UFlockRestorePurchasesAction* Action = Make<UFlockRestorePurchasesAction>(WorldContextObject);
	return Action;
}

void UFlockRestorePurchasesAction::Start(UFlockSubsystem* Flock)
{
	FFlockError Error;
	UFlockStore* Store = FlockBlueprintShop::StoreOf(Flock, Error);
	if (Store == nullptr)
	{
		Finish(TFlockResult<FFlockStoreRestore>::Fail(Error), OnSuccess, OnFailure);
		return;
	}
	TWeakObjectPtr<UFlockRestorePurchasesAction> WeakThis(this);
	Store->Restore([WeakThis](TFlockResult<FFlockStoreRestore> Result)
	{
		if (UFlockRestorePurchasesAction* Self = WeakThis.Get())
		{
			Self->Finish(Result, Self->OnSuccess, Self->OnFailure);
		}
	});
}

// ───────────────────────────── Check Store Setup ────────────────────────────

UFlockCheckStoreSetupAction* UFlockCheckStoreSetupAction::CheckStoreSetup(UObject* WorldContextObject)
{
	UFlockCheckStoreSetupAction* Action = Make<UFlockCheckStoreSetupAction>(WorldContextObject);
	return Action;
}

void UFlockCheckStoreSetupAction::Start(UFlockSubsystem* Flock)
{
	FFlockError Error;
	UFlockStore* Store = FlockBlueprintShop::StoreOf(Flock, Error);
	if (Store == nullptr)
	{
		Finish(TFlockResult<FFlockStoreSetupCheck>::Fail(Error), OnSuccess, OnFailure);
		return;
	}
	TWeakObjectPtr<UFlockCheckStoreSetupAction> WeakThis(this);
	Store->CheckSetup([WeakThis](TFlockResult<FFlockStoreSetupCheck> Result)
	{
		if (UFlockCheckStoreSetupAction* Self = WeakThis.Get())
		{
			Self->Finish(Result, Self->OnSuccess, Self->OnFailure);
		}
	});
}

// ─────────────────────────────── Start Checkout ─────────────────────────────

UFlockStartCheckoutAction* UFlockStartCheckoutAction::StartCheckout(UObject* WorldContextObject, const FString& ShopItemId,
	const FString& PaymentProvider, const FString& ReturnUrl, const FString& CancelUrl)
{
	UFlockStartCheckoutAction* Action = Make<UFlockStartCheckoutAction>(WorldContextObject);
	Action->ShopItemId = ShopItemId;
	Action->PaymentProvider = PaymentProvider;
	Action->ReturnUrl = ReturnUrl;
	Action->CancelUrl = CancelUrl;
	return Action;
}

void UFlockStartCheckoutAction::Start(UFlockSubsystem* Flock)
{
	FFlockError Error;
	FFlockShopProvider* Shop = FlockBlueprintShop::ShopOf(Flock, Error);
	if (Shop == nullptr)
	{
		Finish(TFlockResult<FFlockCheckout>::Fail(Error), OnSuccess, OnFailure);
		return;
	}
	TWeakObjectPtr<UFlockStartCheckoutAction> WeakThis(this);
	Shop->StartCheckout(ShopItemId, PaymentProvider, ReturnUrl, CancelUrl, [WeakThis](TFlockResult<FFlockCheckout> Result)
	{
		if (UFlockStartCheckoutAction* Self = WeakThis.Get())
		{
			Self->Finish(Result, Self->OnSuccess, Self->OnFailure);
		}
	});
}

// ───────────────────────────── Get Player Purchase ───────────────────────────

UFlockGetPlayerPurchaseAction* UFlockGetPlayerPurchaseAction::GetPlayerPurchase(UObject* WorldContextObject, const FString& PurchaseId)
{
	UFlockGetPlayerPurchaseAction* Action = Make<UFlockGetPlayerPurchaseAction>(WorldContextObject);
	Action->PurchaseId = PurchaseId;
	return Action;
}

void UFlockGetPlayerPurchaseAction::Start(UFlockSubsystem* Flock)
{
	FFlockError Error;
	FFlockShopProvider* Shop = FlockBlueprintShop::ShopOf(Flock, Error);
	if (Shop == nullptr)
	{
		Finish(TFlockResult<FFlockPlayerPurchase>::Fail(Error), OnSuccess, OnFailure);
		return;
	}
	TWeakObjectPtr<UFlockGetPlayerPurchaseAction> WeakThis(this);
	Shop->GetPurchase(PurchaseId, [WeakThis](TFlockResult<FFlockPlayerPurchase> Result)
	{
		if (UFlockGetPlayerPurchaseAction* Self = WeakThis.Get())
		{
			Self->Finish(Result, Self->OnSuccess, Self->OnFailure);
		}
	});
}

// ──────────────────────────── Get Player Purchases ───────────────────────────

UFlockGetPlayerPurchasesAction* UFlockGetPlayerPurchasesAction::GetPlayerPurchases(UObject* WorldContextObject, const FString& Status, int32 Page, int32 Limit)
{
	UFlockGetPlayerPurchasesAction* Action = Make<UFlockGetPlayerPurchasesAction>(WorldContextObject);
	Action->Status = Status;
	Action->Page = Page;
	Action->Limit = Limit;
	return Action;
}

void UFlockGetPlayerPurchasesAction::Start(UFlockSubsystem* Flock)
{
	FFlockError Error;
	FFlockShopProvider* Shop = FlockBlueprintShop::ShopOf(Flock, Error);
	if (Shop == nullptr)
	{
		Finish(TFlockResult<FFlockPlayerPurchasePage>::Fail(Error), OnSuccess, OnFailure);
		return;
	}
	TWeakObjectPtr<UFlockGetPlayerPurchasesAction> WeakThis(this);
	Shop->GetPurchases(Status, Page, Limit, [WeakThis](TFlockResult<FFlockPlayerPurchasePage> Result)
	{
		if (UFlockGetPlayerPurchasesAction* Self = WeakThis.Get())
		{
			Self->Finish(Result, Self->OnSuccess, Self->OnFailure);
		}
	});
}


// ──────────────────────────── Redeem Store Receipt ───────────────────────────

UFlockRedeemStoreReceiptAction* UFlockRedeemStoreReceiptAction::RedeemStoreReceipt(UObject* WorldContextObject, const FString& ShopItemId,
	const FFlockStoreReceipt& Receipt)
{
	UFlockRedeemStoreReceiptAction* Action = Make<UFlockRedeemStoreReceiptAction>(WorldContextObject);
	Action->ShopItemId = ShopItemId;
	Action->Receipt = Receipt;
	return Action;
}

void UFlockRedeemStoreReceiptAction::Start(UFlockSubsystem* Flock)
{
	FFlockError Error;
	FFlockShopProvider* Shop = FlockBlueprintShop::ShopOf(Flock, Error);
	if (Shop == nullptr)
	{
		Finish(TFlockResult<FFlockPurchaseResult>::Fail(Error), OnSuccess, OnFailure);
		return;
	}
	TWeakObjectPtr<UFlockRedeemStoreReceiptAction> WeakThis(this);
	Shop->RedeemStoreReceipt(ShopItemId, Receipt, [WeakThis](TFlockResult<FFlockPurchaseResult> Result)
	{
		if (UFlockRedeemStoreReceiptAction* Self = WeakThis.Get())
		{
			Self->Finish(Result, Self->OnSuccess, Self->OnFailure);
		}
	});
}
