// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Store/FlockStore.h"

#include "Auth/FlockAuthSession.h"
#include "Config/FlockConfig.h"
#include "Misc/Paths.h"
#include "Config/FlockGameKey.h"
#include "Dom/JsonObject.h"
#include "FlockEvents.h"
#include "FlockSubsystem.h"
#include "Http/FlockErrorHints.h"
#include "Http/FlockSnapshotStore.h"
#include "Misc/ConfigCacheIni.h"
#include "Providers/FlockShopProvider.h"
#include "Store/FlockWebCheckout.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	const TCHAR* const PurchaseOperation = TEXT("Purchase shop item");
	/** Where the products sold are kept between launches: under the state the SDK keeps across game versions. */
	FString SoldProductsScope() { return FString(FFlockSnapshotStore::StateScope) + TEXT("/store"); }
	constexpr int32 CatalogPageSize = 100;

	TFunction<TSharedPtr<IFlockStoreConnection>()>& StoreLibrary()
	{
		static TFunction<TSharedPtr<IFlockStoreConnection>()> Library;
		return Library;
	}

	FFlockError Refusal(EFlockErrorType Type, const FString& Message, const FString& Fix)
	{
		FFlockError Error = FFlockError::MakeWithFix(Type, Message, Fix);
		Error.Operation = PurchaseOperation;
		return Error;
	}

	const FString& NotSignedInMessage()
	{
		static const FString Message(TEXT("No player is signed in"));
		return Message;
	}

	/** Every connection's purchases sent and being sent during this run of the game, by launch key. */
	TMap<FString, UFlockStore::FExactIdSet>& SentByLaunch()
	{
		static TMap<FString, UFlockStore::FExactIdSet> Sent;
		return Sent;
	}

	TMap<FString, UFlockStore::FExactIdSet>& BeingSentByLaunch()
	{
		static TMap<FString, UFlockStore::FExactIdSet> Sending;
		return Sending;
	}

	FFlockError RestoreCancelled()
	{
		return FFlockError::MakeWithFix(EFlockErrorType::Cancelled,
			TEXT("The store connection was disconnected while the store's purchases were restored"), TEXT("Connect again, then restore again."));
	}
}

UFlockStore::FExactIdSet& UFlockStore::SentThisLaunch() const
{
	return SentByLaunch().FindOrAdd(LaunchKey);
}

UFlockStore::FExactIdSet& UFlockStore::BeingSent() const
{
	return BeingSentFor(LaunchKey);
}

UFlockStore::FExactIdSet& UFlockStore::BeingSentFor(const FString& Key)
{
	return BeingSentByLaunch().FindOrAdd(Key);
}

// ── Store library ──

void UFlockStore::SetStoreLibrary(TFunction<TSharedPtr<IFlockStoreConnection>()> Factory)
{
	StoreLibrary() = MoveTemp(Factory);
}

void UFlockStore::ClearStoreLibrary()
{
	StoreLibrary() = nullptr;
}

TFunction<TSharedPtr<IFlockStoreConnection>()> UFlockStore::GetStoreLibrary()
{
	return StoreLibrary();
}

bool UFlockStore::HasStoreLibrary()
{
	return static_cast<bool>(StoreLibrary());
}

FString UFlockStore::PaymentProviderOfThisPlatform()
{
#if PLATFORM_ANDROID
	return FlockPaymentProviders::GooglePlay;
#elif PLATFORM_IOS
	return FlockPaymentProviders::AppleAppStore;
#else
	return FString();
#endif
}

bool UFlockStore::IsADesktop()
{
	return PLATFORM_DESKTOP != 0;
}

bool UFlockStore::IsSteamTheDefaultService()
{
	FString Service;
	return GConfig != nullptr && GConfig->GetString(TEXT("OnlineSubsystem"), TEXT("DefaultPlatformService"), Service, GEngineIni)
		&& Service.Equals(TEXT("Steam"), ESearchCase::IgnoreCase);
}

bool UFlockStore::IsAStore(const FString& PaymentProvider)
{
	return PaymentProvider.Equals(FlockPaymentProviders::GooglePlay, ESearchCase::CaseSensitive)
		|| PaymentProvider.Equals(FlockPaymentProviders::AppleAppStore, ESearchCase::CaseSensitive);
}

bool UFlockStore::IsSoldHere(const FFlockShopItem& Item)
{
	// A standard item is a one-time product; a currency pack or a consumable a consumable one; a kind added later is not sold here.
	return Item.Type.Equals(FlockShopItemTypes::Standard, ESearchCase::CaseSensitive)
		|| Item.Type.Equals(FlockShopItemTypes::CurrencyPack, ESearchCase::CaseSensitive)
		|| Item.Type.Equals(FlockShopItemTypes::Consumable, ESearchCase::CaseSensitive);
}

// ── Connect ──

void UFlockStore::Connect(UFlockSubsystem* InFlock, const FFlockStoreOptions& Options, TFunction<void(TFlockResult<UFlockStore*>)> OnComplete)
{
	auto Fail = [&OnComplete](const FFlockError& Error)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<UFlockStore*>::Fail(Error));
		}
	};
	// Empty is a setting left blank: the platform's own, as nothing named is. A platform with no store of its own pays
	// through a web checkout with the game's one active gateway.
	const FString PlatformsOwn = PaymentProviderOfThisPlatform();
	const FString Provider = Options.PaymentProvider.IsEmpty() ? PlatformsOwn : Options.PaymentProvider;
	if (Provider.IsEmpty() || FFlockWebCheckout::IsWebGateway(Provider))
	{
		// A phone's store rules forbid paying for digital goods elsewhere, so a phone never opens a payment page.
		if (!PlatformsOwn.IsEmpty())
		{
			Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation,
				FString::Printf(TEXT("A web checkout ('%s') is not opened on a phone, so nothing was connected"), *Provider),
				TEXT("Leave FFlockStoreOptions::PaymentProvider empty to connect this phone's own store: its rules forbid paying for digital goods on a web page. Web checkout is for Windows, Mac and Linux.")));
			return;
		}
		// Nor a console, whose rules forbid it the same way: web checkout is for Windows, Mac and Linux.
		if (!IsADesktop())
		{
			Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation,
				TEXT("This platform has no store Flock pays through, and a web checkout is opened only on Windows, Mac and Linux, so nothing was connected"),
				TEXT("Sell only items priced in a game currency on this platform: its rules forbid paying for digital goods on a web page.")));
			return;
		}
		ConnectWebCheckout(InFlock, Provider, Options, MoveTemp(OnComplete));
		return;
	}
	if (!IsAStore(Provider))
	{
		Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation,
			FString::Printf(TEXT("'%s' is not a store or a payment gateway Flock pays through, so nothing was connected"), *Provider),
			TEXT("Leave FFlockStoreOptions::PaymentProvider empty for this platform's own way to pay, or name one of FlockPaymentProviders: GooglePlay or AppleAppStore (a store), Stripe, Tap or Moyasar (a web checkout).")));
		return;
	}
	const TSharedPtr<IFlockStoreConnection> Connection = HasStoreLibrary() ? StoreLibrary()() : nullptr;
	if (!Connection.IsValid())
	{
		Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation,
			FString::Printf(TEXT("'%s' is reached through Flock's store module, which is not loaded in this build, so nothing was connected"), *Provider),
			TEXT("Keep the engine's Online Subsystem plugin enabled (Edit > Plugins) and install the Flock plugin whole, then build again. A phone store's rules forbid paying for digital goods elsewhere, so Flock never opens a web page instead. A game with its own store code redeems its purchases with FFlockShopProvider::RedeemStoreReceipt.")));
		return;
	}
	if (Connection->GetPaymentProvider().IsEmpty())
	{
		Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation,
			FString::Printf(TEXT("This build's online subsystem is not a store, so '%s' was not connected"), *Provider),
			TEXT("Connect on Android (Google Play) or iOS (the App Store), where the engine's online subsystem is the platform's store.")));
		return;
	}
	if (!Connection->GetPaymentProvider().Equals(Provider, ESearchCase::CaseSensitive))
	{
		Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation,
			FString::Printf(TEXT("This build reaches the store '%s', not '%s', so nothing was connected"), *Connection->GetPaymentProvider(), *Provider),
			TEXT("Leave FFlockStoreOptions::PaymentProvider empty to connect this platform's own store.")));
		return;
	}
	ConnectThrough(InFlock, Connection.ToSharedRef(), MoveTemp(OnComplete));
}

void UFlockStore::ConnectThrough(UFlockSubsystem* InFlock, const TSharedRef<IFlockStoreConnection>& InConnection,
	TFunction<void(TFlockResult<UFlockStore*>)> OnComplete)
{
	if (!MayConnect(InFlock, OnComplete))
	{
		return;
	}
	UFlockStore* Store = NewObject<UFlockStore>(InFlock);
	Store->Flock = InFlock;
	Store->Connection = InConnection;
	Store->PaymentProvider = InConnection->GetPaymentProvider();
	// A test's SDK saves in a folder of its own, so its purchases are never another SDK's in the same run.
	Store->LaunchKey = InFlock->TestSavedFilesFolder + TEXT("|") + FFlockGameKey::Of(InFlock->ActiveConfig.ApiUrl, InFlock->ActiveConfig.ApiKey)
		+ TEXT("|") + Store->PaymentProvider;
	InFlock->ConnectedStore = Store;
	Store->StartConnecting(MoveTemp(OnComplete));
}

bool UFlockStore::MayConnect(UFlockSubsystem* InFlock, TFunction<void(TFlockResult<UFlockStore*>)>& OnComplete)
{
	auto Fail = [&OnComplete](const FFlockError& Error)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<UFlockStore*>::Fail(Error));
		}
	};
	if (InFlock == nullptr || !InFlock->IsInitialized() || InFlock->GetShopProvider() == nullptr)
	{
		Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation, TEXT("Flock is not initialized, so no way to pay real money was connected"),
			TEXT("Connect once Flock has initialized (UFlockEvents::OnInitialized; in Blueprint, Call Or Register On Initialized on Get Events), with the shop provider built.")));
		return false;
	}
	// One connection a client: two would each send every purchase and raise every event.
	if (InFlock->ConnectedStore != nullptr)
	{
		Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation, TEXT("A way to pay real money is already connected"),
			TEXT("Use the connected one (UFlockSubsystem::GetStore; Flock Get Store in Blueprint), or Disconnect it (Flock Disconnect Store) before connecting again.")));
		return false;
	}
	return true;
}

void UFlockStore::ConnectWebCheckout(UFlockSubsystem* InFlock, const FString& Gateway, const FFlockStoreOptions& Options,
	TFunction<void(TFlockResult<UFlockStore*>)> OnComplete)
{
	if (!MayConnect(InFlock, OnComplete))
	{
		return;
	}
	if (IsSteamTheDefaultService())
	{
		InFlock->GetLogger().LogWarning(TEXT("A web checkout was connected in a build whose engine online service is Steam. Steam allows in-game purchases only through the Steam wallet, so a game sold on Steam must not take payments through a web checkout. Ship this checkout in builds for other storefronts (the Epic Games Store, your own launcher or site)."));
	}
	UFlockStore* Store = NewObject<UFlockStore>(InFlock);
	Store->Flock = InFlock;
	Store->PaymentProvider = Gateway;
	// Copied as connected, so a game changing its options later changes nothing here.
	FFlockWebCheckout::FSettings Settings;
	Settings.Gateway = Gateway;
	Settings.ReturnUrl = Options.ReturnUrl;
	Settings.CancelUrl = Options.CancelUrl;
	Settings.CheckEverySeconds = Options.CheckEverySeconds;
	Settings.OpenPaymentPage = Options.OpenPaymentPage;
	TWeakObjectPtr<UFlockStore> WeakStore(Store);
	Store->Web = MakeShared<FFlockWebCheckout>(InFlock, MoveTemp(Settings), [WeakStore](const FFlockShopItem& Item, const FFlockPurchaseResult& Result)
	{
		if (UFlockStore* Self = WeakStore.Get())
		{
			if (!Self->bDisconnected)
			{
				Self->ReportCompleted(Item, Result);
			}
		}
	}, [WeakStore](const FString& ShopItemId, FFlockShopItem& OutItem)
	{
		const UFlockStore* Self = WeakStore.Get();
		return Self != nullptr && Self->FindItem(ShopItemId, OutItem);
	});
	InFlock->ConnectedStore = Store;
	Store->ReadCatalog(1, MakeShared<TArray<FFlockShop>>(), [WeakStore, OnComplete](TFlockResult<TArray<FFlockShop>> Catalog)
	{
		UFlockStore* Self = WeakStore.Get();
		if (Self == nullptr || Self->bDisconnected)
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<UFlockStore*>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Cancelled,
					TEXT("The store connection was disconnected while it connected"), TEXT("Connect again if the game still wants to sell real-money items."))));
			}
			return;
		}
		if (!Catalog.IsSuccess())
		{
			Self->Disconnect();
			if (OnComplete)
			{
				OnComplete(TFlockResult<UFlockStore*>::Fail(Catalog.Error));
			}
			return;
		}
		for (const FFlockShop& Shop : Catalog.Value)
		{
			for (const FFlockShopItem& Item : Shop.ShopItems)
			{
				if (!Item.Id.IsEmpty())
				{
					Self->ItemsById.Add(Item.Id, Item);
					Self->ShopOfItem.Add(Item.Id, Shop.Id);
				}
			}
		}
		Self->BeReady();
		if (OnComplete)
		{
			OnComplete(TFlockResult<UFlockStore*>::Ok(Self));
		}
	});
}

void UFlockStore::StartConnecting(TFunction<void(TFlockResult<UFlockStore*>)> OnComplete)
{
	TWeakObjectPtr<UFlockStore> WeakThis(this);
	// Listening first: a store hands back unfinished purchases as it connects, and those are held until the shops are read.
	IFlockStoreConnection::FListener Listener;
	Listener.OnPaid = [WeakThis](const FFlockStoreOrder& Order) { if (WeakThis.IsValid()) { WeakThis->HandlePaid(Order); } };
	Listener.OnDeferred = [WeakThis](const FFlockStoreOrder& Order) { if (WeakThis.IsValid()) { WeakThis->HandleDeferred(Order); } };
	Listener.OnFailed = [WeakThis](const FFlockStoreFailure& Failure) { if (WeakThis.IsValid()) { WeakThis->HandleFailed(Failure); } };
	Connection->Listen(MoveTemp(Listener));

	// Ends the connect with Error, disconnecting what it started; a connect the game disconnected meanwhile ends Cancelled.
	auto End = [WeakThis, OnComplete](const FFlockError& Error)
	{
		UFlockStore* Self = WeakThis.Get();
		if (Self == nullptr || Self->bDisconnected)
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<UFlockStore*>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Cancelled,
					TEXT("The store connection was disconnected while it connected"), TEXT("Connect again if the game still wants to sell real-money items."))));
			}
			return;
		}
		if (Error.Type != EFlockErrorType::None)
		{
			Self->Disconnect();
			if (OnComplete)
			{
				OnComplete(TFlockResult<UFlockStore*>::Fail(Error));
			}
			return;
		}
		if (OnComplete)
		{
			OnComplete(TFlockResult<UFlockStore*>::Ok(Self));
		}
	};

	Connection->Connect([WeakThis, End](const FFlockError& ConnectError)
	{
		UFlockStore* Self = WeakThis.Get();
		if (Self == nullptr || Self->bDisconnected || ConnectError.Type != EFlockErrorType::None)
		{
			End(ConnectError);
			return;
		}
		Self->ReadCatalog(1, MakeShared<TArray<FFlockShop>>(), [WeakThis, End](TFlockResult<TArray<FFlockShop>> Catalog)
		{
			UFlockStore* Self = WeakThis.Get();
			if (Self == nullptr || Self->bDisconnected || !Catalog.IsSuccess())
			{
				End(Catalog.IsSuccess() ? FFlockError() : Catalog.Error);
				return;
			}
			TArray<FFlockStoreProduct> Products;
			Self->LearnCatalog(Catalog.Value, Products);
			Self->RememberProducts(Products);
			Self->Connection->RegisterProducts(Products, [WeakThis, End]()
			{
				UFlockStore* Self = WeakThis.Get();
				if (Self != nullptr && !Self->bDisconnected)
				{
					Self->BeReady();
				}
				End(FFlockError());
			});
		});
	});
}

void UFlockStore::ReadCatalog(int32 Page, TSharedRef<TArray<FFlockShop>> Shops, TFunction<void(TFlockResult<TArray<FFlockShop>>)> OnDone)
{
	FFlockShopProvider* Shop = Flock.IsValid() ? Flock->GetShopProvider() : nullptr;
	if (Shop == nullptr)
	{
		OnDone(TFlockResult<TArray<FFlockShop>>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Cancelled,
			TEXT("Flock shut down while the store read the game's shops"), TEXT("Connect again once Flock has initialized."))));
		return;
	}
	TWeakObjectPtr<UFlockStore> WeakThis(this);
	Shop->GetAll(Page, CatalogPageSize, [WeakThis, Page, Shops, OnDone](TFlockResult<FFlockShopPage> Result)
	{
		if (!Result.IsSuccess())
		{
			OnDone(TFlockResult<TArray<FFlockShop>>::Fail(Result.Error));
			return;
		}
		Shops->Append(Result.Value.Items);
		UFlockStore* Self = WeakThis.Get();
		if (Self == nullptr || Result.Value.Items.Num() == 0 || Page * CatalogPageSize >= Result.Value.Total)
		{
			OnDone(TFlockResult<TArray<FFlockShop>>::Ok(*Shops));
			return;
		}
		Self->ReadCatalog(Page + 1, Shops, OnDone);
	});
}

void UFlockStore::LearnCatalog(const TArray<FFlockShop>& Shops, TArray<FFlockStoreProduct>& OutProducts)
{
	IFlockLogger& Log = Flock->GetLogger();
	for (const FFlockShop& Shop : Shops)
	{
		for (const FFlockShopItem& Item : Shop.ShopItems)
		{
			if (Item.Id.IsEmpty())
			{
				continue;
			}
			ItemsById.Add(Item.Id, Item);
			ShopOfItem.Add(Item.Id, Shop.Id);
			FString ProductId;
			if (!Item.FindStoreProductId(PaymentProvider, ProductId) || ProductId.IsEmpty())
			{
				continue;
			}
			if (!IsSoldHere(Item))
			{
				Log.LogWarning(FString::Printf(TEXT("Shop item '%s' is a kind ('%s') Flock's store does not sell; it is left out."), *Item.Name, *Item.Type));
				continue;
			}
			if (const FFlockShopItem* First = ItemByProduct.Find(ProductId))
			{
				if (!First->Id.Equals(Item.Id, ESearchCase::CaseSensitive))
				{
					ProductsSoldTwice.Add(ProductId);
				}
				continue;
			}
			ItemByProduct.Add(ProductId, Item);
			OutProducts.Add({ ProductId, !Item.Type.Equals(FlockShopItemTypes::Standard, ESearchCase::CaseSensitive) });
		}
	}
	for (const FString& ProductId : ProductsSoldTwice)
	{
		// Still registered for Purchase, which names the item; never matched to one when the store hands a purchase back.
		ItemByProduct.Remove(ProductId);
		Log.LogWarning(FString::Printf(TEXT("Two shop items sell the %s product '%s'; a purchase of it is sent to Flock only from Purchase, which names the item. Give each shop item its own product id in the Flock dashboard."),
			*PaymentProvider, *ProductId));
	}
}

void UFlockStore::RememberProducts(TArray<FFlockStoreProduct>& InOutProducts)
{
	UFlockSubsystem* Sdk = Flock.Get();
	if (Sdk == nullptr)
	{
		return;
	}
	// Kept whatever Enable Offline Cache says: without them a paid purchase of an item since taken out of the shops would be
	// left to the game, unfinished, for good. The state scope is never pruned with the cache.
	TSharedPtr<FFlockSnapshotStore> Store = Sdk->SnapshotStore;
	if (!Store.IsValid())
	{
		const FString Root = Sdk->TestSavedFilesFolder.IsEmpty()
			? GetDefault<UFlockConfig>()->OfflineCacheDirectory : FPaths::Combine(Sdk->TestSavedFilesFolder, TEXT("snapshots"));
		Store = MakeShared<FFlockSnapshotStore>(Root, Sdk->Logger.ToSharedRef(), UFlockSubsystem::SdkVersion);
	}
	FFlockSnapshotStore& Disk = *Store;
	// Per game and store: another game's products never reach this one.
	const FString Key = FFlockGameKey::Named(TEXT("products-") + PaymentProvider, FFlockGameKey::Of(Sdk->ActiveConfig.ApiUrl, Sdk->ActiveConfig.ApiKey));
	TSharedPtr<FJsonObject> Sold;
	FString Saved;
	if (!Disk.TryRead(SoldProductsScope(), Key, Saved)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Saved), Sold) || !Sold.IsValid())
	{
		Sold = MakeShared<FJsonObject>();
	}
	// The products sold before that no item sells now: the store must still hand their purchases back.
	for (const auto& Product : Sold->Values)
	{
		const FString ProductId(*Product.Key);
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		FString ShopItemId;
		bool bConsumable = false;
		if (ItemByProduct.Contains(ProductId) || ProductsSoldTwice.Contains(ProductId) || !Product.Value.IsValid()
			|| !Product.Value->TryGetObject(Entry) || !(*Entry)->TryGetStringField(TEXT("shop_item_id"), ShopItemId) || ShopItemId.IsEmpty())
		{
			continue;
		}
		(*Entry)->TryGetBoolField(TEXT("consumable"), bConsumable);
		SoldBefore.Add(ProductId, ShopItemId);
		InOutProducts.Add({ ProductId, bConsumable });
	}
	for (const auto& Product : ItemByProduct)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("shop_item_id"), Product.Value.Id);
		Entry->SetBoolField(TEXT("consumable"), !Product.Value.Type.Equals(FlockShopItemTypes::Standard, ESearchCase::CaseSensitive));
		Sold->SetObjectField(Product.Key, Entry);
	}
	FString Payload;
	FJsonSerializer::Serialize(Sold.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Payload));
	Disk.Write(SoldProductsScope(), Key, Payload);
}

void UFlockStore::BeReady()
{
	bReady = true;
	if (FFlockShopProvider* Shop = Flock->GetShopProvider())
	{
		Shop->SetConnectedStore(this);
	}
	UFlockEvents* Events = Flock->GetEvents();
	Events->OnAuthenticated.AddUniqueDynamic(this, &UFlockStore::HandleSignedIn);
	Events->OnLoggedOut.AddUniqueDynamic(this, &UFlockStore::HandleSignInEnded);
	Events->OnAuthExpired.AddUniqueDynamic(this, &UFlockStore::HandleSignInEnded);
	if (IsSignedIn())
	{
		HandleSignedIn(FFlockAuthInfo());
	}
}

// ── Disconnect ──

void UFlockStore::Disconnect()
{
	if (bDisconnected)
	{
		return;
	}
	bDisconnected = true;
	if (UFlockSubsystem* Sdk = Flock.Get())
	{
		if (Sdk->ConnectedStore == this)
		{
			Sdk->ConnectedStore = nullptr;
		}
		if (FFlockShopProvider* Shop = Sdk->GetShopProvider())
		{
			if (Shop->GetConnectedStore() == this)
			{
				Shop->SetConnectedStore(nullptr);
			}
		}
		if (UFlockEvents* Events = Sdk->GetEvents())
		{
			Events->OnAuthenticated.RemoveDynamic(this, &UFlockStore::HandleSignedIn);
			Events->OnLoggedOut.RemoveDynamic(this, &UFlockStore::HandleSignInEnded);
			Events->OnAuthExpired.RemoveDynamic(this, &UFlockStore::HandleSignInEnded);
		}
	}
	if (Connection.IsValid())
	{
		Connection->StopListening();
	}
	if (Web.IsValid())
	{
		Web->Dispose();
	}
	Waiting.Empty();
	WaitingForBuyer.Empty();
	if (CurrentRestore.IsValid())
	{
		const TSharedPtr<FRestoring> Ending = CurrentRestore;
		CurrentRestore.Reset();
		Ending->End(TFlockResult<FFlockStoreRestore>::Fail(RestoreCancelled()));
	}
	EndEveryBuying(Refusal(EFlockErrorType::Cancelled,
		TEXT("The store connection was disconnected while the purchase was waited for"),
		TEXT("Nothing needs fixing. A purchase the player pays for afterwards stays unfinished in the store and is delivered through the next connection.")));
}

void UFlockStore::BeginDestroy()
{
	if (!bDisconnected && Connection.IsValid())
	{
		Connection->StopListening();
	}
	bDisconnected = true;
	Super::BeginDestroy();
}

// ── Purchase ──

void UFlockStore::Purchase(const FString& ShopItemId, TFunction<void(TFlockResult<FFlockPurchaseResult>)> OnComplete)
{
	auto Fail = [&OnComplete](const FFlockError& Error)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockPurchaseResult>::Fail(Error));
		}
	};
	FFlockError Refused;
	FFlockShopItem Item;
	FString ProductId;
	if (Web.IsValid())
	{
		if (!IsUsable(Refused))
		{
			Fail(Refused);
			return;
		}
		if (ShopItemId.IsEmpty())
		{
			Fail(Refusal(EFlockErrorType::Validation, TEXT("Shop Item ID is empty"), TEXT("Pass the shop item's Id (FFlockShopItem::Id).")));
			return;
		}
		if (!IsSignedIn())
		{
			Fail(Refusal(EFlockErrorType::Auth, NotSignedInMessage(), FFlockErrorHints::SignInFirst()));
			return;
		}
		// Read now rather than at connect, so an item added or repriced in the dashboard since then is sold as it is now.
		const int32 SignIn = CurrentSignIn();
		TWeakObjectPtr<UFlockStore> WeakThis(this);
		Flock->GetShopProvider()->GetItem(ShopItemId, [WeakThis, SignIn, OnComplete](TFlockResult<FFlockShopItem> Read)
		{
			auto Answer = [&OnComplete](const FFlockError& Error)
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockPurchaseResult>::Fail(Error));
				}
			};
			UFlockStore* Self = WeakThis.Get();
			FFlockError Refused;
			if (Self == nullptr || !Self->IsUsable(Refused))
			{
				Answer(Self == nullptr ? Refusal(EFlockErrorType::Cancelled, TEXT("The store connection went away while the item was read"),
					TEXT("Connect again with UFlockStore::Connect.")) : Refused);
				return;
			}
			if (!Read.IsSuccess())
			{
				Answer(Read.Error);
				return;
			}
			if (!Read.Value.IsRealMoney)
			{
				Answer(Refusal(EFlockErrorType::Validation, FString::Printf(TEXT("'%s' is priced in a game currency, so no web checkout was opened"), *Read.Value.Name),
					TEXT("Buy it with FFlockShopProvider::Purchase, which takes a game-currency item from the wallet.")));
				return;
			}
			// Bought for the player who asked, never for one who signed in while the item was read.
			if (Self->CurrentSignIn() != SignIn || !Self->IsSignedIn())
			{
				Answer(Refusal(EFlockErrorType::Cancelled, TEXT("The player signed out or changed while the item was read, so nothing was bought"),
					TEXT("Nothing needs fixing: buy it again as the player now signed in, if they want it.")));
				return;
			}
			Self->Web->Purchase(Read.Value, OnComplete);
		});
		return;
	}
	if (!IsUsable(Refused) || !ProductOf(ShopItemId, Item, ProductId, Refused))
	{
		Fail(Refused);
		return;
	}
	// The store keeps one purchase dialog at a time, and its answers name only the product, so one purchase is under way at a
	// time: a second would be dropped by the store or take the first's answer.
	if (Buying.Num() > 0 || !DialogProduct.IsEmpty())
	{
		Fail(Refusal(EFlockErrorType::Validation, TEXT("Another store purchase is under way"),
			TEXT("Wait for it to end, and for the store's purchase dialog to close, before starting another. If no dialog is on screen any more (the store never answered it), Disconnect the store and connect again: a purchase paid meanwhile is handed back to the new connection.")));
		return;
	}
	if (!IsSignedIn())
	{
		Fail(Refusal(EFlockErrorType::Auth, NotSignedInMessage(), FFlockErrorHints::SignInFirst()));
		return;
	}
	// The sign-in the purchase is for, so the store never charges for a player who signed in while the item was read.
	const int32 SignIn = CurrentSignIn();
	const int32 Number = ++BuyingCount;
	// Taken before the read, so a second call for this product meanwhile is refused rather than overwriting this one.
	FBuying& Entry = Buying.Add(ProductId);
	Entry.Number = Number;
	Entry.Item = Item;
	Entry.OnComplete = MoveTemp(OnComplete);

	const FString* ShopId = ShopOfItem.Find(Item.Id);
	TWeakObjectPtr<UFlockStore> WeakThis(this);
	// Switched off in the dashboard since the store connected: refused before the store takes the payment.
	Flock->GetShopProvider()->IsForSale(ShopId ? *ShopId : FString(), Item.Id, [WeakThis, ProductId, Number, SignIn, Item](TFlockResult<bool> ForSale)
	{
		UFlockStore* Self = WeakThis.Get();
		if (Self == nullptr)
		{
			return;
		}
		const FBuying* Current = Self->Buying.Find(ProductId);
		if (Current == nullptr || Current->Number != Number)
		{
			return; // Ended meanwhile (disconnected, or its sign-in ended).
		}
		if (!ForSale.IsSuccess())
		{
			Self->EndBuying(ProductId, Number, TFlockResult<FFlockPurchaseResult>::Fail(ForSale.Error));
			return;
		}
		if (!ForSale.Value)
		{
			FFlockError NotForSale = Refusal(EFlockErrorType::Validation,
				FString::Printf(TEXT("'%s' is not for sale right now, so the store was not asked to charge for it"), *Item.Name),
				FFlockErrorHints::For(EFlockErrorCode::ShopItemNotForSale));
			NotForSale.Code = TEXT("shop.item_not_for_sale");
			NotForSale.ErrorCode = EFlockErrorCode::ShopItemNotForSale;
			Self->EndBuying(ProductId, Number, TFlockResult<FFlockPurchaseResult>::Fail(NotForSale));
			return;
		}
		// Bought for the player who asked, never for one who signed in while the item was read.
		if (Self->CurrentSignIn() != SignIn || !Self->IsSignedIn())
		{
			Self->EndBuying(ProductId, Number, TFlockResult<FFlockPurchaseResult>::Fail(Refusal(EFlockErrorType::Cancelled,
				TEXT("The player signed out or changed while the item was read, so nothing was bought"),
				TEXT("Nothing needs fixing: buy it again as the player now signed in, if they want it."))));
			return;
		}
		Self->Buying.FindChecked(ProductId).SignIn = SignIn;
		Self->DialogProduct = ProductId;
		Self->DialogBuyer = Self->CurrentPlayerId();
		Self->Connection->Buy(ProductId);
	});
}

void UFlockStore::EndBuying(const FString& ProductId, int32 Number, const TFlockResult<FFlockPurchaseResult>& Result)
{
	const TFunction<void(TFlockResult<FFlockPurchaseResult>)> OnComplete = TakeBuying(ProductId, Number);
	if (OnComplete)
	{
		OnComplete(Result);
	}
}

TFunction<void(TFlockResult<FFlockPurchaseResult>)> UFlockStore::TakeBuying(const FString& ProductId, int32 Number)
{
	FBuying* Entry = Buying.Find(ProductId);
	if (Entry == nullptr || Entry->Number != Number)
	{
		return nullptr;
	}
	TFunction<void(TFlockResult<FFlockPurchaseResult>)> OnComplete = MoveTemp(Entry->OnComplete);
	Buying.Remove(ProductId);
	return OnComplete;
}

void UFlockStore::EndBuyingIfSignInEnded()
{
	const int32 Now = CurrentSignIn();
	TArray<TPair<FString, int32>> Ended;
	for (const auto& Entry : Buying)
	{
		if (Entry.Value.SignIn != INDEX_NONE && Entry.Value.SignIn != Now)
		{
			Ended.Emplace(Entry.Key, Entry.Value.Number);
		}
	}
	for (const TPair<FString, int32>& Entry : Ended)
	{
		EndBuying(Entry.Key, Entry.Value, TFlockResult<FFlockPurchaseResult>::Fail(Refusal(EFlockErrorType::Cancelled,
			TEXT("The player signed out or changed while the store's purchase was waited for"),
			TEXT("Nothing needs fixing: once paid, the purchase is delivered when the player who bought it signs in again during this run of the game (after a restart, to the player signed in then), or through Restore."))));
	}
}

void UFlockStore::EndEveryBuying(const FFlockError& Error)
{
	TArray<TPair<FString, int32>> All;
	for (const auto& Entry : Buying)
	{
		All.Emplace(Entry.Key, Entry.Value.Number);
	}
	for (const TPair<FString, int32>& Entry : All)
	{
		EndBuying(Entry.Key, Entry.Value, TFlockResult<FFlockPurchaseResult>::Fail(Error));
	}
}

// ── What the store hands over ──

void UFlockStore::HandleSignedIn(const FFlockAuthInfo& Info)
{
	if (bDisconnected || !bReady)
	{
		return;
	}
	if (Web.IsValid())
	{
		Web->HandleSignedIn();
		return;
	}
	EndBuyingIfSignInEnded();
	// Purchases that waited for a sign-in go now, and the store is asked for any it still holds (Google hands them back only when asked).
	TArray<FFlockStoreOrder> Held = MoveTemp(Waiting);
	Waiting.Reset();
	const FString Player = CurrentPlayerId();
	for (int32 Index = WaitingForBuyer.Num() - 1; Index >= 0; --Index)
	{
		if (WaitingForBuyer[Index].PlayerId.Equals(Player, ESearchCase::CaseSensitive))
		{
			Held.Add(WaitingForBuyer[Index].Order);
			WaitingForBuyer.RemoveAt(Index);
		}
	}
	for (const FFlockStoreOrder& Order : Held)
	{
		HandlePaid(Order);
	}
	Connection->FetchUnfinishedPurchases();
}

void UFlockStore::HandleSignInEnded()
{
	if (Web.IsValid())
	{
		Web->HandleSignInEnded();
		return;
	}
	EndBuyingIfSignInEnded();
}

void UFlockStore::HandlePaid(const FFlockStoreOrder& Order)
{
	if (bDisconnected || !Flock.IsValid() || !Flock->IsInitialized() || Order.ProductId.IsEmpty() || Order.TransactionId.IsEmpty())
	{
		return;
	}
	if (!bReady)
	{
		Hold(Order);
		return;
	}
	const FBuying* Entry = CallAnsweredBy(Order.ProductId, Order.bAnswersThePurchaseDialog);
	const FString Buyer = Order.bAnswersThePurchaseDialog ? CloseTheDialog(Order.ProductId) : FString();
	const int32 Number = Entry != nullptr ? Entry->Number : INDEX_NONE;
	const FFlockShopItem* Item = Entry != nullptr ? &Entry->Item : ItemByProduct.Find(Order.ProductId);
	if (Item == nullptr && !ProductsSoldTwice.Contains(Order.ProductId) && !SoldBefore.Contains(Order.ProductId))
	{
		return; // Not a Flock item's product: the game finishes its own purchases.
	}
	// The dialog's answer after its call ended (the player signed out or changed): kept for the player who opened it, since
	// a receipt sent now would be delivered to whoever is signed in. The store hands the same purchase back again at each
	// fetch, not as the dialog's answer, so the hold is looked up by its id too.
	const int32 HeldFor = WaitingForBuyer.IndexOfByPredicate([&Order](const FWaitingForBuyer& Held)
	{
		return Held.Order.TransactionId.Equals(Order.TransactionId, ESearchCase::CaseSensitive);
	});
	const FString HeldBuyer = HeldFor != INDEX_NONE ? WaitingForBuyer[HeldFor].PlayerId : Buyer;
	if (Entry == nullptr && !HeldBuyer.IsEmpty() && (!IsSignedIn() || !CurrentPlayerId().Equals(HeldBuyer, ESearchCase::CaseSensitive)))
	{
		if (HeldFor == INDEX_NONE)
		{
			FFlockStoreOrder Kept = Order;
			Kept.bAnswersThePurchaseDialog = false;
			WaitingForBuyer.Add({ HeldBuyer, Kept });
		}
		return;
	}
	if (HeldFor != INDEX_NONE)
	{
		WaitingForBuyer.RemoveAt(HeldFor);
	}
	if (!IsSignedIn())
	{
		Hold(Order);
		return;
	}
	if (SentThisLaunch().Contains(Order.TransactionId))
	{
		return;
	}
	SentThisLaunch().Add(Order.TransactionId);
	if (Item == nullptr)
	{
		if (const FString* SoldItemId = SoldBefore.Find(Order.ProductId))
		{
			SendForAnItemNoLongerSold(Order, *SoldItemId);
			return;
		}
		RaiseNotDelivered(nullptr, Order.ProductId, SoldTwice(Order.ProductId, TEXT(" The purchase stays unfinished in the store and is matched at the next launch.")));
		return;
	}
	Send(Order, *Item, Number);
}

const UFlockStore::FBuying* UFlockStore::CallAnsweredBy(const FString& ProductId, bool bAnswersThePurchaseDialog) const
{
	if (!bAnswersThePurchaseDialog || !DialogProduct.Equals(ProductId, ESearchCase::CaseSensitive))
	{
		return nullptr;
	}
	const FBuying* Entry = Buying.Find(ProductId);
	return Entry != nullptr && Entry->SignIn != INDEX_NONE ? Entry : nullptr;
}

FString UFlockStore::CloseTheDialog(const FString& ProductId)
{
	if (!DialogProduct.Equals(ProductId, ESearchCase::CaseSensitive))
	{
		return FString();
	}
	const FString Buyer = DialogBuyer;
	DialogProduct.Reset();
	DialogBuyer.Reset();
	return Buyer;
}

void UFlockStore::Send(const FFlockStoreOrder& Order, const FFlockShopItem& Item, int32 Number)
{
	BeingSent().Add(Order.TransactionId);
	TWeakObjectPtr<UFlockStore> WeakThis(this);
	const FString Key = LaunchKey;
	Flock->GetShopProvider()->RedeemStoreReceipt(Item.Id, ReceiptOf(Order),
		[WeakThis, Key, Order, Item, Number](TFlockResult<FFlockPurchaseResult> Delivery)
	{
		// No longer being sent, whatever became of this connection.
		BeingSentFor(Key).Remove(Order.TransactionId);
		UFlockStore* Self = WeakThis.Get();
		if (Self == nullptr)
		{
			return;
		}
		// Disconnected meanwhile: nothing is finished or raised, so the store hands the purchase back to the next connection,
		// where Flock answers the repeat with what this one delivered.
		if (Self->bDisconnected)
		{
			return;
		}
		// The event comes before the call's answer, so a caller reading what the event keeps finds it there.
		if (!Delivery.IsSuccess())
		{
			// Refused for the sign-in's token (a 401: rejected and not renewed, or nobody signed in), or never sent once the
			// sign-in ended: it never reached the redemption, so it stays unfinished, unreported, and the next sign-in's fetch
			// sends it. A 403 is the server's answer about the receipt itself (another player's), and is reported.
			const bool bNeverRedeemed = Delivery.Error.Type == EFlockErrorType::Cancelled
				|| (Delivery.Error.Type == EFlockErrorType::Auth && Delivery.Error.StatusCode != 403);
			// Off the books first, so a handler of the event can start another purchase; answered after the event.
			const TFunction<void(TFlockResult<FFlockPurchaseResult>)> Answer = Self->TakeBuying(Order.ProductId, Number);
			if (bNeverRedeemed)
			{
				Self->SentThisLaunch().Remove(Order.TransactionId);
			}
			else
			{
				Self->RaiseNotDelivered(&Item, Order.ProductId, Delivery.Error);
			}
			if (Answer)
			{
				Answer(TFlockResult<FFlockPurchaseResult>::Fail(Delivery.Error));
			}
			return;
		}
		// Finished only once Flock delivered; a finish the store cannot make leaves it to be handed back and answered as a repeat.
		Self->Connection->Finish(Order);
		const TFunction<void(TFlockResult<FFlockPurchaseResult>)> Answer = Self->TakeBuying(Order.ProductId, Number);
		Self->ReportCompleted(Item, Delivery.Value);
		if (Answer)
		{
			Answer(Delivery);
		}
	});
}

void UFlockStore::SendForAnItemNoLongerSold(const FFlockStoreOrder& Order, const FString& ShopItemId)
{
	BeingSent().Add(Order.TransactionId);
	TWeakObjectPtr<UFlockStore> WeakThis(this);
	const FString Key = LaunchKey;
	// Its own read still finds a switched-off item, and Flock delivers what was paid for or says why not.
	Flock->GetShopProvider()->GetItem(ShopItemId, [WeakThis, Key, Order](TFlockResult<FFlockShopItem> Read)
	{
		UFlockStore* Self = WeakThis.Get();
		if (Self == nullptr || Self->bDisconnected || !Read.IsSuccess())
		{
			BeingSentFor(Key).Remove(Order.TransactionId);
			if (Self != nullptr && !Self->bDisconnected)
			{
				Self->RaiseNotDelivered(nullptr, Order.ProductId, Read.Error);
			}
			return;
		}
		Self->Send(Order, Read.Value, INDEX_NONE);
	});
}

void UFlockStore::Hold(const FFlockStoreOrder& Order)
{
	const bool bHeld = Waiting.ContainsByPredicate([&Order](const FFlockStoreOrder& Held)
	{
		return Held.TransactionId.Equals(Order.TransactionId, ESearchCase::CaseSensitive);
	});
	if (!bHeld)
	{
		Waiting.Add(Order);
	}
}

void UFlockStore::HandleDeferred(const FFlockStoreOrder& Order)
{
	// The store waits for the payment or a parent's approval: the call answers pending, and OnPurchaseCompleted follows once paid.
	// The dialog is closed; the payment comes later as a purchase the store hands back.
	const FBuying* Entry = CallAnsweredBy(Order.ProductId, /*bAnswersThePurchaseDialog*/ true);
	CloseTheDialog(Order.ProductId);
	if (Entry != nullptr)
	{
		FFlockPurchaseResult Pending;
		Pending.ItemType = Entry->Item.Type;
		Pending.Status = FlockPurchaseStatuses::Pending;
		EndBuying(Order.ProductId, Entry->Number, TFlockResult<FFlockPurchaseResult>::Ok(Pending));
	}
}

void UFlockStore::HandleFailed(const FFlockStoreFailure& Failure)
{
	const FBuying* Entry = CallAnsweredBy(Failure.ProductId, /*bAnswersThePurchaseDialog*/ true);
	CloseTheDialog(Failure.ProductId);
	if (Entry == nullptr)
	{
		return;
	}
	const FString Reason = Failure.Reason.IsEmpty() ? FString(FlockStoreFailureReasons::Unknown) : Failure.Reason;
	const bool bCancelled = Reason.Equals(FlockStoreFailureReasons::Cancelled, ESearchCase::CaseSensitive);
	FFlockError Error = Refusal(EFlockErrorType::Store,
		bCancelled ? FString(TEXT("The player cancelled the purchase in the store's dialog")) : TEXT("The store did not take the payment: ") + Failure.Text,
		FixFor(Reason));
	Error.StoreFailureReason = Reason;
	EndBuying(Failure.ProductId, Entry->Number, TFlockResult<FFlockPurchaseResult>::Fail(Error));
}

FString UFlockStore::FixFor(const FString& Reason)
{
	using namespace FlockStoreFailureReasons;
	auto Is = [&Reason](const TCHAR* Name) { return Reason.Equals(Name, ESearchCase::CaseSensitive); };
	if (Is(Cancelled)) { return TEXT("Nothing was charged. Offer it again if the player wants it."); }
	if (Is(AlreadyOwned)) { return TEXT("This store account already owns the one-time item: call Restore to deliver it, and show it as owned."); }
	if (Is(AlreadyPending)) { return TEXT("An earlier purchase of it is waiting for its payment; OnPurchaseCompleted reports it once paid."); }
	if (Is(PaymentDeclined)) { return TEXT("Nothing was charged. The player can choose another payment method in the store and try again."); }
	if (Is(ProductUnavailable)) { return TEXT("Check the item's product id under Store product IDs in the Flock dashboard matches an active product in the store; CheckSetup lists the ones the store did not find."); }
	if (Is(StoreUnavailable)) { return TEXT("Nothing was charged. Check the device is online and that purchases are allowed on it; on Google Play the payment method may also have been declined, so the player can choose another in the store. Then try again."); }
	if (Is(NotSignedInToTheStore)) { return TEXT("Ask the player to sign in to the store on this device, then try again."); }
	return TEXT("Nothing was charged. Try again; if it keeps failing, CheckSetup checks the store's setup.");
}

void UFlockStore::RaiseNotDelivered(const FFlockShopItem* Item, const FString& ProductId, const FFlockError& Error)
{
	FFlockUndeliveredPurchase Purchase;
	Purchase.bHasItem = Item != nullptr;
	if (Item != nullptr)
	{
		Purchase.Item = *Item;
	}
	Purchase.ProductId = ProductId;
	Purchase.Error = Error;
	OnPurchaseNotDelivered.Broadcast(Purchase);
	if (UFlockSubsystem* Sdk = Flock.Get())
	{
		if (UFlockEvents* Events = Sdk->GetEvents())
		{
			Events->InvokeStorePurchaseNotDelivered(Purchase);
		}
	}
}

void UFlockStore::ReportCompleted(const FFlockShopItem& Item, const FFlockPurchaseResult& Result)
{
	OnPurchaseCompleted.Broadcast(Item, Result);
	if (UFlockSubsystem* Sdk = Flock.Get())
	{
		if (UFlockEvents* Events = Sdk->GetEvents())
		{
			Events->InvokeStorePurchaseCompleted(Item, Result);
		}
	}
}

// ── Price, restore, setup check ──

TFlockResult<FFlockStorePrice> UFlockStore::GetPrice(const FString& ShopItemId) const
{
	FFlockError Refused;
	FFlockShopItem Item;
	FString ProductId;
	if (Web.IsValid())
	{
		if (!IsUsable(Refused) || !KnownItem(ShopItemId, Item, Refused))
		{
			return TFlockResult<FFlockStorePrice>::Fail(Refused);
		}
		if (!Item.IsRealMoney)
		{
			return TFlockResult<FFlockStorePrice>::Fail(Refusal(EFlockErrorType::Validation,
				FString::Printf(TEXT("'%s' is priced in a game currency, so it has no real-money price"), *Item.Name),
				TEXT("Show a game-currency item's Price and Currency as they are; GetPrice is for real-money items.")));
		}
		return TFlockResult<FFlockStorePrice>::Ok(PriceOfTheItem(Item));
	}
	if (!IsUsable(Refused) || !ProductOf(ShopItemId, Item, ProductId, Refused))
	{
		return TFlockResult<FFlockStorePrice>::Fail(Refused);
	}
	return TFlockResult<FFlockStorePrice>::Ok(Connection->PriceOf(ProductId));
}

FFlockStorePrice UFlockStore::PriceOfTheItem(const FFlockShopItem& Item)
{
	// Flock's server counts these in thousandths or in whole units, and every other currency in hundredths.
	static const TSet<FString> ThreeDigitCurrencies = { TEXT("BHD"), TEXT("IQD"), TEXT("JOD"), TEXT("KWD"), TEXT("LYD"), TEXT("OMR"), TEXT("TND") };
	static const TSet<FString> WholeUnitCurrencies = { TEXT("BIF"), TEXT("CLP"), TEXT("DJF"), TEXT("GNF"), TEXT("ISK"), TEXT("JPY"), TEXT("KMF"),
		TEXT("KRW"), TEXT("PYG"), TEXT("RWF"), TEXT("UGX"), TEXT("VND"), TEXT("VUV"), TEXT("XAF"), TEXT("XOF"), TEXT("XPF") };
	const FString Currency = Item.Currency.ToUpper();
	const int32 Digits = ThreeDigitCurrencies.Contains(Currency) ? 3 : WholeUnitCurrencies.Contains(Currency) ? 0 : 2;
	double Amount = static_cast<double>(Item.Price);
	for (int32 Index = 0; Index < Digits; ++Index)
	{
		Amount /= 10.0;
	}
	FNumberFormattingOptions Format;
	Format.MinimumFractionalDigits = Digits;
	Format.MaximumFractionalDigits = Digits;
	FFlockStorePrice Price;
	Price.bFoundInTheStore = true;
	Price.AmountInSmallestUnit = Item.Price;
	Price.CurrencyCode = Currency;
	Price.Text = Currency + TEXT(" ") + FText::AsNumber(Amount, &Format).ToString();
	return Price;
}

void UFlockStore::Restore(TFunction<void(TFlockResult<FFlockStoreRestore>)> OnComplete)
{
	FFlockError Refused;
	if (!IsUsable(Refused))
	{
		OnComplete(TFlockResult<FFlockStoreRestore>::Fail(Refused));
		return;
	}
	if (!IsSignedIn())
	{
		OnComplete(TFlockResult<FFlockStoreRestore>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Auth, NotSignedInMessage(), FFlockErrorHints::SignInFirst())));
		return;
	}
	if (Web.IsValid())
	{
		// Flock already holds what the player bought: the checkouts still open are read again, and each one paid now reported.
		Web->FindOpenCheckouts([OnComplete](const FFlockError& Error)
		{
			OnComplete(Error.Type == EFlockErrorType::None ? TFlockResult<FFlockStoreRestore>::Ok(FFlockStoreRestore())
				: TFlockResult<FFlockStoreRestore>::Fail(Error));
		});
		return;
	}
	if (CurrentRestore.IsValid() && !CurrentRestore->bEnded)
	{
		OnComplete(TFlockResult<FFlockStoreRestore>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation, TEXT("A restore is already under way"),
			TEXT("Wait for it to complete; it delivers every purchase the store account owns."))));
		return;
	}
	TSharedRef<FRestoring> Restoring = MakeShared<FRestoring>();
	CurrentRestore = Restoring;
	// The sign-in the restore is for: the store's purchases are never sent as a player who signed in meanwhile.
	Restoring->SignIn = CurrentSignIn();
	Restoring->OnComplete = MoveTemp(OnComplete);
	// The player asked: an unfinished purchase whose send this launch has ended goes again when the store hands it over.
	SentThisLaunch() = SentThisLaunch().Intersect(BeingSent());
	TWeakObjectPtr<UFlockStore> WeakThis(this);
	Connection->FetchOwnedPurchases([WeakThis, Restoring](TFlockResult<TArray<FFlockStoreOrder>> Owned)
	{
		UFlockStore* Self = WeakThis.Get();
		if (Self == nullptr || Self->bDisconnected)
		{
			Restoring->End(TFlockResult<FFlockStoreRestore>::Fail(RestoreCancelled()));
			return;
		}
		if (!Owned.IsSuccess())
		{
			Restoring->End(TFlockResult<FFlockStoreRestore>::Fail(Owned.Error));
			return;
		}
		Restoring->Owned = Owned.Value;
		Self->RestoreFrom(Restoring, 0);
	});
}

void UFlockStore::RestoreFrom(const TSharedRef<FRestoring>& Restoring, int32 Index)
{
	if (Restoring->bEnded)
	{
		return;
	}
	if (bDisconnected || !Flock.IsValid() || Flock->GetShopProvider() == nullptr)
	{
		Restoring->End(TFlockResult<FFlockStoreRestore>::Fail(RestoreCancelled()));
		return;
	}
	for (; Index < Restoring->Owned.Num(); ++Index)
	{
		const FFlockStoreOrder Order = Restoring->Owned[Index];
		// A purchase being sent now is left to its send: it is reported through the events as usual.
		if (Order.ProductId.IsEmpty() || Order.TransactionId.IsEmpty() || BeingSent().Contains(Order.TransactionId))
		{
			continue;
		}
		if (ProductsSoldTwice.Contains(Order.ProductId))
		{
			FFlockUndeliveredPurchase& NotDelivered = Restoring->Found.NotDelivered.AddDefaulted_GetRef();
			NotDelivered.ProductId = Order.ProductId;
			NotDelivered.Error = SoldTwice(Order.ProductId, TEXT(" Then restore again."));
			continue;
		}
		if (const FFlockShopItem* Item = ItemByProduct.Find(Order.ProductId))
		{
			RestoreOne(Restoring, Index, Order, *Item);
			return;
		}
		// Not a Flock item's product: the game restores its own purchases.
		const FString* SoldItemId = SoldBefore.Find(Order.ProductId);
		if (SoldItemId == nullptr)
		{
			continue;
		}
		TWeakObjectPtr<UFlockStore> WeakThis(this);
		Flock->GetShopProvider()->GetItem(*SoldItemId, [WeakThis, Restoring, Index, Order](TFlockResult<FFlockShopItem> Read)
		{
			UFlockStore* Self = WeakThis.Get();
			if (Self == nullptr || Self->bDisconnected)
			{
				Restoring->End(TFlockResult<FFlockStoreRestore>::Fail(RestoreCancelled()));
				return;
			}
			if (!Read.IsSuccess())
			{
				FFlockUndeliveredPurchase& NotDelivered = Restoring->Found.NotDelivered.AddDefaulted_GetRef();
				NotDelivered.ProductId = Order.ProductId;
				NotDelivered.Error = Read.Error;
				Self->RestoreFrom(Restoring, Index + 1);
				return;
			}
			Self->RestoreOne(Restoring, Index, Order, Read.Value);
		});
		return;
	}
	Restoring->End(TFlockResult<FFlockStoreRestore>::Ok(Restoring->Found));
}

void UFlockStore::RestoreOne(const TSharedRef<FRestoring>& Restoring, int32 Index, const FFlockStoreOrder& Order, const FFlockShopItem& Item)
{
	if (bDisconnected || CurrentSignIn() != Restoring->SignIn || !IsSignedIn())
	{
		Restoring->End(TFlockResult<FFlockStoreRestore>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Cancelled,
			TEXT("The player signed out or changed while the store's purchases were restored, so the rest were not sent"),
			TEXT("Restore again as the player now signed in."))));
		return;
	}
	BeingSent().Add(Order.TransactionId);
	TWeakObjectPtr<UFlockStore> WeakThis(this);
	const FString Key = LaunchKey;
	Flock->GetShopProvider()->RedeemStoreReceipt(Item.Id, ReceiptOf(Order), [WeakThis, Key, Restoring, Index, Order, Item](TFlockResult<FFlockPurchaseResult> Delivery)
	{
		BeingSentFor(Key).Remove(Order.TransactionId);
		UFlockStore* Self = WeakThis.Get();
		if (Self == nullptr || Self->bDisconnected)
		{
			Restoring->End(TFlockResult<FFlockStoreRestore>::Fail(RestoreCancelled()));
			return;
		}
		if (Delivery.IsSuccess())
		{
			Restoring->Found.Restored.Add(Item);
		}
		else if (Delivery.Error.Type == EFlockErrorType::Cancelled)
		{
			// The sign-in ended while it was sent: the rest are not sent either.
			Restoring->End(TFlockResult<FFlockStoreRestore>::Fail(Delivery.Error));
			return;
		}
		else
		{
			FFlockUndeliveredPurchase& NotDelivered = Restoring->Found.NotDelivered.AddDefaulted_GetRef();
			NotDelivered.bHasItem = true;
			NotDelivered.Item = Item;
			NotDelivered.ProductId = Order.ProductId;
			NotDelivered.Error = Delivery.Error;
		}
		Self->RestoreFrom(Restoring, Index + 1);
	});
}

void UFlockStore::CheckSetup(TFunction<void(TFlockResult<FFlockStoreSetupCheck>)> OnComplete)
{
	FFlockError Refused;
	if (!IsUsable(Refused))
	{
		OnComplete(TFlockResult<FFlockStoreSetupCheck>::Fail(Refused));
		return;
	}
	if (!IsSignedIn())
	{
		OnComplete(TFlockResult<FFlockStoreSetupCheck>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Auth, NotSignedInMessage(), FFlockErrorHints::SignInFirst())));
		return;
	}
	FFlockStoreSetupCheck Check;
	if (Web.IsValid())
	{
		Check.NotChecked = TEXT("The payment gateway: Flock's server says whether one is connected only as a checkout opens. Buy a real-money item with the gateway's test keys connected in the Flock dashboard (Monetization > Payments) and its test card.");
		OnComplete(TFlockResult<FFlockStoreSetupCheck>::Ok(Check));
		return;
	}
	const FFlockShopItem* AskAbout = nullptr;
	FString AskedProduct;
	for (const auto& Entry : ItemsById)
	{
		const FFlockShopItem& Item = Entry.Value;
		if (!Item.IsRealMoney || !IsSoldHere(Item))
		{
			continue;
		}
		FString ProductId;
		if (!Item.FindStoreProductId(PaymentProvider, ProductId) || ProductId.IsEmpty())
		{
			Check.MissingProductId.Add(Item);
			continue;
		}
		if (!Connection->PriceOf(ProductId).bFoundInTheStore)
		{
			Check.NotFoundInTheStore.Add(Item);
		}
		if (AskAbout == nullptr)
		{
			AskAbout = &Item;
			AskedProduct = ProductId;
		}
	}
	if (AskAbout == nullptr)
	{
		OnComplete(TFlockResult<FFlockStoreSetupCheck>::Ok(Check));
		return;
	}
	// A purchase no store issued: the store answers it as unknown when Flock's keys work, and refuses the keys otherwise.
	const FFlockStoreReceipt MadeUp = PaymentProvider.Equals(FlockPaymentProviders::AppleAppStore, ESearchCase::CaseSensitive)
		? FFlockStoreReceipt::FromAppleAppStore(TEXT("0"))
		: FFlockStoreReceipt::FromGooglePlay(AskedProduct, TEXT("flock-setup-check"));
	Flock->GetShopProvider()->RedeemStoreReceipt(AskAbout->Id, MadeUp, [Check, OnComplete](TFlockResult<FFlockPurchaseResult> Answer) mutable
	{
		if (Answer.IsSuccess())
		{
			Check.bHasServerProblem = true;
			Check.ServerProblem = FFlockError::MakeWithFix(EFlockErrorType::Validation,
				TEXT("Flock's server accepted a purchase no store issued, so it does not check purchases with the store"),
				TEXT("Check the store's settings under Monetization > Payments in the Flock dashboard, and that this backend checks receipts with the store; report it to the backend team if they look right."));
		}
		else if (Answer.Error.ErrorCode == EFlockErrorCode::PaymentProviderReceiptInvalid)
		{
			Check.bServerCanCheckPurchases = true;
		}
		else
		{
			Check.bHasServerProblem = true;
			Check.ServerProblem = Answer.Error;
		}
		OnComplete(TFlockResult<FFlockStoreSetupCheck>::Ok(Check));
	});
}

// ── Helpers ──

bool UFlockStore::FindItem(const FString& ShopItemId, FFlockShopItem& OutItem) const
{
	if (bDisconnected || !bReady)
	{
		return false;
	}
	if (const FFlockShopItem* Item = ItemsById.Find(ShopItemId))
	{
		if (Item->Id.Equals(ShopItemId, ESearchCase::CaseSensitive))
		{
			OutItem = *Item;
			return true;
		}
	}
	return false;
}

bool UFlockStore::IsUsable(FFlockError& OutError) const
{
	if (bDisconnected || !Flock.IsValid() || !Flock->IsInitialized())
	{
		OutError = Refusal(EFlockErrorType::Validation, TEXT("This store connection is disconnected"),
			TEXT("Connect again with UFlockStore::Connect (a shutdown of Flock disconnects it), and use the connection it completes with."));
		return false;
	}
	if (!bReady)
	{
		OutError = Refusal(EFlockErrorType::Validation, TEXT("This store connection is still connecting"),
			TEXT("Wait for UFlockStore::Connect to complete before buying, pricing, restoring or checking the setup."));
		return false;
	}
	return true;
}

bool UFlockStore::KnownItem(const FString& ShopItemId, FFlockShopItem& OutItem, FFlockError& OutError) const
{
	if (ShopItemId.IsEmpty())
	{
		OutError = Refusal(EFlockErrorType::Validation, TEXT("Shop Item ID is empty"), TEXT("Pass the shop item's Id (FFlockShopItem::Id)."));
		return false;
	}
	if (!FindItem(ShopItemId, OutItem))
	{
		OutError = Refusal(EFlockErrorType::Validation, TEXT("That shop item was not in the game's shops when the store connected"),
			TEXT("Pass an FFlockShopItem::Id from this game's shops. An item added in the Flock dashboard since then is sold once this connection is disconnected and Connect runs again; an item or shop switched off (Inactive) there is not sold."));
		return false;
	}
	return true;
}

bool UFlockStore::ProductOf(const FString& ShopItemId, FFlockShopItem& OutItem, FString& OutProductId, FFlockError& OutError) const
{
	if (!KnownItem(ShopItemId, OutItem, OutError))
	{
		return false;
	}
	if (!OutItem.FindStoreProductId(PaymentProvider, OutProductId) || OutProductId.IsEmpty() || !IsSoldHere(OutItem))
	{
		OutError = Refusal(EFlockErrorType::Validation, FString::Printf(TEXT("This shop item has no %s product id"), *PaymentProvider),
			FString::Printf(TEXT("Enter the item's %s product id under Store product IDs on the shop item in the Flock dashboard, exactly as the store has it, then connect again."), *PaymentProvider));
		return false;
	}
	return true;
}

FFlockStoreReceipt UFlockStore::ReceiptOf(const FFlockStoreOrder& Order) const
{
	return PaymentProvider.Equals(FlockPaymentProviders::AppleAppStore, ESearchCase::CaseSensitive)
		? FFlockStoreReceipt::FromAppleAppStore(Order.TransactionId)
		: FFlockStoreReceipt::FromGooglePlay(Order.ProductId, Order.TransactionId);
}

FFlockError UFlockStore::SoldTwice(const FString& ProductId, const FString& Then) const
{
	return Refusal(EFlockErrorType::Validation,
		FString::Printf(TEXT("Two shop items sell the %s product '%s', so this purchase cannot be matched to one"), *PaymentProvider, *ProductId),
		TEXT("Give each shop item its own store product id in the Flock dashboard.") + Then);
}

int32 UFlockStore::CurrentSignIn() const
{
	const UFlockSubsystem* Sdk = Flock.Get();
	return Sdk != nullptr && Sdk->AuthSession.IsValid() ? Sdk->AuthSession->GetSignInNumber() : INDEX_NONE;
}

FString UFlockStore::CurrentPlayerId() const
{
	const UFlockSubsystem* Sdk = Flock.Get();
	return Sdk != nullptr && Sdk->AuthSession.IsValid() ? Sdk->AuthSession->GetPlayerId() : FString();
}

bool UFlockStore::IsSignedIn() const
{
	const UFlockSubsystem* Sdk = Flock.Get();
	return Sdk != nullptr && Sdk->AuthSession.IsValid() && Sdk->AuthSession->IsAuthenticated();
}
