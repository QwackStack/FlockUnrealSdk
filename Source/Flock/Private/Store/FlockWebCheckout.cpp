// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Store/FlockWebCheckout.h"

#include "Auth/FlockAuthSession.h"
#include "FlockSubsystem.h"
#include "GenericPlatform/GenericPlatformProcess.h"
#include "HAL/PlatformProcess.h"
#include "Http/FlockErrorHints.h"
#include "Multiplayer/FlockRepeatingCalls.h"
#include "Providers/FlockMultiplayerProvider.h"
#include "Providers/FlockShopProvider.h"

namespace
{
	const TCHAR* const PurchaseOperation = TEXT("Purchase shop item");
	constexpr int32 OpenCheckoutsPageSize = 100;

	FFlockError Ending(EFlockErrorType Type, const FString& Message, const FString& Fix)
	{
		FFlockError Error = FFlockError::MakeWithFix(Type, Message, Fix);
		Error.Operation = PurchaseOperation;
		return Error;
	}
}

FFlockWebCheckout::FFlockWebCheckout(UFlockSubsystem* InFlock, FSettings InSettings,
	TFunction<void(const FFlockShopItem&, const FFlockPurchaseResult&)> InPaid, TFunction<bool(const FString&, FFlockShopItem&)> InFindItem)
	: Flock(InFlock)
	, Settings(MoveTemp(InSettings))
	, Paid(MoveTemp(InPaid))
	, FindItem(MoveTemp(InFindItem))
{
	// Zero, negative, not a number or endless: the default.
	if (!(Settings.CheckEverySeconds > 0.0) || !FMath::IsFinite(Settings.CheckEverySeconds))
	{
		Settings.CheckEverySeconds = DefaultCheckEverySeconds;
	}
}

bool FFlockWebCheckout::IsWebGateway(const FString& PaymentProvider)
{
	return PaymentProvider.Equals(FlockPaymentProviders::Stripe, ESearchCase::CaseSensitive)
		|| PaymentProvider.Equals(FlockPaymentProviders::Tap, ESearchCase::CaseSensitive)
		|| PaymentProvider.Equals(FlockPaymentProviders::Moyasar, ESearchCase::CaseSensitive);
}

FFlockWebCheckout::~FFlockWebCheckout()
{
	for (const auto& Entry : Watches)
	{
		if (Entry.Value->Call.IsValid())
		{
			Entry.Value->Call->Stop();
		}
	}
}

// ── Purchase ──

void FFlockWebCheckout::Purchase(const FFlockShopItem& Item, TFunction<void(TFlockResult<FFlockPurchaseResult>)> OnComplete)
{
	auto Fail = [&OnComplete](const FFlockError& Error)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockPurchaseResult>::Fail(Error));
		}
	};
	UFlockSubsystem* Sdk = Flock.Get();
	FFlockShopProvider* Shop = Sdk != nullptr ? Sdk->GetShopProvider() : nullptr;
	if (bDisposed || Shop == nullptr)
	{
		Fail(Ending(EFlockErrorType::Validation, TEXT("This store connection is disconnected"),
			TEXT("Connect again with UFlockStore::Connect (a shutdown of Flock disconnects it), and use the connection it completes with.")));
		return;
	}
	if (!IsSignedIn())
	{
		Fail(Ending(EFlockErrorType::Auth, TEXT("No player is signed in"), FFlockErrorHints::SignInFirst()));
		return;
	}
	if (Buying.Contains(Item.Id))
	{
		Fail(Ending(EFlockErrorType::Validation, TEXT("This item is already being bought"),
			TEXT("Wait for the first Purchase of it to complete before starting another; its checkout's payment page is open in the player's browser.")));
		return;
	}
	const int32 SignIn = CurrentSignIn();
	const int32 Number = ++BuyingCount;
	Buying.Add(Item.Id, Number);
	TWeakPtr<FFlockWebCheckout> WeakSelf = AsShared();
	const FString ItemId = Item.Id;
	// Answers the call once and lets the item be bought again, whatever ends it (only this call's hold is released).
	TFunction<void(TFlockResult<FFlockPurchaseResult>)> Done = [WeakSelf, ItemId, Number, OnComplete](TFlockResult<FFlockPurchaseResult> Result)
	{
		if (const TSharedPtr<FFlockWebCheckout> Self = WeakSelf.Pin())
		{
			Self->ReleaseBuying(ItemId, Number);
		}
		if (OnComplete)
		{
			OnComplete(Result);
		}
	};

	Shop->StartCheckout(Item.Id, Settings.Gateway, Settings.ReturnUrl, Settings.CancelUrl,
		[WeakSelf, Item, SignIn, Number, Done](TFlockResult<FFlockCheckout> Opened)
	{
		const TSharedPtr<FFlockWebCheckout> Self = WeakSelf.Pin();
		if (!Opened.IsSuccess())
		{
			Done(TFlockResult<FFlockPurchaseResult>::Fail(Opened.Error));
			return;
		}
		// Opened for a sign-in that ended while it was sent, or a connection gone: its page is never shown, and the server
		// closes it unpaid.
		if (!Self.IsValid() || Self->bDisposed || Self->CurrentSignIn() != SignIn || !Self->IsSignedIn())
		{
			Done(TFlockResult<FFlockPurchaseResult>::Fail(Ending(EFlockErrorType::Cancelled,
				TEXT("The player signed out or changed, or the store was disconnected, while the checkout was opened, so its payment page was not shown"),
				TEXT("Nothing was charged: the checkout closes unpaid by itself. Buy again as the player now signed in, if they want it."))));
			return;
		}
		const TSharedRef<FWatch> Watch = Self->WatchCheckout(Opened.Value.PurchaseId, Item.Id, &Item, /*bOpenedHere*/ true);
		Watch->OnDone = Done;
		Watch->BuyingNumber = Number;
		FString WhyNot;
		if (!Self->OpenPaymentPage(Opened.Value.PaymentUrl, WhyNot))
		{
			// The call ends here; the checkout is still read in case the page opened after all, and reported once paid.
			if (const TFunction<void(TFlockResult<FFlockPurchaseResult>)> Answer = Self->Take(Watch))
			{
				Answer(TFlockResult<FFlockPurchaseResult>::Fail(Ending(EFlockErrorType::Validation,
					FString::Printf(TEXT("The payment page could not be opened in the player's browser (%s)"), *WhyNot),
					TEXT("Set FFlockStoreOptions::OpenPaymentPage to show the payment address another way. The checkout is still read: if the player pays it, OnPurchaseCompleted reports it."))));
			}
		}
	});
}

bool FFlockWebCheckout::OpenPaymentPage(const FString& PaymentUrl, FString& OutWhyNot) const
{
	if (Settings.OpenPaymentPage)
	{
		Settings.OpenPaymentPage(PaymentUrl);
		return true;
	}
	FString Error;
	FPlatformProcess::LaunchURL(*PaymentUrl, nullptr, &Error);
	if (!Error.IsEmpty())
	{
		OutWhyNot = Error;
		return false;
	}
	return true;
}

// ── Reading a checkout ──

TSharedRef<FFlockWebCheckout::FWatch> FFlockWebCheckout::WatchCheckout(const FString& PurchaseId, const FString& ShopItemId,
	const FFlockShopItem* Item, bool bOpenedHere)
{
	// One read per checkout: a purchase call and a sign-in's search that find the same one share it.
	if (const TSharedRef<FWatch>* Watching = Watches.Find(PurchaseId))
	{
		if ((*Watching)->Call.IsValid() && (*Watching)->Call->IsRunning())
		{
			if (!(*Watching)->bHasItem && Item != nullptr)
			{
				(*Watching)->bHasItem = true;
				(*Watching)->Item = *Item;
			}
			return *Watching;
		}
	}
	const TSharedRef<FWatch> Watch = MakeShared<FWatch>();
	Watch->PurchaseId = PurchaseId;
	Watch->ShopItemId = ShopItemId;
	Watch->bHasItem = Item != nullptr;
	if (Item != nullptr)
	{
		Watch->Item = *Item;
	}
	Watch->SignIn = CurrentSignIn();
	Watch->OpenedAt = bOpenedHere ? Now() : -1.0;
	Watches.Add(PurchaseId, Watch);

	UFlockSubsystem* Sdk = Flock.Get();
	FFlockMultiplayerProvider* Calls = Sdk != nullptr ? Sdk->GetMultiplayerProvider() : nullptr;
	if (Calls == nullptr)
	{
		return Watch; // Flock is shutting down: the disconnect that follows answers the caller.
	}
	TWeakPtr<FFlockWebCheckout> WeakSelf = AsShared();
	const TWeakPtr<FWatch> WeakWatch = Watch;
	Watch->Call = Calls->GetRepeatingCalls().Start<FFlockPlayerPurchase>(TEXT("Web checkout ") + PurchaseId, Settings.CheckEverySeconds,
		[WeakSelf, WeakWatch](TFunction<void(TFlockResult<FFlockPlayerPurchase>)> OnAttempt) -> FFlockRequestHandle
		{
			const TSharedPtr<FFlockWebCheckout> Self = WeakSelf.Pin();
			const TSharedPtr<FWatch> Watched = WeakWatch.Pin();
			UFlockSubsystem* Sdk = Self.IsValid() ? Self->Flock.Get() : nullptr;
			FFlockShopProvider* Shop = Sdk != nullptr ? Sdk->GetShopProvider() : nullptr;
			if (Shop == nullptr || !Watched.IsValid())
			{
				OnAttempt(TFlockResult<FFlockPlayerPurchase>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Cancelled,
					TEXT("Flock shut down while the web checkout was read"), TEXT("Connect again once Flock has initialized."))));
				return FFlockRequestHandle();
			}
			// Opened here and still within the time the server takes to save it: a not found means "not saved yet".
			const bool bMayNotBeSavedYet = Watched->OpenedAt >= 0.0 && Self->Now() - Watched->OpenedAt < NewCheckoutSavedWithinSeconds;
			return Shop->ReadPurchase(Watched->PurchaseId, Watched->SignIn, bMayNotBeSavedYet,
				[bMayNotBeSavedYet, OnAttempt](TFlockResult<FFlockPlayerPurchase> Read)
				{
					if (bMayNotBeSavedYet && !Read.IsSuccess() && Read.Error.ErrorCode == EFlockErrorCode::ShopPurchaseNotFound)
					{
						// An empty purchase: read again at the next interval.
						OnAttempt(TFlockResult<FFlockPlayerPurchase>::Ok(FFlockPlayerPurchase()));
						return;
					}
					OnAttempt(Read);
				});
		},
		[WeakSelf, WeakWatch](const FFlockPlayerPurchase& Purchase)
		{
			const TSharedPtr<FFlockWebCheckout> Self = WeakSelf.Pin();
			const TSharedPtr<FWatch> Watched = WeakWatch.Pin();
			if (Self.IsValid() && Watched.IsValid())
			{
				Self->HandleRead(Watched.ToSharedRef(), Purchase);
			}
		},
		[WeakSelf, WeakWatch](const FFlockError& Failure)
		{
			const TSharedPtr<FFlockWebCheckout> Self = WeakSelf.Pin();
			const TSharedPtr<FWatch> Watched = WeakWatch.Pin();
			if (Self.IsValid() && Watched.IsValid())
			{
				Self->HandleStopped(Watched.ToSharedRef(), Failure);
			}
		});
	return Watch;
}

void FFlockWebCheckout::HandleRead(const TSharedRef<FWatch>& Watch, const FFlockPlayerPurchase& Purchase)
{
	// Not saved yet: read again at the next interval.
	if (bDisposed || Purchase.Id.IsEmpty())
	{
		return;
	}
	// Ended only by a status that is final. Pending, and a status this SDK does not know yet (the server may widen the set), are
	// read again at the next interval.
	const bool bFailed = Purchase.Status.Equals(FlockPurchaseStatuses::Failed, ESearchCase::CaseSensitive);
	const bool bExpired = Purchase.Status.Equals(FlockPurchaseStatuses::Expired, ESearchCase::CaseSensitive);
	const bool bRefunded = Purchase.Status.Equals(FlockPurchaseStatuses::Refunded, ESearchCase::CaseSensitive);
	if (!Purchase.IsPaid() && !bFailed && !bExpired && !bRefunded)
	{
		return;
	}
	if (Watch->Call.IsValid())
	{
		Watch->Call->Stop();
	}
	Forget(Watch);
	Ended.Add(Watch->PurchaseId);
	if (Purchase.IsPaid())
	{
		HandlePaid(Watch, Purchase);
		return;
	}
	const FString Reason = bFailed ? FString(FlockStoreFailureReasons::PaymentDeclined)
		: bExpired ? FString(FlockStoreFailureReasons::Expired)
		: FString(FlockStoreFailureReasons::Unknown);
	if (const TFunction<void(TFlockResult<FFlockPurchaseResult>)> Answer = Take(Watch))
	{
		FFlockError Error = Ending(EFlockErrorType::Store,
			FString::Printf(TEXT("The web checkout ended as %s, so nothing was bought"), *Purchase.Status), FixFor(Reason));
		Error.StoreFailureReason = Reason;
		Answer(TFlockResult<FFlockPurchaseResult>::Fail(Error));
	}
}

void FFlockWebCheckout::HandleStopped(const TSharedRef<FWatch>& Watch, const FFlockError& Failure)
{
	// The reads stopped on a failure that is not the checkout's end (a sign-in refused, a proxy's 403, a checkout Flock did not
	// keep): the page may still be paid, so the checkout is not marked ended (a restore or the next sign-in reads it again) and
	// the wait ends with its outcome unknown.
	Forget(Watch);
	// Whether it was paid is unknown, so the cached wallet is no longer known to be the server's.
	if (UFlockSubsystem* Sdk = Flock.Get())
	{
		if (FFlockShopProvider* Shop = Sdk->GetShopProvider())
		{
			Shop->ForgetPlayerDataAfterPayment();
		}
	}
	if (const TFunction<void(TFlockResult<FFlockPurchaseResult>)> Answer = Take(Watch))
	{
		const bool bNotKept = Failure.ErrorCode == EFlockErrorCode::ShopPurchaseNotFound && Watch->OpenedAt >= 0.0;
		FFlockError Unknown = bNotKept
			? Ending(EFlockErrorType::Validation,
				TEXT("Flock's server does not hold the checkout it opened for this purchase, so its payment page must not be used"),
				TEXT("Ask the player not to pay on that page, and buy again to open a new checkout. If it keeps happening, the server is losing the checkouts it opens: report it to the backend team."))
			: Ending(Failure.Type,
				FString::Printf(TEXT("The web checkout could no longer be read (%s), so whether it was paid is unknown"), *Failure.Message),
				TEXT("The payment page may still be paid. Read the player's open checkouts with UFlockStore::Restore or FFlockShopProvider::GetPurchases(FlockPurchaseStatuses::Pending) before offering it again; a checkout paid meanwhile is delivered by Flock, so refresh the wallet and inventory."));
		Unknown.Code = Failure.Code;
		Unknown.ErrorCode = Failure.ErrorCode;
		Unknown.StatusCode = Failure.StatusCode;
		// Unknown either way: the player may already have paid on that page.
		Unknown.bOutcomeUnknown = true;
		Answer(TFlockResult<FFlockPurchaseResult>::Fail(Unknown));
	}
}

void FFlockWebCheckout::HandlePaid(const TSharedRef<FWatch>& Watch, const FFlockPlayerPurchase& Purchase)
{
	UFlockSubsystem* Sdk = Flock.Get();
	FFlockShopProvider* Shop = Sdk != nullptr ? Sdk->GetShopProvider() : nullptr;
	if (Shop != nullptr)
	{
		// Paid outside a call that answers with the wallet: the next read of the player's data asks the server.
		Shop->ForgetPlayerDataAfterPayment();
	}
	FFlockPurchaseResult Result;
	Result.PurchaseId = Purchase.Id;
	Result.ItemType = Purchase.ItemType;
	Result.Status = Purchase.Status;
	Result.Granted = Purchase.Granted;
	// Off the books first, so a handler of the event can buy again; the event comes before the call's answer.
	const TFunction<void(TFlockResult<FFlockPurchaseResult>)> Answer = Take(Watch);
	if (Watch->bHasItem || Shop == nullptr)
	{
		FFlockShopItem Item = Watch->Item;
		if (!Watch->bHasItem)
		{
			Item.Id = Watch->ShopItemId;
		}
		Paid(Item, Result);
		if (Answer)
		{
			Answer(TFlockResult<FFlockPurchaseResult>::Ok(Result));
		}
		return;
	}
	// A checkout found at a sign-in names its item by id alone: read it for the event.
	TWeakPtr<FFlockWebCheckout> WeakSelf = AsShared();
	const FString ShopItemId = Watch->ShopItemId;
	Shop->GetItem(ShopItemId, [WeakSelf, ShopItemId, Result, Answer](TFlockResult<FFlockShopItem> Read)
	{
		const TSharedPtr<FFlockWebCheckout> Self = WeakSelf.Pin();
		if (Self.IsValid() && !Self->bDisposed)
		{
			FFlockShopItem Item;
			// An answer that names another item, or none, is not this item.
			if (Read.IsSuccess() && Read.Value.Id.Equals(ShopItemId, ESearchCase::CaseSensitive))
			{
				Item = Read.Value;
			}
			else
			{
				Item.Id = ShopItemId;
				if (UFlockSubsystem* Sdk = Self->Flock.Get())
				{
					Sdk->GetLogger().LogWarning(FString::Printf(TEXT("A web checkout was paid for shop item '%s', which could not be read (%s); OnPurchaseCompleted carries its Id alone."),
						*ShopItemId, Read.IsSuccess() ? TEXT("the answer named another item") : *Read.Error.Message));
				}
			}
			Self->Paid(Item, Result);
		}
		if (Answer)
		{
			Answer(TFlockResult<FFlockPurchaseResult>::Ok(Result));
		}
	});
}

void FFlockWebCheckout::Forget(const TSharedRef<FWatch>& Watch)
{
	if (const TSharedRef<FWatch>* Current = Watches.Find(Watch->PurchaseId))
	{
		if (*Current == Watch)
		{
			Watches.Remove(Watch->PurchaseId);
		}
	}
}

TFunction<void(TFlockResult<FFlockPurchaseResult>)> FFlockWebCheckout::Take(const TSharedRef<FWatch>& Watch)
{
	TFunction<void(TFlockResult<FFlockPurchaseResult>)> Answer = MoveTemp(Watch->OnDone);
	Watch->OnDone = nullptr;
	// Off the books before anything is raised, so a handler can buy the item again.
	if (Answer)
	{
		ReleaseBuying(Watch->ShopItemId, Watch->BuyingNumber);
	}
	return Answer;
}

void FFlockWebCheckout::ReleaseBuying(const FString& ShopItemId, int32 Number)
{
	if (const int32* Holding = Buying.Find(ShopItemId))
	{
		if (*Holding == Number)
		{
			Buying.Remove(ShopItemId);
		}
	}
}

// ── Sign-ins ──

void FFlockWebCheckout::HandleSignedIn()
{
	if (bDisposed)
	{
		return;
	}
	EndWhatAnEndedSignInStarted();
	TWeakPtr<FFlockWebCheckout> WeakSelf = AsShared();
	FindOpenCheckouts([WeakSelf](const FFlockError& Error)
	{
		const TSharedPtr<FFlockWebCheckout> Self = WeakSelf.Pin();
		// A sign-in that ended meanwhile looks again at the next one.
		if (!Self.IsValid() || Error.Type == EFlockErrorType::None || Error.Type == EFlockErrorType::Cancelled)
		{
			return;
		}
		if (UFlockSubsystem* Sdk = Self->Flock.Get())
		{
			Sdk->GetLogger().LogWarning(FString::Printf(TEXT("The player's open web checkouts could not be listed (%s); they are looked for again at the next sign-in, or with UFlockStore::Restore."),
				*Error.Message));
		}
	});
}

void FFlockWebCheckout::HandleSignInEnded()
{
	if (!bDisposed)
	{
		EndWhatAnEndedSignInStarted();
	}
}

void FFlockWebCheckout::EndWhatAnEndedSignInStarted()
{
	const int32 Now = CurrentSignIn();
	// The checkouts seen to end belong to the sign-in that saw them; a second sign-in event for the same sign-in (a restore
	// answered after the connect) keeps them.
	if (EndedSignIn != Now)
	{
		Ended.Empty();
		EndedSignIn = Now;
	}
	TArray<TSharedRef<FWatch>> Current;
	Watches.GenerateValueArray(Current);
	for (const TSharedRef<FWatch>& Watch : Current)
	{
		if (Watch->SignIn == Now && IsSignedIn() && Watch->Call.IsValid() && Watch->Call->IsRunning())
		{
			continue;
		}
		if (Watch->Call.IsValid())
		{
			Watch->Call->Stop();
		}
		Forget(Watch);
		if (const TFunction<void(TFlockResult<FFlockPurchaseResult>)> Answer = Take(Watch))
		{
			Answer(TFlockResult<FFlockPurchaseResult>::Fail(Ending(EFlockErrorType::Cancelled,
				TEXT("The player signed out or changed while the web checkout was waited for"),
				TEXT("Nothing needs fixing: if that player pays it, Flock delivers it to them; OnPurchaseCompleted does not report it, so refresh the wallet and inventory after each sign-in."))));
		}
	}
}

void FFlockWebCheckout::FindOpenCheckouts(TFunction<void(const FFlockError&)> OnDone)
{
	if (bDisposed || !IsSignedIn())
	{
		OnDone(Ending(EFlockErrorType::Auth, TEXT("No player is signed in"), FFlockErrorHints::SignInFirst()));
		return;
	}
	FindOpenCheckoutsFrom(1, CurrentSignIn(), MoveTemp(OnDone));
}

void FFlockWebCheckout::FindOpenCheckoutsFrom(int32 Page, int32 SignIn, TFunction<void(const FFlockError&)> OnDone)
{
	UFlockSubsystem* Sdk = Flock.Get();
	FFlockShopProvider* Shop = Sdk != nullptr ? Sdk->GetShopProvider() : nullptr;
	if (Shop == nullptr)
	{
		OnDone(Ending(EFlockErrorType::Cancelled, TEXT("Flock shut down while the open web checkouts were listed"), TEXT("Connect again once Flock has initialized.")));
		return;
	}
	TWeakPtr<FFlockWebCheckout> WeakSelf = AsShared();
	Shop->GetPurchases(FlockPurchaseStatuses::Pending, Page, OpenCheckoutsPageSize,
		[WeakSelf, Page, SignIn, OnDone](TFlockResult<FFlockPlayerPurchasePage> Listed)
	{
		const TSharedPtr<FFlockWebCheckout> Self = WeakSelf.Pin();
		if (!Self.IsValid() || Self->bDisposed || Self->CurrentSignIn() != SignIn)
		{
			OnDone(Ending(EFlockErrorType::Cancelled, TEXT("The player signed out or changed, or the store was disconnected, while the open web checkouts were listed"),
				TEXT("Nothing needs fixing: the next sign-in looks for them again.")));
			return;
		}
		if (!Listed.IsSuccess())
		{
			OnDone(Listed.Error);
			return;
		}
		for (const FFlockPlayerPurchase& Purchase : Listed.Value.Items)
		{
			if (!Purchase.Id.IsEmpty() && Purchase.IsPending() && IsWebGateway(Purchase.PaymentProvider) && !Self->Ended.Contains(Purchase.Id))
			{
				// The item as the connection read it; one no longer in the shops is read when the checkout is paid.
				FFlockShopItem Item;
				const bool bKnown = Self->FindItem && Self->FindItem(Purchase.ShopItemId, Item);
				Self->WatchCheckout(Purchase.Id, Purchase.ShopItemId, bKnown ? &Item : nullptr, /*bOpenedHere*/ false);
			}
		}
		if (Listed.Value.Items.Num() < OpenCheckoutsPageSize || Page * OpenCheckoutsPageSize >= Listed.Value.Total)
		{
			OnDone(FFlockError());
			return;
		}
		Self->FindOpenCheckoutsFrom(Page + 1, SignIn, OnDone);
	});
}

// ── Disconnect ──

void FFlockWebCheckout::Dispose()
{
	if (bDisposed)
	{
		return;
	}
	bDisposed = true;
	TArray<TSharedRef<FWatch>> Current;
	Watches.GenerateValueArray(Current);
	Watches.Empty();
	for (const TSharedRef<FWatch>& Watch : Current)
	{
		if (Watch->Call.IsValid())
		{
			Watch->Call->Stop();
		}
		if (const TFunction<void(TFlockResult<FFlockPurchaseResult>)> Answer = Take(Watch))
		{
			Answer(TFlockResult<FFlockPurchaseResult>::Fail(Ending(EFlockErrorType::Cancelled,
				TEXT("The store connection was disconnected while the web checkout was waited for"),
				TEXT("Nothing needs fixing: a checkout paid afterwards is delivered by Flock but not reported, so refresh the wallet and inventory once connected again; one still unpaid is read again by the next connection."))));
		}
	}
}

// ── Helpers ──

FString FFlockWebCheckout::FixFor(const FString& Reason)
{
	if (Reason.Equals(FlockStoreFailureReasons::PaymentDeclined, ESearchCase::CaseSensitive))
	{
		return TEXT("Nothing was charged: the payment gateway declined the payment. Buy again to open a new checkout, where the player can pay another way.");
	}
	if (Reason.Equals(FlockStoreFailureReasons::Expired, ESearchCase::CaseSensitive))
	{
		return TEXT("Nothing was charged: the checkout was not paid within the hour it stays open, or the gateway cancelled it. Buy again to open a new one.");
	}
	return TEXT("Nothing was delivered by this checkout. Read the player's purchases with FFlockShopProvider::GetPurchases to see where it got to, and buy again if it was not paid.");
}

int32 FFlockWebCheckout::CurrentSignIn() const
{
	const UFlockSubsystem* Sdk = Flock.Get();
	return Sdk != nullptr && Sdk->AuthSession.IsValid() ? Sdk->AuthSession->GetSignInNumber() : INDEX_NONE;
}

bool FFlockWebCheckout::IsSignedIn() const
{
	const UFlockSubsystem* Sdk = Flock.Get();
	return Sdk != nullptr && Sdk->AuthSession.IsValid() && Sdk->AuthSession->IsAuthenticated();
}

double FFlockWebCheckout::Now() const
{
	UFlockSubsystem* Sdk = Flock.Get();
	FFlockMultiplayerProvider* Calls = Sdk != nullptr ? Sdk->GetMultiplayerProvider() : nullptr;
	return Calls != nullptr ? Calls->GetRepeatingCalls().Now() : FPlatformTime::Seconds();
}
