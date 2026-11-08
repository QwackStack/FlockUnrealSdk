// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Providers/FlockShopProvider.h"
#include "Http/FlockErrorHints.h"

#include "Http/FlockEndpoints.h"
#include "Http/FlockJsonUtils.h"
#include "Misc/FlockWhiteSpace.h"
#include "Providers/FlockAnalyticsProvider.h"
#include "Providers/FlockPlayerProvider.h"

const TCHAR* const FFlockShopProvider::SnapshotCategory = TEXT("shop");

FFlockShopProvider::FFlockShopProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
	const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
	const FString& InVersionedApiUrl, const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore,
	const FString& InGameVersionId)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, VersionedApiUrl(InVersionedApiUrl)
{
	// A call whose token has expired renews it once and is sent again, as on every provider that acts for the player.
	SetAuthSession(InSession);
	SetSnapshotStore(InSnapshotStore, InGameVersionId);
}

// ─────────────────────────────────── Catalog ───────────────────────────────────

void FFlockShopProvider::GetAll(int32 Page, int32 Limit, TFunction<void(TFlockResult<FFlockShopPage>)> OnComplete)
{
	const FString PageKey = FString::Printf(TEXT("all_p%d_l%d"), Page, Limit);
	if (const FFlockShopPage* Cached = ShopPages.Find(PageKey))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockShopPage>::Ok(*Cached));
		}
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FString::Printf(TEXT("%s?page=%d&limit=%d"), FlockEndpoints::Shop, Page, Limit));
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	TWeakPtr<FFlockShopProvider> WeakSelf = AsShared();
	const int32 DropsWhenSent = CatalogDrops;

	FetchWithSnapshot<FFlockShopPage>(SnapshotCategory, PageKey,
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockShopPage>)> OnAttempt)
		{
			// GetPaged returns the template TFlockPage; copy it into the concrete, snapshot-able page.
			return ClientRef->GetPaged<FFlockShop>(Url, SessionRef->GetAuthHeaders(),
				[OnAttempt](TFlockResult<TFlockPage<FFlockShop>> Result)
				{
					if (!Result.bSuccess)
					{
						OnAttempt(TFlockResult<FFlockShopPage>::Fail(Result.Error));
						return;
					}
					FFlockShopPage Page;
					Page.Items = Result.Value.Items;
					Page.Total = Result.Value.Total;
					Page.Page = Result.Value.Page;
					Page.Limit = Result.Value.Limit;
					OnAttempt(TFlockResult<FFlockShopPage>::Ok(Page));
				});
		},
		TEXT("Fetch shops"),
		[WeakSelf, PageKey, DropsWhenSent, OnComplete](TFlockResult<FFlockShopPage> Result)
		{
			if (const TSharedPtr<FFlockShopProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess && Self->KeepsWhatItRead(DropsWhenSent, PageKey))
				{
					Self->ShopPages.Add(PageKey, Result.Value);
					for (const FFlockShop& Shop : Result.Value.Items)
					{
						Self->IndexShop(Shop);
					}
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

void FFlockShopProvider::GetById(const FString& ShopId, TFunction<void(TFlockResult<FFlockShop>)> OnComplete)
{
	if (!RequireNotEmpty(ShopId, TEXT("Shop ID"), OnComplete))
	{
		return;
	}
	if (const FFlockShop* Cached = ShopsById.Find(ShopId))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockShop>::Ok(*Cached));
		}
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::ShopById(ShopId));
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	TWeakPtr<FFlockShopProvider> WeakSelf = AsShared();
	const int32 DropsWhenSent = CatalogDrops;

	FetchWithSnapshot<FFlockShop>(SnapshotCategory, FString::Printf(TEXT("shop_%s"), *ShopId),
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockShop>)> OnAttempt)
		{
			return ClientRef->GetRaw<FFlockShop>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
		},
		TEXT("Fetch shop"),
		[WeakSelf, ShopId, DropsWhenSent, OnComplete](TFlockResult<FFlockShop> Result)
		{
			// Keyed by the requested id (not shop.Id), so the next call with this id is a hit.
			if (const TSharedPtr<FFlockShopProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess && Self->KeepsWhatItRead(DropsWhenSent, FString::Printf(TEXT("shop_%s"), *ShopId)))
				{
					Self->ShopsById.Add(ShopId, Result.Value);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

void FFlockShopProvider::GetByName(const FString& Name, TFunction<void(TFlockResult<FFlockShop>)> OnComplete)
{
	if (!RequireNotEmpty(Name, TEXT("Shop Name"), OnComplete))
	{
		return;
	}
	if (const FString* Id = ShopIdByName.Find(Name))
	{
		if (const FFlockShop* Cached = ShopsById.Find(*Id))
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<FFlockShop>::Ok(*Cached));
			}
			return;
		}
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::ShopByName(Name));
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	TWeakPtr<FFlockShopProvider> WeakSelf = AsShared();
	const int32 DropsWhenSent = CatalogDrops;

	FetchWithSnapshot<FFlockShop>(SnapshotCategory, FString::Printf(TEXT("shop_name_%s"), *Name),
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockShop>)> OnAttempt)
		{
			return ClientRef->GetRaw<FFlockShop>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
		},
		TEXT("Fetch shop by name"),
		[WeakSelf, Name, DropsWhenSent, OnComplete](TFlockResult<FFlockShop> Result)
		{
			if (const TSharedPtr<FFlockShopProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess && Self->KeepsWhatItRead(DropsWhenSent, FString::Printf(TEXT("shop_name_%s"), *Name)))
				{
					Self->IndexShop(Result.Value);
					if (!Result.Value.Id.IsEmpty())
					{
						Self->ShopIdByName.Add(Name, Result.Value.Id);
					}
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

void FFlockShopProvider::GetItem(const FString& ShopItemId, TFunction<void(TFlockResult<FFlockShopItem>)> OnComplete)
{
	if (!RequireNotEmpty(ShopItemId, TEXT("Shop Item ID"), OnComplete))
	{
		return;
	}
	if (const FFlockShopItem* Cached = ItemsById.Find(ShopItemId))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockShopItem>::Ok(*Cached));
		}
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::ShopItemById(ShopItemId));
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	TWeakPtr<FFlockShopProvider> WeakSelf = AsShared();
	const int32 DropsWhenSent = CatalogDrops;

	FetchWithSnapshot<FFlockShopItem>(SnapshotCategory, FString::Printf(TEXT("item_%s"), *ShopItemId),
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockShopItem>)> OnAttempt)
		{
			return ClientRef->GetRaw<FFlockShopItem>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
		},
		TEXT("Fetch shop item"),
		[WeakSelf, ShopItemId, DropsWhenSent, OnComplete](TFlockResult<FFlockShopItem> Result)
		{
			if (const TSharedPtr<FFlockShopProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess && Self->KeepsWhatItRead(DropsWhenSent, FString::Printf(TEXT("item_%s"), *ShopItemId)))
				{
					Self->ItemsById.Add(ShopItemId, Result.Value);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

void FFlockShopProvider::GetItemsByShop(const FString& ShopId, const FString& PatchId,
	TFunction<void(TFlockResult<TArray<FFlockShopItem>>)> OnComplete)
{
	if (!RequireNotEmpty(ShopId, TEXT("Shop ID"), OnComplete))
	{
		return;
	}

	const FString CacheKey = FString::Printf(TEXT("items_shop_%s_%s"), *ShopId,
		PatchId.IsEmpty() ? TEXT("current") : *PatchId);
	if (const TArray<FString>* CachedIds = ItemIdsByShop.Find(CacheKey))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<TArray<FFlockShopItem>>::Ok(ResolveItems(*CachedIds)));
		}
		return;
	}

	FString Path = FlockEndpoints::ShopItemsByShop(ShopId);
	if (!PatchId.IsEmpty())
	{
		Path += FString::Printf(TEXT("?patch_id=%s"), *PatchId);
	}
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(Path);
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	TWeakPtr<FFlockShopProvider> WeakSelf = AsShared();
	const int32 DropsWhenSent = CatalogDrops;

	// Enveloped list ({error,response,result:[...]}) — GetList unwraps `result` as a bare array.
	FetchListWithSnapshot<FFlockShopItem>(SnapshotCategory, CacheKey,
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<TArray<FFlockShopItem>>)> OnAttempt)
		{
			return ClientRef->GetList<FFlockShopItem>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
		},
		TEXT("Fetch shop items"),
		[WeakSelf, CacheKey, DropsWhenSent, OnComplete](TFlockResult<TArray<FFlockShopItem>> Result)
		{
			if (const TSharedPtr<FFlockShopProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess && Self->KeepsWhatItRead(DropsWhenSent, CacheKey))
				{
					TArray<FString> Ids;
					Ids.Reserve(Result.Value.Num());
					for (const FFlockShopItem& Item : Result.Value)
					{
						if (!Item.Id.IsEmpty())
						{
							Self->ItemsById.Add(Item.Id, Item);
							Ids.Add(Item.Id);
						}
					}
					Self->ItemIdsByShop.Add(CacheKey, Ids);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

// ───────────────────────────── Purchase + inventory ────────────────────────────

void FFlockShopProvider::Purchase(const FString& ShopItemId, const FString& PlayerId,
	TFunction<void(TFlockResult<FFlockPurchaseResult>)> OnComplete)
{
	if (!RequireNotEmpty(ShopItemId, TEXT("Shop Item ID"), OnComplete))
	{
		return;
	}
	// Default to the signed-in player; callers no longer need to pass it explicitly.
	const FString ResolvedPlayerId = PlayerId.IsEmpty() ? Session->GetPlayerId() : PlayerId;
	if (!RequireNotEmpty(ResolvedPlayerId, TEXT("Player ID"), OnComplete, FFlockErrorHints::SignInFirst()))
	{
		return;
	}

	TWeakPtr<FFlockShopProvider> WeakSelf = AsShared();
	// The purchase acts for the sign-in current now: sent only while it lasts, and its records go to it alone.
	const int32 PurchaseSignIn = Session->GetSignInNumber();

	// The item is fetched first — the transaction record needs its price/currency, and a failed fetch
	// fails the purchase: without the item there is nothing to charge for and nothing to record.
	GetItem(ShopItemId,
		[WeakSelf, ShopItemId, ResolvedPlayerId, PurchaseSignIn, OnComplete](TFlockResult<FFlockShopItem> ItemResult)
		{
			const TSharedPtr<FFlockShopProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockPurchaseResult>::Fail(FFlockError::Make(
						EFlockErrorType::Cancelled, TEXT("Shop provider was destroyed"))));
				}
				return;
			}
			if (!ItemResult.bSuccess)
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockPurchaseResult>::Fail(ItemResult.Error));
				}
				return;
			}

			const FFlockShopItem Item = ItemResult.Value;

			FFlockShopTransactionRequest Request;
			Request.ShopItemId = ShopItemId;
			Request.PlayerId = ResolvedPlayerId;
			// One key for every attempt of this call: the server answers a purchase sent again with the first one, so an
			// answer that never came is sent again without buying twice.
			Request.IdempotencyKey = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
			FString Body;
			if (!FFlockJsonUtils::StructToWireJson(Request, Body))
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockPurchaseResult>::Fail(FFlockError::Make(EFlockErrorType::Serialization, TEXT("The SDK could not write the purchase's body"))));
				}
				return;
			}

			// Fire the Started record and move straight on — the purchase never waits on telemetry.
			if (Self->Session->GetSignInNumber() == PurchaseSignIn)
			{
				Self->RecordPurchaseStatus(TEXT("Started"), Item);
			}

			const TSharedRef<FFlockHttpClient> ClientRef = Self->Client;
			const TSharedRef<FFlockAuthSession> SessionRef = Self->Session;
			const FString Url = Self->MakeUrl(FlockEndpoints::ShopTransaction);
			FFlockRetryHandler::FOperation<FFlockPurchaseResult> SendPurchase = ForSignInOnly<FFlockPurchaseResult>(SessionRef, PurchaseSignIn,
				[ClientRef, SessionRef, Url, Body](TFunction<void(TFlockResult<FFlockPurchaseResult>)> OnAttempt)
				{
					return ClientRef->PostJsonRaw<FFlockPurchaseResult>(Url, SessionRef->GetAuthHeaders(), Body, MoveTemp(OnAttempt));
				});

			// Sent again under its key after an answer that never came (the server makes one purchase per key); once any
			// attempt is in doubt the call ends unknown, unless a later refusal proves nothing was bought. Bare response —
			// PostJsonRaw reads the model at the root.
			Self->ExecuteMoneyCall<FFlockPurchaseResult>(MoveTemp(SendPurchase),
				[WeakSelf, Item, PurchaseSignIn, OnComplete](TFlockResult<FFlockPurchaseResult> PurchaseResult)
				{
					// Fire the outcome record (best-effort) and deliver the result immediately — neither
					// waits on the other. If the provider is gone, or the purchase's sign-in has ended, the record
					// is skipped: it would otherwise go to whoever is signed in now. A purchase whose outcome is unknown, or one
					// cancelled (its request may have gone out), records neither: it may have been bought.
					if (const TSharedPtr<FFlockShopProvider> Self2 = WeakSelf.Pin())
					{
						if (Self2->Session->GetSignInNumber() == PurchaseSignIn && !PurchaseResult.Error.bOutcomeUnknown
							&& PurchaseResult.Error.Type != EFlockErrorType::Cancelled)
						{
							Self2->RecordPurchaseStatus(PurchaseResult.bSuccess ? TEXT("Purchased") : TEXT("Failed"), Item);
						}
						// Switched off in the dashboard since the shop was read: the read the fix asks for has to ask the server,
						// which leaves the item out. Dropped before the caller is told, so a read it makes from its handler does;
						// the copy on disk goes too, since a read that fails once would otherwise serve it and keep it again.
						if (!PurchaseResult.bSuccess && PurchaseResult.Error.ErrorCode == EFlockErrorCode::ShopItemNotForSale)
						{
							Self2->ClearCache();
						}
						// The server's balance is not the cached one (another device spent from it, say), so the wallet read
						// the fix asks for has to ask the server too.
						if (!PurchaseResult.bSuccess && (PurchaseResult.Error.ErrorCode == EFlockErrorCode::ShopInsufficientFunds
							|| PurchaseResult.Error.ErrorCode == EFlockErrorCode::ShopCurrencyNotHeld))
						{
							Self2->ForgetCachedPlayerData();
						}
						if (PurchaseResult.bSuccess)
						{
							// Applied before the caller is told, so a handler that reads player data straight
							// after the purchase sees the balances the purchase produced rather than the ones
							// it replaced.
							Self2->ApplyWalletToPlayerCache(PurchaseResult.Value.Wallet);
						}
					}
					if (OnComplete)
					{
						OnComplete(PurchaseResult);
					}
				},
				TEXT("Purchase shop item"), /*bIdempotent*/ true, Self->MoneyCall(
					TEXT("The purchase may have been made: no answer said whether it was. Read the player's inventory and wallet to see whether it went through before offering it again; a new Purchase call is a new purchase."),
					&IsARefusalAfterTheKey));
		});
}

bool FFlockShopProvider::IsARefusalAfterTheKey(const FFlockError& Refused)
{
	// The server looks the key up right after locking the wallet and before these checks, so they prove no earlier attempt bought it.
	return Refused.ErrorCode == EFlockErrorCode::ShopInsufficientFunds || Refused.ErrorCode == EFlockErrorCode::ShopCurrencyNotHeld
		|| Refused.ErrorCode == EFlockErrorCode::ShopItemNotForSale;
}

FFlockProviderBase::FMoneyCall FFlockShopProvider::MoneyCall(const FString& FixWhenUnknown,
	TFunction<bool(const FFlockError&)> ProvesNoAttemptDidIt) const
{
	FMoneyCall Rules;
	Rules.ForgetPlayersData = FFlockPlayerProvider::ClearsPlayerDataCacheOf(PlayerProvider);
	Rules.FixWhenUnknown = FixWhenUnknown;
	Rules.ProvesNoAttemptDidIt = MoveTemp(ProvesNoAttemptDidIt);
	return Rules;
}

void FFlockShopProvider::RedeemStoreReceipt(const FString& ShopItemId, const FFlockStoreReceipt& Receipt,
	TFunction<void(TFlockResult<FFlockPurchaseResult>)> OnComplete)
{
	if (!RequireNotEmpty(ShopItemId, TEXT("Shop Item ID"), OnComplete))
	{
		return;
	}
	TSharedRef<FJsonObject> Ids = MakeShared<FJsonObject>();
	if (!ReceiptIds(Receipt, Ids, OnComplete))
	{
		return;
	}
	// The server redeems for the player the sign-in names, so with nobody signed in there is no one to redeem for.
	if (!Session->IsAuthenticated())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockPurchaseResult>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Auth,
				TEXT("RedeemStoreReceipt needs a signed-in player, and none is"), FFlockErrorHints::SignInFirst())));
		}
		return;
	}

	// No player_id: the server redeems for the player the sign-in names, and refuses a body naming another one.
	const TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("shop_item_id"), ShopItemId);
	Request->SetStringField(TEXT("provider"), Receipt.Store);
	Request->SetObjectField(TEXT("receipt"), Ids);
	FString Body;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	if (!FJsonSerializer::Serialize(Request, Writer))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockPurchaseResult>::Fail(FFlockError::Make(EFlockErrorType::Serialization,
				TEXT("The SDK could not write the redemption's body"))));
		}
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const FString Url = MakeUrl(FlockEndpoints::ShopValidateReceipt);
	// Sent only while the sign-in current now lasts: another player's sign-in never redeems this player's receipt. The
	// token is read on each attempt, so a refreshed one is used.
	FFlockRetryHandler::FOperation<FFlockPurchaseResult> SendRedemption = ForSignInOnly<FFlockPurchaseResult>(
		SessionRef, Session->GetSignInNumber(),
		[ClientRef, SessionRef, Url, Body](TFunction<void(TFlockResult<FFlockPurchaseResult>)> OnAttempt)
		{
			return ClientRef->PostJsonRaw<FFlockPurchaseResult>(Url, SessionRef->GetAuthHeaders(), Body, MoveTemp(OnAttempt));
		});

	TWeakPtr<FFlockShopProvider> WeakSelf = AsShared();
	// A store purchase makes one Flock purchase, so this one may be sent again, unlike a wallet purchase; once an attempt is
	// in doubt the call ends unknown. Bare response — PostJsonRaw reads the model at the root.
	ExecuteMoneyCall<FFlockPurchaseResult>(MoveTemp(SendRedemption),
		[WeakSelf, OnComplete](TFlockResult<FFlockPurchaseResult> Result)
		{
			if (const TSharedPtr<FFlockShopProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess)
				{
					// A reply that lands after a sign-out still carries the buyer's wallet, which goes to the buyer's own
					// cache entry (keyed by the wallet's player), never to whoever is signed in now.
					Self->ApplyWalletToPlayerCache(Result.Value.Wallet);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		},
		TEXT("Redeem store receipt"), /*bIdempotent*/ true,
		MoneyCall(TEXT("Leave the store's purchase unfinished and send the same receipt again: it cannot be paid for twice.")));
}

void FFlockShopProvider::ForgetCachedPlayerData() const
{
	FFlockPlayerProvider::ClearsPlayerDataCacheOf(PlayerProvider)();
}

bool FFlockShopProvider::ReceiptIds(const FFlockStoreReceipt& Receipt, TSharedRef<FJsonObject>& OutIds,
	const TFunction<void(TFlockResult<FFlockPurchaseResult>)>& OnComplete)
{
	auto Refuse = [&OnComplete](const FString& Message, const FString& Fix)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockPurchaseResult>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation, Message, Fix)));
		}
		return false;
	};
	// A store id with spaces around it is a bug where it was copied; trimming it here would hide that bug.
	auto AsTheStoreIssuedIt = [&Refuse, &OutIds](const FString& Id, const TCHAR* Key, const TCHAR* Name)
	{
		if (Id.IsEmpty())
		{
			return Refuse(FString::Printf(TEXT("The %s is empty"), Name),
				FString::Printf(TEXT("Pass the %s the store gave for this purchase."), Name));
		}
		if (FlockWhiteSpace::IsAtEitherEnd(Id))
		{
			return Refuse(FString::Printf(TEXT("The %s starts or ends with a space"), Name),
				FString::Printf(TEXT("Pass the %s exactly as the store gave it; spaces around it come from wherever it was copied or stored."), Name));
		}
		OutIds->SetStringField(Key, Id);
		return true;
	};

	// Matched with its letter case: the route takes the store's name exactly, and a member written by hand is refused
	// here rather than read as the other store.
	if (Receipt.Store.Equals(FlockPaymentProviders::AppleAppStore, ESearchCase::CaseSensitive))
	{
		return AsTheStoreIssuedIt(Receipt.TransactionId, TEXT("transaction_id"), TEXT("App Store transaction id"));
	}
	if (Receipt.Store.Equals(FlockPaymentProviders::GooglePlay, ESearchCase::CaseSensitive))
	{
		return AsTheStoreIssuedIt(Receipt.ProductId, TEXT("product_id"), TEXT("Google Play product id"))
			&& AsTheStoreIssuedIt(Receipt.PurchaseToken, TEXT("purchase_token"), TEXT("Google Play purchase token"));
	}
	return Refuse(Receipt.Store.IsEmpty()
			? FString(TEXT("No store receipt was given"))
			: FString::Printf(TEXT("'%s' is not a store this call redeems"), *Receipt.Store),
		TEXT("Pass the store's purchase as FFlockStoreReceipt::FromAppleAppStore or FFlockStoreReceipt::FromGooglePlay."));
}

void FFlockShopProvider::Consume(const FString& InventoryId, TFunction<void(TFlockResult<FFlockConsumeResult>)> OnComplete)
{
	ConsumeEntry(InventoryId, Session->GetSignInNumber(), MoveTemp(OnComplete), /*bUsedMeanwhileIsExpected*/ false);
}

void FFlockShopProvider::ConsumeEntry(const FString& InventoryId, int32 SignInNumber,
	TFunction<void(TFlockResult<FFlockConsumeResult>)> OnComplete, bool bUsedMeanwhileIsExpected)
{
	if (!RequireNotEmpty(InventoryId, TEXT("Inventory ID"), OnComplete))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const FString Url = MakeUrl(FlockEndpoints::PlayerInventoryConsume(InventoryId));

	// Consuming credits currency, so it carries a purchase's risk in the other direction: re-sending after
	// an ambiguous failure grants twice, and nothing on the client can undo that. bIdempotent=false keeps
	// the retry budget to failures that prove the request was never processed, and surfaces the rest.
	// Bare response — PostJsonRaw reads the model at the root. The route declares no request body; an
	// empty object is sent so the POST stays well-formed. Sent only while its sign-in lasts, the token read on each attempt.
	TWeakPtr<FFlockShopProvider> WeakSelf = AsShared();

	ExecuteMoneyCall<FFlockConsumeResult>(ForSignInOnly<FFlockConsumeResult>(SessionRef, SignInNumber,
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockConsumeResult>)> OnAttempt)
		{
			return ClientRef->PostJsonRaw<FFlockConsumeResult>(Url, SessionRef->GetAuthHeaders(), TEXT("{}"), MoveTemp(OnAttempt));
		}),
		[WeakSelf, OnComplete](TFlockResult<FFlockConsumeResult> Result)
		{
			// Consuming credits currency, so the cached wallet goes stale here exactly as it does on a
			// purchase — same write-through, same ordering.
			if (Result.bSuccess)
			{
				if (const TSharedPtr<FFlockShopProvider> Self = WeakSelf.Pin())
				{
					Self->ApplyWalletToPlayerCache(Result.Value.Wallet);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		},
		TEXT("Consume inventory item"), /*bIdempotent*/ false,
		MoneyCall(TEXT("Read the player's inventory to see whether it was used before offering to use it again.")),
		// ConsumeOne passes over an entry used meanwhile and tries the next: a step of the call, not a failure to log.
		[bUsedMeanwhileIsExpected](const FFlockError& Error)
		{
			return bUsedMeanwhileIsExpected && Error.ErrorCode == EFlockErrorCode::PlayerInventoryAlreadyUsed;
		});
}

void FFlockShopProvider::ConsumeOne(const FString& ShopItemId, TFunction<void(TFlockResult<FFlockConsumeResult>)> OnComplete)
{
	if (!RequireNotEmpty(ShopItemId, TEXT("Shop Item ID"), OnComplete))
	{
		return;
	}
	// Taken before the read, so an entry read for one player is never used as the player who signed in next.
	const FString PlayerId = Session->GetPlayerId();
	if (!Session->IsAuthenticated() || PlayerId.IsEmpty())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockConsumeResult>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Auth,
				TEXT("ConsumeOne needs a signed-in player, and none is"), FFlockErrorHints::SignInFirst())));
		}
		return;
	}
	const int32 SignInNumber = Session->GetSignInNumber();
	TWeakPtr<FFlockShopProvider> WeakSelf = AsShared();
	ReadUnusedEntries(PlayerId, SignInNumber,
		[WeakSelf, ShopItemId, SignInNumber, OnComplete](TFlockResult<TArray<FFlockPlayerInventory>> Entries)
		{
			if (!Entries.bSuccess)
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockConsumeResult>::Fail(Entries.Error));
				}
				return;
			}
			TArray<FString> Ids;
			for (const FFlockPlayerInventory& Entry : Entries.Value)
			{
				if (!Entry.Id.IsEmpty() && Entry.ShopItemId.Equals(ShopItemId, ESearchCase::CaseSensitive))
				{
					Ids.Add(Entry.Id);
				}
			}
			ConsumeFirstOf(WeakSelf, MakeShared<TArray<FString>>(MoveTemp(Ids)), 0, SignInNumber, OnComplete);
		});
}

void FFlockShopProvider::ConsumeFirstOf(const TWeakPtr<FFlockShopProvider>& WeakSelf, const TSharedRef<TArray<FString>>& InventoryIds,
	int32 Index, int32 SignInNumber, TFunction<void(TFlockResult<FFlockConsumeResult>)> OnComplete)
{
	const TSharedPtr<FFlockShopProvider> Self = WeakSelf.Pin();
	if (!Self.IsValid())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockConsumeResult>::Fail(FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Shop provider was destroyed"))));
		}
		return;
	}
	if (!InventoryIds->IsValidIndex(Index))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockConsumeResult>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation,
				TEXT("The signed-in player holds no unused entry of that shop item"),
				TEXT("Offer to use it only while GetOwnedItemCounts counts it; it may have been used on another device."))));
		}
		return;
	}
	Self->ConsumeEntry((*InventoryIds)[Index], SignInNumber,
		[WeakSelf, InventoryIds, Index, SignInNumber, OnComplete](TFlockResult<FFlockConsumeResult> Result)
		{
			// Used by another call or device since the read: the next one is still the player's to use. Any other failure
			// stops, so one call never uses two.
			if (!Result.bSuccess && Result.Error.ErrorCode == EFlockErrorCode::PlayerInventoryAlreadyUsed)
			{
				ConsumeFirstOf(WeakSelf, InventoryIds, Index + 1, SignInNumber, OnComplete);
				return;
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		}, /*bUsedMeanwhileIsExpected*/ true);
}

void FFlockShopProvider::GetOwnedItemCounts(TFunction<void(TFlockResult<FFlockOwnedItemCounts>)> OnComplete)
{
	const FString PlayerId = Session->GetPlayerId();
	if (!RequireNotEmpty(PlayerId, TEXT("Signed-in player's id"), OnComplete, FFlockErrorHints::SignInFirst()))
	{
		return;
	}
	ReadUnusedEntries(PlayerId, Session->GetSignInNumber(),
		[OnComplete](TFlockResult<TArray<FFlockPlayerInventory>> Entries)
		{
			if (!OnComplete)
			{
				return;
			}
			if (!Entries.bSuccess)
			{
				OnComplete(TFlockResult<FFlockOwnedItemCounts>::Fail(Entries.Error));
				return;
			}
			// Counted through an index of rows by item, so a large inventory is counted in one pass.
			FFlockOwnedItemCounts Counts;
			TMap<FString, int32, FDefaultSetAllocator, FFlockShopProvider::FExactIdKeyFuncs<int32>> RowOf;
			for (const FFlockPlayerInventory& Entry : Entries.Value)
			{
				if (const int32* Row = RowOf.Find(Entry.ShopItemId))
				{
					++Counts.Items[*Row].Count;
					continue;
				}
				FFlockOwnedItemCount Held;
				Held.ShopItemId = Entry.ShopItemId;
				Held.Count = 1;
				RowOf.Add(Entry.ShopItemId, Counts.Items.Add(Held));
			}
			OnComplete(TFlockResult<FFlockOwnedItemCounts>::Ok(Counts));
		});
}

void FFlockShopProvider::Owns(const FString& ShopItemId, TFunction<void(TFlockResult<bool>)> OnComplete)
{
	if (!RequireNotEmpty(ShopItemId, TEXT("Shop Item ID"), OnComplete))
	{
		return;
	}
	GetOwnedItemCounts([ShopItemId, OnComplete](TFlockResult<FFlockOwnedItemCounts> Counts)
	{
		if (!OnComplete)
		{
			return;
		}
		if (!Counts.bSuccess)
		{
			OnComplete(TFlockResult<bool>::Fail(Counts.Error));
			return;
		}
		OnComplete(TFlockResult<bool>::Ok(Counts.Value.CountOf(ShopItemId) > 0));
	});
}

void FFlockShopProvider::ReadUnusedEntries(const FString& PlayerId, int32 SignInNumber,
	TFunction<void(TFlockResult<TArray<FFlockPlayerInventory>>)> OnComplete)
{
	ReadUnusedEntriesFrom(AsShared(), PlayerId, SignInNumber, 1, MakeShared<FUnusedEntries>(), MoveTemp(OnComplete));
}

void FFlockShopProvider::ReadUnusedEntriesFrom(const TWeakPtr<FFlockShopProvider>& WeakSelf, const FString& PlayerId, int32 SignInNumber,
	int32 Page, const TSharedRef<FUnusedEntries>& Unused,
	TFunction<void(TFlockResult<TArray<FFlockPlayerInventory>>)> OnComplete)
{
	const TSharedPtr<FFlockShopProvider> Self = WeakSelf.Pin();
	if (!Self.IsValid())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<TArray<FFlockPlayerInventory>>::Fail(FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Shop provider was destroyed"))));
		}
		return;
	}
	Self->ReadInventoryPage(PlayerId, SignInNumber, Page, InventoryPageSize,
		[WeakSelf, PlayerId, SignInNumber, Page, Unused, OnComplete](TFlockResult<FFlockPlayerInventoryPage> Result)
		{
			if (!Result.bSuccess)
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<TArray<FFlockPlayerInventory>>::Fail(Result.Error));
				}
				return;
			}
			for (const FFlockPlayerInventory& Entry : Result.Value.Items)
			{
				// The route pages newest first by offset, so an entry made between two page reads hands the last of one page
				// back as the first of the next: it counts once.
				const bool bAlreadyRead = Unused->Ids.Contains(Entry.Id);
				Unused->Ids.Add(Entry.Id, true);
				if (!Entry.ShopItemId.IsEmpty() && !bAlreadyRead
					&& Entry.Status.Equals(FlockPlayerInventoryStatuses::Unused, ESearchCase::CaseSensitive))
				{
					Unused->Entries.Add(Entry);
				}
			}
			if (Result.Value.Items.Num() == 0 || Page * InventoryPageSize >= Result.Value.Total)
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<TArray<FFlockPlayerInventory>>::Ok(Unused->Entries));
				}
				return;
			}
			ReadUnusedEntriesFrom(WeakSelf, PlayerId, SignInNumber, Page + 1, Unused, OnComplete);
		});
}

void FFlockShopProvider::GetPlayerInventory(const FString& PlayerId, int32 Page, int32 Limit,
	TFunction<void(TFlockResult<FFlockPlayerInventoryPage>)> OnComplete)
{
	const FString ResolvedPlayerId = PlayerId.IsEmpty() ? Session->GetPlayerId() : PlayerId;
	if (!RequireNotEmpty(ResolvedPlayerId, TEXT("Player ID"), OnComplete, FFlockErrorHints::SignInFirst()))
	{
		return;
	}
	ReadInventoryPage(ResolvedPlayerId, Session->GetSignInNumber(), Page, Limit, MoveTemp(OnComplete));
}

void FFlockShopProvider::ReadInventoryPage(const FString& PlayerId, int32 SignInNumber, int32 Page, int32 Limit,
	TFunction<void(TFlockResult<FFlockPlayerInventoryPage>)> OnComplete)
{
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const FString Url = MakeUrl(FString::Printf(TEXT("%s?page=%d&limit=%d"),
		*FlockEndpoints::PlayerInventoryByPlayer(PlayerId), Page, Limit));

	// Inventory changes on every purchase — intentionally never cached (no dict, no snapshot); always
	// fresh, so offline it fails rather than serving stale ownership. Same rule as bans. Sent only while its sign-in lasts.
	Execute<FFlockPlayerInventoryPage>(ForSignInOnly<FFlockPlayerInventoryPage>(SessionRef, SignInNumber,
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockPlayerInventoryPage>)> OnAttempt)
		{
			return ClientRef->GetPaged<FFlockPlayerInventory>(Url, SessionRef->GetAuthHeaders(),
				[OnAttempt](TFlockResult<TFlockPage<FFlockPlayerInventory>> Result)
				{
					if (!Result.bSuccess)
					{
						OnAttempt(TFlockResult<FFlockPlayerInventoryPage>::Fail(Result.Error));
						return;
					}
					FFlockPlayerInventoryPage Page;
					Page.Items = Result.Value.Items;
					Page.Total = Result.Value.Total;
					Page.Page = Result.Value.Page;
					Page.Limit = Result.Value.Limit;
					OnAttempt(TFlockResult<FFlockPlayerInventoryPage>::Ok(Page));
				});
		}),
		MoveTemp(OnComplete), TEXT("Get player inventory"));
}

bool FFlockShopProvider::KeepsWhatItRead(int32 DropsWhenSent, const FString& SnapshotKey)
{
	if (CatalogDrops == DropsWhenSent)
	{
		return true;
	}
	// Answered across a clear: the snapshot written just before this completion may predate the clear, so it goes too.
	if (const TSharedPtr<FFlockSnapshotStore>& Store = GetSnapshotStore())
	{
		Store->DeleteKey(GetSnapshotScope(SnapshotCategory), SnapshotKey);
	}
	return false;
}

void FFlockShopProvider::ClearCache()
{
	ShopPages.Reset();
	ShopsById.Reset();
	ShopIdByName.Reset();
	ItemsById.Reset();
	ItemIdsByShop.Reset();
	// A read already on its way may have been answered before what made this clear, and would put it back in memory.
	++CatalogDrops;
	DeleteSnapshotCategory(SnapshotCategory);
}

// ─────────────────────────────── Internals ─────────────────────────────────────

void FFlockShopProvider::IndexShop(const FFlockShop& Shop)
{
	if (!Shop.Id.IsEmpty())
	{
		ShopsById.Add(Shop.Id, Shop);
	}
}

TArray<FFlockShopItem> FFlockShopProvider::ResolveItems(const TArray<FString>& Ids) const
{
	TArray<FFlockShopItem> Result;
	Result.Reserve(Ids.Num());
	for (const FString& Id : Ids)
	{
		if (const FFlockShopItem* Item = ItemsById.Find(Id))
		{
			Result.Add(*Item);
		}
	}
	return Result;
}

void FFlockShopProvider::ApplyWalletToPlayerCache(const FFlockPlayerData& Wallet) const
{
	if (Wallet.Id.IsEmpty())
	{
		// The server omits the wallet when the purchase moved no currency, so there is nothing to update
		// and nothing stale to correct.
		return;
	}
	const TSharedPtr<FFlockPlayerProvider> Players = PlayerProvider.Pin();
	if (!Players.IsValid())
	{
		// Built outside the subsystem, or torn down: the purchase still succeeds, the cache is just not
		// refreshed. ApplyServerPlayerData is likewise a no-op for a player whose rows were never cached.
		return;
	}
	Players->ApplyServerPlayerData(Wallet);
}

void FFlockShopProvider::RecordPurchaseStatus(const FString& Status, const FFlockShopItem& Item)
{
	const TSharedPtr<FFlockAnalyticsProvider> AnalyticsPtr = Analytics.Pin();
	if (!AnalyticsPtr.IsValid())
	{
		// Analytics disabled or gone: recording is best-effort, so there is simply nothing to do.
		return;
	}

	FFlockAnalyticsTransactionRequest Request;
	Request.Amount = static_cast<double>(Item.Price);
	Request.CurrencyCode = Item.Currency;
	Request.ShopItemId = Item.Id;
	// These two go on the wire verbatim, and the PascalCase is deliberate — distinct from the request
	// struct's lowercase defaults, so a missed assignment reads as wrong rather than plausible.
	Request.TransactionType = TEXT("Purchase");
	Request.Status = Status;
	// PlayerId/SessionId/CreatedAt left empty — RecordTransaction fills them from the current session.

	// Fire-and-forget: no completion, so the purchase neither waits on nor reacts to the record.
	AnalyticsPtr->RecordTransaction(Request);
}
