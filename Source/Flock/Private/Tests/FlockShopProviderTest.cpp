// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockAnalyticsConfig.h"
#include "Analytics/FlockConsentStore.h"
#include "Analytics/FlockLifecyclePump.h"
#include "Analytics/FlockSession.h"
#include "Analytics/FlockTerminationTracker.h"
#include "Auth/FlockAuthSession.h"
#include "FlockEvents.h"
#include "FlockLogger.h"
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockSnapshotStore.h"
#include "Misc/Base64.h"
#include "Misc/Paths.h"
#include "Providers/FlockAnalyticsProvider.h"
#include "Blueprint/FlockShopLibrary.h"
#include "Providers/FlockPlayerProvider.h"
#include "Providers/FlockShopProvider.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockTestSafeIndex.h"
#include "Tests/Support/FlockMemoryEventCache.h"
#include "Tests/Support/FlockMemoryTokenStore.h"

namespace FlockShopProviderTestHelpers
{
	inline FFlockRetryPolicy NoRetry()
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = 0;
		return Policy;
	}

	inline FFlockRetryPolicy Retrying()
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = 3;
		Policy.InitialDelaySeconds = 0.f;
		Policy.bUseJitter = false;
		return Policy;
	}

	/** Minimal signed-in-looking token so the auth session reports a player id. */
	inline FString MakeTestJwt(const FString& PlayerId)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + 3600;
		FString Payload = FBase64::Encode(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp));
		Payload.ReplaceInline(TEXT("+"), TEXT("-"));
		Payload.ReplaceInline(TEXT("/"), TEXT("_"));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Payload);
	}

	/**
	 * Runs retries that the handler scheduled on the core ticker.
	 *
	 * A retry is never synchronous even at a zero delay — it goes through FTSTicker — so a test that only
	 * calls and asserts sees the first attempt and nothing else, and would read a working retry as an
	 * absent one.
	 */
	inline void PumpRetries()
	{
		for (int32 Index = 0; Index < 8; ++Index)
		{
			FTSTicker::GetCoreTicker().Tick(1.f);
		}
	}

	inline FString TempRoot()
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("shop_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	// ── Real wire shapes (bare/paginated/enveloped-list). An enveloped fixture on a route that is
	// actually bare still parses and still passes, so it hides the mismatch until a live backend
	// answers "missing result" — mirror what the server really sends. ──

	inline FString ItemBody(const FString& Id, int32 Price)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"name\":\"Sword\",\"status\":\"active\",\"type\":\"standard\",\"shop_id\":\"shop-1\",\"patch_id\":null,")
			TEXT("\"price\":%d,\"currency\":\"GOLD\",\"rewards\":[],\"data\":{\"rarity\":\"epic\"},\"created_at\":\"\",\"updated_at\":\"\"}"),
			*Id, Price);
	}


	/** A reward-bearing item: the shape a currency pack advertises in the catalog. */
	inline FString RewardItemBody(const FString& Id, int32 Price)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"name\":\"Gold Pack\",\"status\":\"active\",\"type\":\"currency_pack\",\"shop_id\":\"shop-1\",")
			TEXT("\"patch_id\":null,\"price\":%d,\"currency\":\"USD\",")
			TEXT("\"rewards\":[{\"type\":\"currency\",\"code\":\"GOLD\",\"amount\":500},")
			TEXT("{\"type\":\"currency\",\"code\":\"GEMS\",\"amount\":10}],")
			TEXT("\"data\":{},\"created_at\":\"\",\"updated_at\":\"\"}"),
			*Id, Price);
	}


	inline FString ShopBody(const FString& Id)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"name\":\"Starter\",\"status\":\"active\",\"game_id\":\"g\",\"game_version_id\":\"ver-1\",")
			TEXT("\"data\":{\"web_shop_url\":\"https://w\",\"pwa_shop_url\":\"https://p\",\"stats\":{\"visits\":5}},")
			TEXT("\"shop_items\":[%s],\"created_at\":\"\",\"updated_at\":\"\"}"),
			*Id, *ItemBody(TEXT("item-1"), 100));
	}


	/**
	 * An inventory row. `rewards` is carried because that member reaches the parse by a **different route**
	 * from the other reward arrays: it is nested two levels down inside a struct that is itself nested, and
	 * arrives through the reflection path rather than the hand-written ReadRewards. "One level deeper than
	 * the model" is the exact shape of the defect this change exists to fix, so it is asserted, not assumed.
	 */
	inline FString InventoryBody(const FString& Id, const FString& Status = TEXT("owned"))
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"player_id\":\"player-a\",\"shop_item_id\":\"item-1\",\"status\":\"%s\",")
			TEXT("\"rewards\":[{\"type\":\"currency\",\"code\":\"GOLD\",\"amount\":500}],")
			TEXT("\"created_at\":\"\",\"used_at\":null}"),
			*Id, *Status);
	}


	inline FString ShopPageBody()
	{
		return FString::Printf(TEXT("{\"items\":[%s],\"total\":1,\"page\":1,\"limit\":100}"), *ShopBody(TEXT("shop-1")));
	}


	inline FString InventoryPageBody()
	{
		return FString::Printf(TEXT("{\"items\":[%s],\"total\":1,\"page\":1,\"limit\":100}"), *InventoryBody(TEXT("inv-1")));
	}


	// ── The two reward routes. Both answer their model at the **root**, with no envelope: an enveloped
	// fixture here would parse, pass, and hide the mismatch until a live backend answered with nulls.
	// PurchaseResultSchema nests the inventory row under `inventory` — the field this SDK used to read
	// straight off the root, which is the defect these fixtures exist to keep fixed. ──

	/** A purchase that granted currency: inventory row, grant list and the wallet afterwards. */
	inline FString PurchaseResultBody(const FString& InventoryId = TEXT("inv-1"))
	{
		return FString::Printf(
			TEXT("{\"purchase_id\":\"pur-1\",\"item_type\":\"currency_pack\",\"inventory\":%s,")
			TEXT("\"granted\":[{\"type\":\"currency\",\"code\":\"GOLD\",\"amount\":500},")
			TEXT("{\"type\":\"currency\",\"code\":\"GEMS\",\"amount\":10}],")
			TEXT("\"wallet\":{\"id\":\"pd-1\",\"player_template_id\":\"tpl-w\",\"game_id\":\"g\",\"player_id\":\"player-a\",")
			TEXT("\"data\":{\"GOLD\":500},\"created_at\":\"\",\"updated_at\":\"\"}}"),
			*InventoryBody(InventoryId, TEXT("granted")));
	}


	/**
	 * A plain purchase: the server **omits** `granted` and `wallet` entirely rather than sending null or
	 * an empty array, which is the case the empty-not-null rule has to survive.
	 */
	/** A purchase result whose wallet reports an explicit Gold balance, for the write-through tests. */
	inline FString PurchaseResultBodyWithWalletGold(int32 Gold)
	{
		return FString::Printf(
			TEXT("{\"purchase_id\":\"pur-1\",\"item_type\":\"currency_pack\",\"inventory\":%s,")
			TEXT("\"granted\":[{\"type\":\"currency\",\"code\":\"GOLD\",\"amount\":500}],")
			TEXT("\"wallet\":{\"id\":\"pd-1\",\"player_template_id\":\"tmpl-1\",\"game_id\":\"g\",\"player_id\":\"player-a\",")
			TEXT("\"data\":[{\"type\":\"int\",\"field_name\":\"gold\",\"value\":%d}],\"created_at\":\"\",\"updated_at\":\"\"}}"),
			*InventoryBody(TEXT("inv-1"), TEXT("granted")), Gold);
	}

	inline FString PlainPurchaseResultBody()
	{
		return FString::Printf(
			TEXT("{\"purchase_id\":\"pur-2\",\"item_type\":\"cosmetic\",\"inventory\":%s}"),
			*InventoryBody(TEXT("inv-2")));
	}


	/** A currency pack holds nothing, so the server sends `inventory: null` — an explicit wire null. */
	inline FString NoInventoryPurchaseResultBody()
	{
		return TEXT("{\"purchase_id\":\"pur-3\",\"item_type\":\"currency_pack\",\"inventory\":null,")
			TEXT("\"granted\":[{\"type\":\"currency\",\"code\":\"GOLD\",\"amount\":500}],\"wallet\":null}");
	}


	/** ConsumeResultSchema: the updated row, what it granted, and the wallet. Root-shaped, no envelope. */
	inline FString ConsumeResultBody()
	{
		return FString::Printf(
			TEXT("{\"inventory\":%s,\"granted\":[{\"type\":\"currency\",\"code\":\"GOLD\",\"amount\":250}],")
			TEXT("\"wallet\":{\"id\":\"pd-1\",\"player_template_id\":\"tmpl-1\",\"game_id\":\"g\",\"player_id\":\"player-a\",")
			TEXT("\"data\":[{\"type\":\"int\",\"field_name\":\"gold\",\"value\":250}],\"created_at\":\"\",\"updated_at\":\"\"}}"),
			*InventoryBody(TEXT("inv-1"), TEXT("used")));
	}

	/**
	 * The same consume result with `type` **omitted** from the grant.
	 *
	 * Its own fixture rather than a tweak to the one above, so each serves a single intent: this one
	 * exists only to prove the "currency" member default survives the reflection path. The server may
	 * omit the key, and an empty string would make IsCurrency() false for the only kind that exists.
	 */
	inline FString ConsumeResultBodyNoRewardType()
	{
		return FString::Printf(
			TEXT("{\"inventory\":%s,\"granted\":[{\"code\":\"GOLD\",\"amount\":250}],")
			TEXT("\"wallet\":{\"id\":\"pd-1\",\"player_template_id\":\"tpl-w\",\"game_id\":\"g\",\"player_id\":\"player-a\",")
			TEXT("\"data\":{\"GOLD\":250},\"created_at\":\"\",\"updated_at\":\"\"}}"),
			*InventoryBody(TEXT("inv-1"), TEXT("used")));
	}


	inline FString ItemsEnvelopedBody()
	{
		return FString::Printf(TEXT("{\"error\":null,\"response\":null,\"result\":[%s]}"), *ItemBody(TEXT("item-1"), 100));
	}

	struct FFixture
	{
		FString Dir;
		FString ApiUrl;
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockAuthSession> Session;
		TSharedPtr<FFlockSnapshotStore> Snapshot;
		TSharedPtr<FFlockShopProvider> Provider;
		TSharedPtr<FFlockPlayerProvider> Players;

		// Analytics is wired only by the purchase-telemetry / transaction tests.
		UFlockEvents* Events = nullptr;
		TSharedPtr<FFlockMemoryEventCache> LogCache;
		TSharedPtr<FFlockMemoryEventCache> EndCache;
		TSharedPtr<FFlockAnalyticsProvider> Analytics;

		explicit FFixture(const FFlockRetryPolicy& Policy = NoRetry(), const FString& ExistingDir = FString(),
			const FString& InApiUrl = TEXT("http://x/v1"))
			: Dir(ExistingDir.IsEmpty() ? TempRoot() : ExistingDir)
			, ApiUrl(InApiUrl)
			, Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				InApiUrl, TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
		{
			Snapshot = MakeShared<FFlockSnapshotStore>(Dir, MakeShared<FFlockNullLogger>(), TEXT("9.9.9"));
			Provider = MakeShared<FFlockShopProvider>(Client, Policy, MakeShared<FFlockNullLogger>(),
				Session, InApiUrl, Snapshot, TEXT("ver-1"));
		}

		void SignIn(const FString& PlayerId = TEXT("player-a"))
		{
			FString Error;
			Session->SetTokens(MakeTestJwt(PlayerId), TEXT("r-1"), Error);
		}

		/** Builds a player provider and wires it in, so the wallet write-through has a cache to update. */
		void WirePlayers()
		{
			Players = MakeShared<FFlockPlayerProvider>(Client, NoRetry(), MakeShared<FFlockNullLogger>(),
				Session, ApiUrl, Snapshot, TEXT("ver-1"));
			Provider->SetPlayerProvider(Players);
		}

		/**
		 * Seeds the per-player cache. ApplyServerPlayerData deliberately ignores a player whose rows were
		 * never fetched, so without this the write-through would be a no-op and the tests would pass
		 * against a version that did nothing.
		 */
		bool PrimePlayerCache(int32 Gold)
		{
			Fake->On(TEXT("v1/player_data"), FFlockFakeTransport::Ok(FString::Printf(
				TEXT("{\"items\":[{\"id\":\"pd-1\",\"player_template_id\":\"tmpl-1\",\"game_id\":\"g\",")
				TEXT("\"player_id\":\"player-a\",\"data\":[{\"type\":\"int\",\"field_name\":\"gold\",\"value\":%d}],")
				TEXT("\"created_at\":\"\",\"updated_at\":\"\"}],\"total\":1,\"page\":1,\"limit\":100}"), Gold)));
			bool bDone = false;
			Players->GetMyDataByTemplate(TEXT("tmpl-1"), [&](TFlockResult<FFlockPlayerData> R) { bDone = R.bSuccess; });
			// Returned rather than check()'d: a broken fixture must fail the tests that use it, not abort
			// the process and take every other result down with it. Same reasoning as FlockTestAt.
			return bDone;
		}

		/** The Gold balance in the cached row, or -1 when there is no cached row to read. */
		int32 CachedGold() const
		{
			FFlockPlayerData Row;
			if (!Players.IsValid() || !Players->TryGetCachedRow(TEXT("pd-1"), Row))
			{
				return -1;
			}
			int32 Gold = 0;
			return Row.Data.TryGetInt(TEXT("Gold"), Gold) ? Gold : -1;
		}

		/** Builds a real analytics provider and wires it into the shop, so purchase telemetry has a target. */
		void WireAnalytics()
		{
			Events = NewObject<UFlockEvents>();
			const FFlockAnalyticsConfig Config;
			FFlockAnalyticsDependencies Deps;
			LogCache = MakeShared<FFlockMemoryEventCache>(0);
			EndCache = MakeShared<FFlockMemoryEventCache>(0);
			Deps.LogEventCache = LogCache;
			Deps.SessionEndCache = EndCache;
			Deps.Session = MakeShared<FFlockSession>(Config, FPaths::Combine(Dir, TEXT("session.json")));
			Deps.TerminationTracker = MakeShared<FFlockTerminationTracker>(false, FPaths::Combine(Dir, TEXT("marker.json")));
			Deps.ConsentStore = MakeShared<FFlockConsentStore>(FPaths::Combine(Dir, TEXT("consent.json")));
			Deps.Pump = MakeShared<FFlockLifecyclePump>();
			Deps.bEnableLogSink = false;
			Analytics = MakeShared<FFlockAnalyticsProvider>(Client, NoRetry(), MakeShared<FFlockNullLogger>(),
				Session, Events, ApiUrl, Config, Deps, TEXT("ver-1"), TEXT("0.10.0"));
			Provider->SetAnalyticsProvider(Analytics);
		}
	};

	inline void Cleanup(const FString& Dir)
	{
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
	}
}

using namespace FlockShopProviderTestHelpers;

// ── SHOP-01: bare shop-by-id parses, captures free-form data verbatim, then memoizes ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopGetByIdTest, "Flock.Shop.Provider.GetByIdBareAndCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopGetByIdTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("shop/shop-1"), FFlockFakeTransport::Ok(ShopBody(TEXT("shop-1"))));

	FFlockShop Shop;
	bool bDone = false;
	Fx.Provider->GetById(TEXT("shop-1"), [&](TFlockResult<FFlockShop> R) { bDone = R.bSuccess; Shop = R.Value; });
	TestTrue(TEXT("shop succeeds"), bDone);
	TestEqual(TEXT("id parsed"), Shop.Id, FString(TEXT("shop-1")));
	TestEqual(TEXT("game_version_id -> GameVersionId"), Shop.GameVersionId, FString(TEXT("ver-1")));
	TestEqual(TEXT("data.web_shop_url -> WebShopUrl"), Shop.Data.WebShopUrl, FString(TEXT("https://w")));
	int32 Visits = 0;
	TestTrue(TEXT("stats.visits read via handle (no manual parse)"), Shop.Data.Stats.TryGetInt(TEXT("visits"), Visits));
	TestEqual(TEXT("stats.visits value"), Visits, 5);
	if (TestEqual(TEXT("one nested item"), Shop.ShopItems.Num(), 1))
	{
		TestEqual(TEXT("nested item id"), Shop.ShopItems[0].Id, FString(TEXT("item-1")));
		TestEqual(TEXT("nested item price"), Shop.ShopItems[0].Price, 100);
		FString Rarity;
		TestTrue(TEXT("nested item data.rarity read via handle"), Shop.ShopItems[0].Data.TryGetString(TEXT("rarity"), Rarity));
		TestEqual(TEXT("nested item data.rarity value"), Rarity, FString(TEXT("epic")));
	}

	Fx.Provider->GetById(TEXT("shop-1"), [&](TFlockResult<FFlockShop> R) {});
	TestEqual(TEXT("cache hit, one request"), Fx.Fake->CountTo(TEXT("shop/shop-1")), 1);

	Cleanup(Fx.Dir);
	return true;
}

// ── SHOP-01 (by-name): the name rides the URL ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopGetByNameTest, "Flock.Shop.Provider.GetByNameUrl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopGetByNameTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("by-name"), FFlockFakeTransport::Ok(ShopBody(TEXT("shop-x"))));

	bool bDone = false;
	Fx.Provider->GetByName(TEXT("myshop"), [&](TFlockResult<FFlockShop> R) { bDone = R.bSuccess; });
	TestTrue(TEXT("by-name succeeds"), bDone);

	bool bSawName = false;
	for (const FFlockHttpRequest& R : Fx.Fake->Requests)
	{
		if (R.Url.Contains(TEXT("myshop"))) { bSawName = true; break; }
	}
	TestTrue(TEXT("by-name lookup carries the name in the URL"), bSawName);

	Cleanup(Fx.Dir);
	return true;
}

// ── Paginated GetAll parses the {items,total,page,limit} shape, memoizes, and round-trips a snapshot ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopGetAllTest, "Flock.Shop.Provider.GetAllPaginatedAndCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopGetAllTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("shop?page=1"), FFlockFakeTransport::Ok(ShopPageBody()));

	FFlockShopPage Page;
	bool bDone = false;
	Fx.Provider->GetAll(1, 100, [&](TFlockResult<FFlockShopPage> R) { bDone = R.bSuccess; Page = R.Value; });
	TestTrue(TEXT("shops succeed"), bDone);
	TestEqual(TEXT("total"), Page.Total, 1);
	TestEqual(TEXT("page"), Page.Page, 1);
	TestEqual(TEXT("limit"), Page.Limit, 100);
	if (TestEqual(TEXT("one shop"), Page.Items.Num(), 1))
	{
		TestEqual(TEXT("shop id"), Page.Items[0].Id, FString(TEXT("shop-1")));
	}

	Fx.Provider->GetAll(1, 100, [&](TFlockResult<FFlockShopPage> R) {});
	TestEqual(TEXT("cache hit, one request"), Fx.Fake->CountTo(TEXT("shop?page=1")), 1);

	Cleanup(Fx.Dir);
	return true;
}

// ── The paginated page survives the offline snapshot round-trip ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopGetAllSnapshotTest, "Flock.Shop.Provider.GetAllServedFromSnapshotOffline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopGetAllSnapshotTest::RunTest(const FString& Parameters)
{
	const FString Dir = TempRoot();
	{
		FFixture Warm(NoRetry(), Dir);
		Warm.Fake->On(TEXT("shop?page=1"), FFlockFakeTransport::Ok(ShopPageBody()));
		bool bDone = false;
		Warm.Provider->GetAll(1, 100, [&](TFlockResult<FFlockShopPage> R) { bDone = R.bSuccess; });
		TestTrue(TEXT("warm fetch succeeds"), bDone);
	}
	{
		FFixture Cold(NoRetry(), Dir);
		Cold.Provider->SetReachabilityProbe([]() { return false; });
		Cold.Fake->On(TEXT("shop?page=1"), FFlockFakeTransport::Offline());
		FFlockShopPage Page;
		bool bServed = false;
		Cold.Provider->GetAll(1, 100, [&](TFlockResult<FFlockShopPage> R) { bServed = R.bSuccess; Page = R.Value; });
		TestTrue(TEXT("served from snapshot offline"), bServed);
		TestEqual(TEXT("no network call"), Cold.Fake->CountTo(TEXT("shop?page=1")), 0);
		TestEqual(TEXT("snapshot preserved total"), Page.Total, 1);
		if (TestEqual(TEXT("snapshot preserved items"), Page.Items.Num(), 1))
		{
			TestEqual(TEXT("snapshot preserved shop id"), Page.Items[0].Id, FString(TEXT("shop-1")));
			// The opaque-data handle must survive the snapshot round-trip (it stores JSON in a reflected field).
			if (Page.Items[0].ShopItems.Num() > 0)
			{
				FString Rarity;
				TestTrue(TEXT("free-form data survived the snapshot round-trip"),
					Page.Items[0].ShopItems[0].Data.TryGetString(TEXT("rarity"), Rarity));
				TestEqual(TEXT("snapshot data.rarity value"), Rarity, FString(TEXT("epic")));
			}
		}
	}
	Cleanup(Dir);
	return true;
}

// ── Items-by-shop reads the enveloped-list shape and honours patch_id ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopItemsByShopTest, "Flock.Shop.Provider.ItemsByShopEnvelopedList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopItemsByShopTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("shop_item/shop/shop-1"), FFlockFakeTransport::Ok(ItemsEnvelopedBody()));

	TArray<FFlockShopItem> Items;
	bool bDone = false;
	Fx.Provider->GetItemsByShop(TEXT("shop-1"), FString(), [&](TFlockResult<TArray<FFlockShopItem>> R) { bDone = R.bSuccess; Items = R.Value; });
	TestTrue(TEXT("items succeed"), bDone);
	if (TestEqual(TEXT("one item"), Items.Num(), 1))
	{
		TestEqual(TEXT("item id"), Items[0].Id, FString(TEXT("item-1")));
		FString Rarity;
		TestTrue(TEXT("item data.rarity read via handle"), Items[0].Data.TryGetString(TEXT("rarity"), Rarity));
		TestEqual(TEXT("item data.rarity value"), Rarity, FString(TEXT("epic")));
	}

	// A patch id lands in the query and keys a distinct cache entry (a second request).
	Fx.Provider->GetItemsByShop(TEXT("shop-1"), TEXT("patch-9"), [&](TFlockResult<TArray<FFlockShopItem>> R) {});
	bool bSawPatch = false;
	for (const FFlockHttpRequest& R : Fx.Fake->Requests)
	{
		if (R.Url.Contains(TEXT("patch_id=patch-9"))) { bSawPatch = true; break; }
	}
	TestTrue(TEXT("patch_id carried in the URL"), bSawPatch);

	Cleanup(Fx.Dir);
	return true;
}

// ── Bare shop-item-by-id parses and memoizes ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopGetItemTest, "Flock.Shop.Provider.GetItemBareAndCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopGetItemTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));

	FFlockShopItem Item;
	bool bDone = false;
	Fx.Provider->GetItem(TEXT("item-1"), [&](TFlockResult<FFlockShopItem> R) { bDone = R.bSuccess; Item = R.Value; });
	TestTrue(TEXT("item succeeds"), bDone);
	TestEqual(TEXT("id parsed"), Item.Id, FString(TEXT("item-1")));
	TestEqual(TEXT("price parsed"), Item.Price, 100);
	TestEqual(TEXT("currency parsed"), Item.Currency, FString(TEXT("GOLD")));

	Fx.Provider->GetItem(TEXT("item-1"), [&](TFlockResult<FFlockShopItem> R) {});
	TestEqual(TEXT("cache hit, one request"), Fx.Fake->CountTo(TEXT("shop_item/item-1")), 1);

	Cleanup(Fx.Dir);
	return true;
}

// ── Validation short-circuits before any request ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopValidationTest, "Flock.Shop.Provider.ValidationShortCircuits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopValidationTest::RunTest(const FString& Parameters)
{
	{
		FFixture Fx;
		bool bFailed = false;
		Fx.Provider->GetById(FString(), [&](TFlockResult<FFlockShop> R) { bFailed = !R.bSuccess && R.Error.Type == EFlockErrorType::Validation; });
		TestTrue(TEXT("empty shop id -> validation"), bFailed);
		TestEqual(TEXT("no request"), Fx.Fake->Requests.Num(), 0);
		Cleanup(Fx.Dir);
	}
	{
		// Signed in, but empty item id -> validation before any network.
		FFixture Fx;
		Fx.SignIn();
		bool bFailed = false;
		Fx.Provider->Purchase(FString(), FString(), [&](TFlockResult<FFlockPurchaseResult> R) { bFailed = !R.bSuccess && R.Error.Type == EFlockErrorType::Validation; });
		TestTrue(TEXT("empty item id -> validation"), bFailed);
		TestEqual(TEXT("no request"), Fx.Fake->Requests.Num(), 0);
		Cleanup(Fx.Dir);
	}
	{
		// Item id present, but signed out -> validation (player-scoped route).
		FFixture Fx;
		bool bFailed = false;
		Fx.Provider->Purchase(TEXT("item-1"), FString(), [&](TFlockResult<FFlockPurchaseResult> R) { bFailed = !R.bSuccess && R.Error.Type == EFlockErrorType::Validation; });
		TestTrue(TEXT("purchase signed out -> validation"), bFailed);
		TestEqual(TEXT("no request"), Fx.Fake->Requests.Num(), 0);
		Cleanup(Fx.Dir);
	}
	{
		FFixture Fx;
		bool bFailed = false;
		Fx.Provider->GetPlayerInventory(FString(), 1, 100, [&](TFlockResult<FFlockPlayerInventoryPage> R) { bFailed = !R.bSuccess && R.Error.Type == EFlockErrorType::Validation; });
		TestTrue(TEXT("inventory signed out -> validation"), bFailed);
		TestEqual(TEXT("no request"), Fx.Fake->Requests.Num(), 0);
		Cleanup(Fx.Dir);
	}
	return true;
}

// ── SHOP-03: an ambiguous purchase failure (5xx) is money-safe — NOT retried ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopPurchaseNotRetriedTest, "Flock.Shop.Provider.PurchaseNotRetriedOnServerError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopPurchaseNotRetriedTest::RunTest(const FString& Parameters)
{
	// A retrying policy would re-send an idempotent 5xx — proving the purchase does not is the point.
	FFixture Fx(Retrying());
	Fx.SignIn();
	Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));
	Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Status(500, TEXT("{}")));

	bool bFailed = false;
	Fx.Provider->Purchase(TEXT("item-1"), FString(), [&](TFlockResult<FFlockPurchaseResult> R) { bFailed = !R.bSuccess; });
	TestTrue(TEXT("ambiguous purchase failure surfaces"), bFailed);
	TestEqual(TEXT("money mutation not retried on a 5xx"), Fx.Fake->CountTo(TEXT("shop/transaction")), 1);

	Cleanup(Fx.Dir);
	return true;
}

// ── SHOP-06: inventory is never cached — offline it fails rather than serving stale ownership ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopInventoryOfflineTest, "Flock.Shop.Provider.InventoryOfflineFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopInventoryOfflineTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	// Warm once (a snapshot-backed read would cache here); then go offline.
	Fx.Fake->On(TEXT("player_inventory/player/player-a"), FFlockFakeTransport::Ok(InventoryPageBody()));
	bool bWarm = false;
	Fx.Provider->GetPlayerInventory(FString(), 1, 100, [&](TFlockResult<FFlockPlayerInventoryPage> R) { bWarm = R.bSuccess; });
	TestTrue(TEXT("warm inventory succeeds"), bWarm);

	Fx.Fake->On(TEXT("player_inventory/player/player-a"), FFlockFakeTransport::Offline());
	bool bFailed = false;
	Fx.Provider->GetPlayerInventory(FString(), 1, 100, [&](TFlockResult<FFlockPlayerInventoryPage> R) { bFailed = !R.bSuccess; });
	TestTrue(TEXT("offline inventory fails (never cached)"), bFailed);

	Cleanup(Fx.Dir);
	return true;
}

// ── RecordTransaction requires a signed-in player and posts to analytics/transactions ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopRecordTransactionTest, "Flock.Shop.Analytics.RecordTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopRecordTransactionTest::RunTest(const FString& Parameters)
{
	{
		// Signed out: rejected before any network.
		FFixture Fx;
		Fx.WireAnalytics();
		Fx.Fake->On(TEXT("analytics/transactions"), FFlockFakeTransport::Ok(TEXT("{}")));
		FFlockAnalyticsTransactionRequest Req;
		Req.Amount = 5.0;
		bool bFailed = false;
		Fx.Analytics->RecordTransaction(Req, [&](TFlockResult<FFlockAnalyticsAck> R) { bFailed = !R.bSuccess && R.Error.Type == EFlockErrorType::Auth; });
		TestTrue(TEXT("signed out -> auth failure"), bFailed);
		TestEqual(TEXT("no request"), Fx.Fake->CountTo(TEXT("analytics/transactions")), 0);
		Cleanup(Fx.Dir);
	}
	{
		// Signed in: posts, filling player_id from the session.
		FFixture Fx;
		Fx.SignIn();
		Fx.WireAnalytics();
		Fx.Fake->On(TEXT("analytics/transactions"), FFlockFakeTransport::Ok(TEXT("{}")));
		FFlockAnalyticsTransactionRequest Req;
		Req.Amount = 5.0;
		bool bDone = false;
		Fx.Analytics->RecordTransaction(Req, [&](TFlockResult<FFlockAnalyticsAck> R) { bDone = R.bSuccess; });
		TestTrue(TEXT("signed in -> succeeds"), bDone);
		TestEqual(TEXT("one request"), Fx.Fake->CountTo(TEXT("analytics/transactions")), 1);

		bool bBodyHasPlayer = false;
		for (const FFlockHttpRequest& R : Fx.Fake->Requests)
		{
			if (R.Url.Contains(TEXT("analytics/transactions")) && R.JsonBody.Contains(TEXT("player-a")))
			{
				bBodyHasPlayer = true;
				break;
			}
		}
		TestTrue(TEXT("player id filled from the session"), bBodyHasPlayer);
		Cleanup(Fx.Dir);
	}
	return true;
}

// ── A successful purchase records Started + Purchased, best-effort, without breaking the purchase ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopPurchaseTelemetryTest, "Flock.Shop.Provider.PurchaseRecordsTelemetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopPurchaseTelemetryTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.WireAnalytics();
	Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));
	Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Ok(PurchaseResultBody()));
	Fx.Fake->On(TEXT("analytics/transactions"), FFlockFakeTransport::Ok(TEXT("{}")));

	FFlockPurchaseResult Purchased;
	bool bDone = false;
	Fx.Provider->Purchase(TEXT("item-1"), FString(), [&](TFlockResult<FFlockPurchaseResult> R) { bDone = R.bSuccess; Purchased = R.Value; });
	TestTrue(TEXT("purchase succeeds"), bDone);
	TestEqual(TEXT("inventory id returned, nested under .Inventory"), Purchased.Inventory.Id, FString(TEXT("inv-1")));
	TestEqual(TEXT("one purchase POST"), Fx.Fake->CountTo(TEXT("shop/transaction")), 1);
	TestEqual(TEXT("Started + Purchased recorded"), Fx.Fake->CountTo(TEXT("analytics/transactions")), 2);

	Cleanup(Fx.Dir);
	return true;
}

// ── ClearCache forces the next catalog read back to the network ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopClearCacheTest, "Flock.Shop.Provider.ClearCacheRefetches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopClearCacheTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("shop/shop-1"), FFlockFakeTransport::Ok(ShopBody(TEXT("shop-1"))));

	Fx.Provider->GetById(TEXT("shop-1"), [&](TFlockResult<FFlockShop> R) {});
	Fx.Provider->ClearCache();
	Fx.Provider->GetById(TEXT("shop-1"), [&](TFlockResult<FFlockShop> R) {});
	TestEqual(TEXT("cache cleared -> two requests"), Fx.Fake->CountTo(TEXT("shop/shop-1")), 2);

	Cleanup(Fx.Dir);
	return true;
}

// ── SHOP-08: the purchase route is PurchaseResultSchema at the root, and the inventory row is NESTED ──
// This is the defect the whole change exists to fix: the SDK used to deserialize the response straight
// onto the inventory model, which against a real backend produced a populated-looking row of nulls. The
// fixture is root-shaped and nests `inventory`, so reading it off the root cannot pass.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopPurchaseResultShapeTest, "Flock.Shop.Provider.PurchaseParsesNestedResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopPurchaseResultShapeTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(RewardItemBody(TEXT("item-1"), 100)));
	Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Ok(PurchaseResultBody()));

	FFlockPurchaseResult Purchased;
	bool bDone = false;
	Fx.Provider->Purchase(TEXT("item-1"), FString(), [&](TFlockResult<FFlockPurchaseResult> R) { bDone = R.bSuccess; Purchased = R.Value; });

	TestTrue(TEXT("purchase succeeds"), bDone);
	TestEqual(TEXT("purchase id read from the root"), Purchased.PurchaseId, FString(TEXT("pur-1")));
	TestEqual(TEXT("item type read from the root"), Purchased.ItemType, FString(TEXT("currency_pack")));
	// The row is under .Inventory now, not at the top level.
	TestEqual(TEXT("the inventory row is nested, not at the root"), Purchased.Inventory.Id, FString(TEXT("inv-1")));
	// "granted" is the status the backend stamps on a row created by a reward-bearing purchase.
	TestEqual(TEXT("and its own fields survived the nesting"), Purchased.Inventory.Status, FString(TEXT("granted")));
	// The row's own reward snapshot: an array of objects two levels down, taken by the reflection path
	// rather than ReadRewards. Nothing else in the suite exercises that route.
	if (TestEqual(TEXT("the row's reward snapshot parsed"), Purchased.Inventory.Rewards.Num(), 1))
	{
		TestEqual(TEXT("its code"), FlockTestAt(Purchased.Inventory.Rewards, 0).Code, FString(TEXT("GOLD")));
		TestEqual(TEXT("its amount"), FlockTestAt(Purchased.Inventory.Rewards, 0).Amount, 500);
		TestTrue(TEXT("and its type survived the nested transform"),
			FlockTestAt(Purchased.Inventory.Rewards, 0).IsCurrency());
	}

	// Multiple rewards, in order, with their codes and amounts intact.
	if (TestEqual(TEXT("both grants parsed"), Purchased.Granted.Num(), 2))
	{
		TestEqual(TEXT("first grant code"), FlockTestAt(Purchased.Granted, 0).Code, FString(TEXT("GOLD")));
		TestEqual(TEXT("first grant amount"), FlockTestAt(Purchased.Granted, 0).Amount, 500);
		TestTrue(TEXT("first grant is a currency reward"), FlockTestAt(Purchased.Granted, 0).IsCurrency());
		TestEqual(TEXT("second grant code"), FlockTestAt(Purchased.Granted, 1).Code, FString(TEXT("GEMS")));
		TestEqual(TEXT("second grant amount"), FlockTestAt(Purchased.Granted, 1).Amount, 10);
	}

	// The wallet is an FFlockPlayerData, whose own parse flattens `data` — the reflection path would not
	// reach it, which is why the result declares a custom FromWireObject.
	TestEqual(TEXT("wallet row id"), Purchased.Wallet.Id, FString(TEXT("pd-1")));
	int32 Gold = 0;
	TestTrue(TEXT("wallet data was flattened, not dropped"), Purchased.Wallet.Data.TryGetInt(TEXT("GOLD"), Gold));
	TestEqual(TEXT("wallet balance after the grant"), Gold, 500);

	Cleanup(Fx.Dir);
	return true;
}

// ── SHOP-09: an omitted `granted` is an empty list, never a failure and never a null to check ──
// The server drops the key entirely for an item that grants nothing, so treating absence as malformed
// would fail the ordinary purchase. Callers must be able to iterate without a guard.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopPurchaseOmittedGrantTest, "Flock.Shop.Provider.PurchaseOmittedGrantIsEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopPurchaseOmittedGrantTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));
	Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Ok(PlainPurchaseResultBody()));

	FFlockPurchaseResult Purchased;
	bool bDone = false;
	Fx.Provider->Purchase(TEXT("item-1"), FString(), [&](TFlockResult<FFlockPurchaseResult> R) { bDone = R.bSuccess; Purchased = R.Value; });

	TestTrue(TEXT("a purchase with no grant still succeeds"), bDone);
	TestEqual(TEXT("granted defaults to empty"), Purchased.Granted.Num(), 0);
	TestTrue(TEXT("an absent wallet leaves an empty row, not a parse failure"), Purchased.Wallet.Id.IsEmpty());
	TestEqual(TEXT("the inventory row is still there"), Purchased.Inventory.Id, FString(TEXT("inv-2")));

	Cleanup(Fx.Dir);
	return true;
}

// ── SHOP-10: `inventory: null` is a real answer, not a broken one ──
// A currency pack hands its contents over immediately and so creates nothing to own. An explicit wire
// null must leave an empty row a caller can test, rather than failing the purchase that did go through.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopPurchaseNullInventoryTest, "Flock.Shop.Provider.PurchaseWithoutInventoryRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopPurchaseNullInventoryTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(RewardItemBody(TEXT("item-1"), 100)));
	Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Ok(NoInventoryPurchaseResultBody()));

	FFlockPurchaseResult Purchased;
	bool bDone = false;
	Fx.Provider->Purchase(TEXT("item-1"), FString(), [&](TFlockResult<FFlockPurchaseResult> R) { bDone = R.bSuccess; Purchased = R.Value; });

	TestTrue(TEXT("a null inventory does not fail the purchase"), bDone);
	TestTrue(TEXT("the row is empty, which is how a caller detects it"), Purchased.Inventory.Id.IsEmpty());
	TestEqual(TEXT("the grant is what the purchase actually produced"), Purchased.Granted.Num(), 1);

	Cleanup(Fx.Dir);
	return true;
}

// ── SHOP-11: a catalog item advertises its type and its rewards before anyone buys ──
// This is what lets a shop UI show "500 gold" on the tile rather than only after the transaction.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopItemAdvertisesRewardsTest, "Flock.Shop.Provider.ItemAdvertisesRewards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopItemAdvertisesRewardsTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("shop_item/item-9"), FFlockFakeTransport::Ok(RewardItemBody(TEXT("item-9"), 499)));

	FFlockShopItem Item;
	bool bDone = false;
	Fx.Provider->GetItem(TEXT("item-9"), [&](TFlockResult<FFlockShopItem> R) { bDone = R.bSuccess; Item = R.Value; });

	TestTrue(TEXT("fetched"), bDone);
	TestEqual(TEXT("item type"), Item.Type, FString(TEXT("currency_pack")));
	if (TestEqual(TEXT("both advertised rewards parsed"), Item.Rewards.Num(), 2))
	{
		TestEqual(TEXT("first reward code"), FlockTestAt(Item.Rewards, 0).Code, FString(TEXT("GOLD")));
		TestEqual(TEXT("first reward amount"), FlockTestAt(Item.Rewards, 0).Amount, 500);
	}

	// A plain item carries an empty list rather than a null anyone has to check.
	FFlockShopItem Plain;
	Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));
	Fx.Provider->GetItem(TEXT("item-1"), [&](TFlockResult<FFlockShopItem> R) { Plain = R.Value; });
	TestEqual(TEXT("a plain item advertises no rewards"), Plain.Rewards.Num(), 0);

	Cleanup(Fx.Dir);
	return true;
}

// ── SHOP-12: a reward that omits `type` reads as a currency grant ──
// The wire default is "currency" and the server may leave the key out. Defaulting to an empty string
// would make IsCurrency() false for the only reward kind that exists, silently skipping every grant a
// graph branches on. Pinned separately because it depends on the struct's member initializer surviving
// the reflection path, which is not obvious from reading either half.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopRewardTypeDefaultTest, "Flock.Shop.Provider.RewardTypeDefaultsToCurrency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopRewardTypeDefaultTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("player_inventory/inv-1/consume"), FFlockFakeTransport::Ok(ConsumeResultBodyNoRewardType()));

	FFlockConsumeResult Consumed;
	bool bDone = false;
	Fx.Provider->Consume(TEXT("inv-1"), [&](TFlockResult<FFlockConsumeResult> R) { bDone = R.bSuccess; Consumed = R.Value; });

	TestTrue(TEXT("consume succeeds"), bDone);
	// This fixture's grant deliberately omits `type`.
	if (TestEqual(TEXT("the grant parsed"), Consumed.Granted.Num(), 1))
	{
		TestEqual(TEXT("an omitted type falls back to the wire default"),
			FlockTestAt(Consumed.Granted, 0).Type, FString(FlockShopItemRewardTypes::Currency));
		TestTrue(TEXT("so it still reads as a currency reward"), FlockTestAt(Consumed.Granted, 0).IsCurrency());
	}

	Cleanup(Fx.Dir);
	return true;
}

// ── SHOP-13: consume routes to the right path and parses its root-shaped result ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopConsumeTest, "Flock.Shop.Provider.ConsumeParsesResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopConsumeTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("player_inventory/inv-1/consume"), FFlockFakeTransport::Ok(ConsumeResultBody()));

	FFlockConsumeResult Consumed;
	bool bDone = false;
	Fx.Provider->Consume(TEXT("inv-1"), [&](TFlockResult<FFlockConsumeResult> R) { bDone = R.bSuccess; Consumed = R.Value; });

	TestTrue(TEXT("consume succeeds"), bDone);
	TestEqual(TEXT("one POST to the consume route"), Fx.Fake->CountTo(TEXT("player_inventory/inv-1/consume")), 1);
	TestEqual(TEXT("the updated row came back"), Consumed.Inventory.Id, FString(TEXT("inv-1")));
	TestEqual(TEXT("what it granted"), Consumed.Granted.Num(), 1);
	TestEqual(TEXT("granted amount"), FlockTestAt(Consumed.Granted, 0).Amount, 250);
	TestEqual(TEXT("and the wallet afterwards"), Consumed.Wallet.Id, FString(TEXT("pd-1")));

	// An empty id never reaches the network — it would build a path with a hole in it.
	bool bFailed = false;
	Fx.Provider->Consume(FString(), [&](TFlockResult<FFlockConsumeResult> R)
	{
		bFailed = !R.bSuccess && R.Error.Type == EFlockErrorType::Validation;
	});
	TestTrue(TEXT("an empty inventory id is a validation failure"), bFailed);

	Cleanup(Fx.Dir);
	return true;
}

// ── SHOP-14: consume is a money mutation, so an ambiguous failure surfaces instead of re-sending ──
// Consuming credits currency. A retry on a 500 would grant twice, and nothing on the client could undo
// it. The 429 half of the test is what proves the single attempt is *policy* rather than a retry config
// that happens to be dead — without it, a provider that never retried anything would pass just as well.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopConsumeMoneySafetyTest, "Flock.Shop.Provider.ConsumeNotRetriedOnAmbiguousFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopConsumeMoneySafetyTest::RunTest(const FString& Parameters)
{
	{
		// A 500 is ambiguous: the grant may already have landed. One attempt, then surface it.
		FFixture Fx(Retrying());
		Fx.Fake->On(TEXT("player_inventory/inv-1/consume"), FFlockFakeTransport::Status(500, TEXT("{}")));

		bool bFailed = false;
		Fx.Provider->Consume(TEXT("inv-1"), [&](TFlockResult<FFlockConsumeResult> R) { bFailed = !R.bSuccess; });
		// Pumped even though nothing should be scheduled: without this, "one attempt" would also pass
		// against a version that *had* queued a retry and simply never got to run it.
		PumpRetries();

		TestTrue(TEXT("the failure surfaces"), bFailed);
		TestEqual(TEXT("a money mutation is not retried on a 5xx"),
			Fx.Fake->CountTo(TEXT("player_inventory/inv-1/consume")), 1);
		Cleanup(Fx.Dir);
	}
	{
		// A 429 proves the server never processed it, so retrying cannot double-grant.
		FFixture Fx(Retrying());
		Fx.Fake->OnSequence(TEXT("player_inventory/inv-1/consume"),
			{ FFlockFakeTransport::Status(429, TEXT("{}")), FFlockFakeTransport::Ok(ConsumeResultBody()) });

		bool bOk = false;
		Fx.Provider->Consume(TEXT("inv-1"), [&](TFlockResult<FFlockConsumeResult> R) { bOk = R.bSuccess; });
		PumpRetries();

		TestTrue(TEXT("a provably-unprocessed failure is retried and succeeds"), bOk);
		TestEqual(TEXT("which took two attempts"),
			Fx.Fake->CountTo(TEXT("player_inventory/inv-1/consume")), 2);
		Cleanup(Fx.Dir);
	}
	return true;
}

// ── SHOP-15: the Blueprint library answers exactly what the structs answer ──
// A graph must not be able to reach a different conclusion from C++ about whether a reward is currency
// or whether a purchase produced a row — which is the whole reason these are nodes and not literals.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopLibraryParityTest, "Flock.Shop.Library.CppParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopLibraryParityTest::RunTest(const FString& Parameters)
{
	FFlockShopItemReward Currency;
	Currency.Code = TEXT("GOLD");
	Currency.Amount = 5;
	TestEqual(TEXT("Is Currency Reward matches the struct"),
		UFlockShopLibrary::IsCurrencyReward(Currency), Currency.IsCurrency());
	TestTrue(TEXT("and answers true for the default type"), UFlockShopLibrary::IsCurrencyReward(Currency));

	// A kind the server adds later stays readable and simply is not currency — no throw, no default.
	FFlockShopItemReward Future;
	Future.Type = TEXT("achievement");
	TestFalse(TEXT("an unknown kind is not currency"), UFlockShopLibrary::IsCurrencyReward(Future));
	TestEqual(TEXT("and its raw type is preserved"), Future.Type, FString(TEXT("achievement")));

	TestEqual(TEXT("the exposed constant is the wire spelling"),
		UFlockShopLibrary::CurrencyRewardType(), FString(FlockShopItemRewardTypes::Currency));

	FFlockPurchaseResult WithRow;
	WithRow.Inventory.Id = TEXT("inv-1");
	WithRow.Wallet.Id = TEXT("pd-1");
	TestTrue(TEXT("Has Inventory Row"), UFlockShopLibrary::HasInventoryRow(WithRow));
	TestTrue(TEXT("Has Wallet"), UFlockShopLibrary::PurchaseHasWallet(WithRow));

	const FFlockPurchaseResult Bare;
	TestFalse(TEXT("no row on a grant-only purchase"), UFlockShopLibrary::HasInventoryRow(Bare));
	TestFalse(TEXT("no wallet when no currency moved"), UFlockShopLibrary::PurchaseHasWallet(Bare));

	FFlockConsumeResult Consumed;
	TestFalse(TEXT("consume wallet absent"), UFlockShopLibrary::ConsumeHasWallet(Consumed));
	Consumed.Wallet.Id = TEXT("pd-1");
	TestTrue(TEXT("consume wallet present"), UFlockShopLibrary::ConsumeHasWallet(Consumed));

	return true;
}

// ── SHOP-16: a purchase folds the wallet it returns into the cached player row ──
// Found live on 2026-08-30: without this the per-player cache keeps pre-purchase balances, so the next
// read serves the old number — and a read-modify-write writes it straight back over the purchase. The
// self-test's own commands sweep did exactly that and silently reset a balance a purchase had changed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopPurchaseWritesWalletThroughTest, "Flock.Shop.Provider.PurchaseWritesWalletToPlayerCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopPurchaseWritesWalletThroughTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.WirePlayers();
	TestTrue(TEXT("player cache primed"), Fx.PrimePlayerCache(/*Gold*/ 100));
	TestEqual(TEXT("cache primed with the pre-purchase balance"), Fx.CachedGold(), 100);

	Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(RewardItemBody(TEXT("item-1"), 100)));
	// The wallet the server reports after the debit and the grant.
	Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Ok(PurchaseResultBodyWithWalletGold(500)));

	bool bDone = false;
	Fx.Provider->Purchase(TEXT("item-1"), FString(), [&](TFlockResult<FFlockPurchaseResult> R) { bDone = R.bSuccess; });

	TestTrue(TEXT("purchase succeeds"), bDone);
	TestEqual(TEXT("the cached row now holds the post-purchase balance"), Fx.CachedGold(), 500);

	Cleanup(Fx.Dir);
	return true;
}

// ── SHOP-17: consuming writes through too — it credits currency for the same reason ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopConsumeWritesWalletThroughTest, "Flock.Shop.Provider.ConsumeWritesWalletToPlayerCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopConsumeWritesWalletThroughTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.WirePlayers();
	TestTrue(TEXT("player cache primed"), Fx.PrimePlayerCache(/*Gold*/ 100));

	Fx.Fake->On(TEXT("player_inventory/inv-1/consume"), FFlockFakeTransport::Ok(ConsumeResultBody()));

	bool bDone = false;
	Fx.Provider->Consume(TEXT("inv-1"), [&](TFlockResult<FFlockConsumeResult> R) { bDone = R.bSuccess; });

	TestTrue(TEXT("consume succeeds"), bDone);
	// ConsumeResultBody's wallet carries GOLD 250.
	TestEqual(TEXT("the grant reached the cached row"), Fx.CachedGold(), 250);

	Cleanup(Fx.Dir);
	return true;
}

// ── SHOP-18: no wallet means nothing to write, and the cache is left exactly as it was ──
// The server omits the wallet when the purchase moved no currency. Writing a default-constructed row
// through would replace a good cached row with an empty one — worse than not writing at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopNoWalletLeavesCacheTest, "Flock.Shop.Provider.PurchaseWithoutWalletLeavesCacheAlone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopNoWalletLeavesCacheTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.WirePlayers();
	TestTrue(TEXT("player cache primed"), Fx.PrimePlayerCache(/*Gold*/ 100));

	Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));
	// PlainPurchaseResultBody omits `wallet` entirely, the way the server does for a plain item.
	Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Ok(PlainPurchaseResultBody()));

	bool bDone = false;
	Fx.Provider->Purchase(TEXT("item-1"), FString(), [&](TFlockResult<FFlockPurchaseResult> R) { bDone = R.bSuccess; });

	TestTrue(TEXT("purchase succeeds"), bDone);
	TestEqual(TEXT("the cached row is untouched, not blanked"), Fx.CachedGold(), 100);

	Cleanup(Fx.Dir);
	return true;
}

// ── SHOP-19: with no player provider wired, a purchase still succeeds ──
// The write-through is a convenience, not a dependency: a provider built outside the subsystem must not
// fail a purchase just because there is no cache to refresh.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopNoPlayerProviderTest, "Flock.Shop.Provider.PurchaseSucceedsWithoutPlayerProvider",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopNoPlayerProviderTest::RunTest(const FString& Parameters)
{
	FFixture Fx;                       // deliberately no WirePlayers()
	Fx.SignIn();
	Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(RewardItemBody(TEXT("item-1"), 100)));
	Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Ok(PurchaseResultBodyWithWalletGold(500)));

	bool bDone = false;
	FFlockPurchaseResult Purchased;
	Fx.Provider->Purchase(TEXT("item-1"), FString(), [&](TFlockResult<FFlockPurchaseResult> R) { bDone = R.bSuccess; Purchased = R.Value; });

	TestTrue(TEXT("the purchase still succeeds"), bDone);
	TestEqual(TEXT("and still reports its wallet to the caller"), Purchased.Wallet.Id, FString(TEXT("pd-1")));

	Cleanup(Fx.Dir);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
