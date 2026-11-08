// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"
#include "Misc/FlockEngineCompat.h"

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
#include "Http/FlockErrorHints.h"
#include "Http/FlockSnapshotStore.h"
#include "Misc/Base64.h"
#include "Misc/Paths.h"
#include "Providers/FlockAnalyticsProvider.h"
#include "Blueprint/FlockShopLibrary.h"
#include "Providers/FlockPlayerProvider.h"
#include "Providers/FlockShopProvider.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockTestRetries.h"
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

// ── SHOP-03: an ambiguous consume failure (5xx) is money-safe — NOT retried; a purchase is, under its one key ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopPurchaseNotRetriedTest, "Flock.Shop.Provider.ConsumeNotRetriedOnServerErrorAPurchaseIsUnderItsKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopPurchaseNotRetriedTest::RunTest(const FString& Parameters)
{
	// A retrying policy re-sends an idempotent 5xx: the consume must not be; the purchase is, its key making it one purchase.
	FFixture Fx(Retrying());
	Fx.SignIn();
	Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));
	Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Status(500, TEXT("{}")));
	Fx.Fake->On(TEXT("player_inventory/inv-1/consume"), FFlockFakeTransport::Status(500, TEXT("{}")));

	bool bFailed = false;
	Fx.Provider->Purchase(TEXT("item-1"), FString(), [&](TFlockResult<FFlockPurchaseResult> R) { bFailed = !R.bSuccess; });
	bool bConsumeFailed = false;
	Fx.Provider->Consume(TEXT("inv-1"), [&](TFlockResult<FFlockConsumeResult> R) { bConsumeFailed = !R.bSuccess; });
	FlockTestRetries::Pump();
	TestTrue(TEXT("ambiguous purchase failure surfaces"), bFailed);
	TestEqual(TEXT("the purchase is sent on every attempt, under one key"), Fx.Fake->CountTo(TEXT("shop/transaction")), 4);
	TestTrue(TEXT("ambiguous consume failure surfaces"), bConsumeFailed);
	TestEqual(TEXT("the consume is not retried on a 5xx"), Fx.Fake->CountTo(TEXT("player_inventory/inv-1/consume")), 1);

	Cleanup(Fx.Dir);
	return true;
}

// ── A real-money item sent through the wallet purchase is refused on its own code, with its hint ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopRealMoneyItemTest, "Flock.Shop.Provider.ARealMoneyItemIsRefusedOnItsOwnCodeWithItsHint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopRealMoneyItemTest::RunTest(const FString& Parameters)
{
	// The body as the backend's shop settlement code builds it; the route's documented responses leave the code out.
	FFixture Fx(Retrying());
	Fx.SignIn();
	Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));
	Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Status(422,
		TEXT("{\"detail\":{\"code\":\"shop.real_currency_needs_checkout\",\"message\":\"'USD' is a real currency and cannot be paid from a wallet; open a checkout with POST /v1/shop/checkout, or verify a store receipt with POST /v1/shop/validate-receipt\"}}")));

	TOptional<TFlockResult<FFlockPurchaseResult>> Answer;
	Fx.Provider->Purchase(TEXT("item-1"), FString(), [&Answer](TFlockResult<FFlockPurchaseResult> R) { Answer = R; });
	FlockTestRetries::Pump();
	if (!TestTrue(TEXT("answered"), Answer.IsSet()))
	{
		Cleanup(Fx.Dir);
		return false;
	}
	TestFalse(TEXT("refused"), Answer->bSuccess);
	TestEqual(TEXT("on its own code, not Unknown"), Answer->Error.ErrorCode, EFlockErrorCode::ShopRealCurrencyNeedsCheckout);
	TestEqual(TEXT("as the 422 it is"), Answer->Error.StatusCode, 422);
	TestEqualSensitive(TEXT("with its hint"), Answer->Error.Hint, FFlockErrorHints::For(EFlockErrorCode::ShopRealCurrencyNeedsCheckout));
	TestFalse(TEXT("a hint there is"), Answer->Error.Hint.IsEmpty());
	TestTrue(TEXT("and the server's reason"), Answer->Error.Message.Contains(TEXT("real currency")) || Answer->Error.ServerMessage.Contains(TEXT("real currency")));
	TestEqual(TEXT("sent once"), Fx.Fake->CountTo(TEXT("shop/transaction")), 1);
	Cleanup(Fx.Dir);
	return true;
}

// ── An item switched off in the dashboard is refused on its own code, and the shops kept are dropped so its fix works ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopItemNotForSaleTest, "Flock.Shop.Provider.AnItemSwitchedOffDropsTheShopsKeptSoReadingAgainAsksTheServer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopItemNotForSaleTest::RunTest(const FString& Parameters)
{
	// Measured on the local stack on 2026-11-05: the wallet purchase of an item set Inactive in the dashboard.
	const FString NotForSale = TEXT("{\"detail\":{\"code\":\"shop.item_not_for_sale\",\"message\":\"This item is not for sale right now.\"}}");

	{
		FFixture Fx;
		Fx.SignIn();
		Fx.Fake->On(TEXT("by-name"), FFlockFakeTransport::Ok(ShopBody(TEXT("shop-x"))));
		Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));
		Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Status(409, NotForSale));

		Fx.Provider->GetByName(TEXT("Starter"), [](TFlockResult<FFlockShop>) {});
		Fx.Provider->GetByName(TEXT("Starter"), [](TFlockResult<FFlockShop>) {});
		// The control: without a drop, reading the shop again is answered from what was kept.
		TestEqual(TEXT("a shop read twice asks the server once"), Fx.Fake->CountTo(TEXT("by-name")), 1);

		TOptional<TFlockResult<FFlockPurchaseResult>> Answer;
		Fx.Provider->Purchase(TEXT("item-1"), FString(), [&Answer](TFlockResult<FFlockPurchaseResult> R) { Answer = R; });
		if (!TestTrue(TEXT("answered"), Answer.IsSet()))
		{
			Cleanup(Fx.Dir);
			return false;
		}
		TestFalse(TEXT("refused"), Answer->bSuccess);
		TestEqual(TEXT("on its own code, not Unknown"), Answer->Error.ErrorCode, EFlockErrorCode::ShopItemNotForSale);
		TestEqual(TEXT("as the 409 it is"), Answer->Error.StatusCode, 409);
		TestEqualSensitive(TEXT("with its fix"), Answer->Error.Hint, FFlockErrorHints::For(EFlockErrorCode::ShopItemNotForSale));
		TestEqual(TEXT("sent once: a refusal is certain, nothing was bought"), Fx.Fake->CountTo(TEXT("shop/transaction")), 1);

		// The fix says to read the shop again: that read asks the server, which leaves the switched-off item out.
		Fx.Provider->GetByName(TEXT("Starter"), [](TFlockResult<FFlockShop>) {});
		TestEqual(TEXT("reading the shop again asks the server"), Fx.Fake->CountTo(TEXT("by-name")), 2);
		Fx.Provider->GetItem(TEXT("item-1"), [](TFlockResult<FFlockShopItem>) {});
		TestEqual(TEXT("and so does reading the item again"), Fx.Fake->CountTo(TEXT("shop_item/item-1")), 2);

		// Any other refusal keeps the shops: only a switched-off item makes them wrong.
		Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Status(422,
			TEXT("{\"detail\":{\"code\":\"shop.insufficient_funds\",\"message\":\"Insufficient funds\"}}")));
		Fx.Provider->Purchase(TEXT("item-1"), FString(), [](TFlockResult<FFlockPurchaseResult>) {});
		Fx.Provider->GetByName(TEXT("Starter"), [](TFlockResult<FFlockShop>) {});
		TestEqual(TEXT("a purchase refused for another reason leaves the shops kept"), Fx.Fake->CountTo(TEXT("by-name")), 2);
		// A 409 is not the cause: a reused key is refused with 409 too, and keeps them.
		Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Status(409,
			TEXT("{\"detail\":{\"code\":\"shop.idempotency_key_reused\",\"message\":\"idempotency_key was already used for a different item\"}}")));
		Fx.Provider->Purchase(TEXT("item-1"), FString(), [](TFlockResult<FFlockPurchaseResult>) {});
		Fx.Provider->GetByName(TEXT("Starter"), [](TFlockResult<FFlockShop>) {});
		TestEqual(TEXT("a 409 refusal for another reason leaves the shops kept"), Fx.Fake->CountTo(TEXT("by-name")), 2);
		Cleanup(Fx.Dir);
	}

	{
		// The shops are dropped before the caller is told: a read made from the refusal's own handler asks the server.
		FFixture Fx;
		Fx.SignIn();
		Fx.Fake->On(TEXT("by-name"), FFlockFakeTransport::Ok(ShopBody(TEXT("shop-x"))));
		Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));
		Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Status(409, NotForSale));
		Fx.Provider->GetByName(TEXT("Starter"), [](TFlockResult<FFlockShop>) {});

		int32 ReadsByTheHandler = -1;
		const TSharedPtr<FFlockShopProvider> Provider = Fx.Provider;
		const TSharedRef<FFlockFakeTransport> Fake = Fx.Fake;
		Fx.Provider->Purchase(TEXT("item-1"), FString(), [Provider, Fake, &ReadsByTheHandler](TFlockResult<FFlockPurchaseResult>)
		{
			Provider->GetByName(TEXT("Starter"), [](TFlockResult<FFlockShop>) {});
			ReadsByTheHandler = Fake->CountTo(TEXT("by-name"));
		});
		TestEqual(TEXT("a read from the refusal's handler asks the server"), ReadsByTheHandler, 2);
		Cleanup(Fx.Dir);
	}

	{
		// The copy on disk goes too: a read that cannot reach the server afterwards fails, rather than serving the shop
		// with the switched-off item and keeping it again.
		const FString Dir = TempRoot();
		{
			FFixture Fx(NoRetry(), Dir);
			Fx.SignIn();
			Fx.Fake->On(TEXT("by-name"), FFlockFakeTransport::Ok(ShopBody(TEXT("shop-x"))));
			Fx.Provider->GetByName(TEXT("Starter"), [](TFlockResult<FFlockShop>) {});
		}
		{
			// The control: a provider with nothing in memory and the server out of reach is answered from the copy on disk.
			FFixture Fx(NoRetry(), Dir);
			Fx.SignIn();
			Fx.Fake->On(TEXT("by-name"), FFlockFakeTransport::Offline());
			bool bServed = false;
			Fx.Provider->GetByName(TEXT("Starter"), [&bServed](TFlockResult<FFlockShop> R) { bServed = R.bSuccess; });
			TestTrue(TEXT("without a drop, the copy on disk answers a read that cannot reach the server"), bServed);
		}
		{
			FFixture Fx(NoRetry(), Dir);
			Fx.SignIn();
			Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));
			Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Status(409, NotForSale));
			Fx.Provider->Purchase(TEXT("item-1"), FString(), [](TFlockResult<FFlockPurchaseResult>) {});
			Fx.Fake->On(TEXT("by-name"), FFlockFakeTransport::Offline());
			bool bServed = true;
			Fx.Provider->GetByName(TEXT("Starter"), [&bServed](TFlockResult<FFlockShop> R) { bServed = R.bSuccess; });
			TestFalse(TEXT("after the drop, a read that cannot reach the server is not answered from the old copy"), bServed);
		}
		Cleanup(Dir);
	}

	{
		// ClearCache, which the drop uses, keeps nothing in memory from a read on its way when it runs.
		FFixture Fx;
		Fx.Fake->On(TEXT("by-name"), FFlockFakeTransport::Ok(ShopBody(TEXT("shop-x"))));
		Fx.Fake->bDeferred = true;
		Fx.Provider->GetByName(TEXT("Starter"), [](TFlockResult<FFlockShop>) {});
		Fx.Provider->ClearCache();
		Fx.Fake->FlushPending();
		Fx.Fake->bDeferred = false;
		{
			// Nor on disk: a provider that cannot reach the server finds no copy of what that read answered.
			FFixture Offline(NoRetry(), Fx.Dir);
			Offline.Fake->On(TEXT("by-name"), FFlockFakeTransport::Offline());
			bool bServed = true;
			Offline.Provider->GetByName(TEXT("Starter"), [&bServed](TFlockResult<FFlockShop> R) { bServed = R.bSuccess; });
			TestFalse(TEXT("a read on its way across ClearCache left no copy on disk"), bServed);
		}
		Fx.Provider->GetByName(TEXT("Starter"), [](TFlockResult<FFlockShop>) {});
		TestEqual(TEXT("a read on its way across ClearCache kept nothing"), Fx.Fake->CountTo(TEXT("by-name")), 2);
		Cleanup(Fx.Dir);
	}

	{
		// Each of the five shop reads, on its way when the refusal lands, may have been answered before the item was switched
		// off: its caller gets the answer, and nothing of it is kept. First matching route wins, so the narrow ones come first.
		FFixture Fx;
		Fx.SignIn();
		Fx.Fake->On(TEXT("shop_item/shop/shop-2"), FFlockFakeTransport::Ok(ItemsEnvelopedBody()));
		Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));
		Fx.Fake->On(TEXT("shop_item/item-2"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-2"), 100)));
		Fx.Fake->On(TEXT("by-name"), FFlockFakeTransport::Ok(ShopBody(TEXT("shop-x"))));
		Fx.Fake->On(TEXT("v1/shop/shop-2"), FFlockFakeTransport::Ok(ShopBody(TEXT("shop-2"))));
		Fx.Fake->On(TEXT("shop?page"), FFlockFakeTransport::Ok(ShopPageBody()));
		Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Status(409, NotForSale));
		Fx.Fake->bDeferred = true;

		TOptional<TFlockResult<FFlockPurchaseResult>> Answer;
		Fx.Provider->Purchase(TEXT("item-1"), FString(), [&Answer](TFlockResult<FFlockPurchaseResult> R) { Answer = R; });
		Fx.Fake->FlushPending(); // the item lands, and the purchase goes out
		int32 ReadsAnswered = 0;
		Fx.Provider->GetByName(TEXT("Starter"), [&ReadsAnswered](TFlockResult<FFlockShop> R) { ReadsAnswered += R.bSuccess; });
		Fx.Provider->GetById(TEXT("shop-2"), [&ReadsAnswered](TFlockResult<FFlockShop> R) { ReadsAnswered += R.bSuccess; });
		Fx.Provider->GetAll(1, 100, [&ReadsAnswered](TFlockResult<FFlockShopPage> R) { ReadsAnswered += R.bSuccess; });
		Fx.Provider->GetItem(TEXT("item-2"), [&ReadsAnswered](TFlockResult<FFlockShopItem> R) { ReadsAnswered += R.bSuccess; });
		Fx.Provider->GetItemsByShop(TEXT("shop-2"), FString(),
			[&ReadsAnswered](TFlockResult<TArray<FFlockShopItem>> R) { ReadsAnswered += R.bSuccess; });
		Fx.Fake->FlushPending(); // the refusal lands first, then the five reads sent before it
		Fx.Fake->bDeferred = false;

		TestTrue(TEXT("the purchase was refused as not for sale"),
			Answer.IsSet() && Answer->Error.ErrorCode == EFlockErrorCode::ShopItemNotForSale);
		TestEqual(TEXT("each read in flight still answers its caller"), ReadsAnswered, 5);

		// Asked twice more: the first asks the server, as the read in flight kept nothing; the second is answered from what
		// the first kept, so a read sent after the drop is kept.
		for (int32 Round = 0; Round < 2; ++Round)
		{
			Fx.Provider->GetByName(TEXT("Starter"), [](TFlockResult<FFlockShop>) {});
			Fx.Provider->GetById(TEXT("shop-2"), [](TFlockResult<FFlockShop>) {});
			Fx.Provider->GetAll(1, 100, [](TFlockResult<FFlockShopPage>) {});
			Fx.Provider->GetItem(TEXT("item-2"), [](TFlockResult<FFlockShopItem>) {});
			Fx.Provider->GetItemsByShop(TEXT("shop-2"), FString(), [](TFlockResult<TArray<FFlockShopItem>>) {});
		}
		TestEqual(TEXT("by name: the read in flight kept nothing, the next one is kept"), Fx.Fake->CountTo(TEXT("by-name")), 2);
		TestEqual(TEXT("by id: the read in flight kept nothing, the next one is kept"), Fx.Fake->CountTo(TEXT("v1/shop/shop-2")), 2);
		TestEqual(TEXT("the page: the read in flight kept nothing, the next one is kept"), Fx.Fake->CountTo(TEXT("shop?page")), 2);
		TestEqual(TEXT("an item: the read in flight kept nothing, the next one is kept"), Fx.Fake->CountTo(TEXT("shop_item/item-2")), 2);
		TestEqual(TEXT("a shop's items: the read in flight kept nothing, the next one is kept"),
			Fx.Fake->CountTo(TEXT("shop_item/shop/shop-2")), 2);
		Cleanup(Fx.Dir);
	}
	return true;
}

// ── Made with the server unreachable: a consume is sent once, a purchase on each attempt under its key; neither is queued ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopOfflineMoneyTest, "Flock.Shop.Provider.APurchaseOrConsumeMadeOfflineIsNeverQueued",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopOfflineMoneyTest::RunTest(const FString& Parameters)
{
	FFixture Fx(Retrying());
	Fx.SignIn();
	Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));
	Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Offline());
	Fx.Fake->On(TEXT("player_inventory/inv-1/consume"), FFlockFakeTransport::Offline());

	EFlockErrorType PurchaseFailure = EFlockErrorType::None;
	Fx.Provider->Purchase(TEXT("item-1"), FString(), [&PurchaseFailure](TFlockResult<FFlockPurchaseResult> R) { PurchaseFailure = R.bSuccess ? EFlockErrorType::None : R.Error.Type; });
	EFlockErrorType ConsumeFailure = EFlockErrorType::None;
	Fx.Provider->Consume(TEXT("inv-1"), [&ConsumeFailure](TFlockResult<FFlockConsumeResult> R) { ConsumeFailure = R.bSuccess ? EFlockErrorType::None : R.Error.Type; });
	FlockTestRetries::Pump();
	TestEqual(TEXT("the purchase fails as unreachable"), PurchaseFailure, EFlockErrorType::Connection);
	TestEqual(TEXT("so does the consume"), ConsumeFailure, EFlockErrorType::Connection);

	// The network comes back: nothing held back is sent.
	Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Ok(PurchaseResultBody()));
	Fx.Fake->On(TEXT("player_inventory/inv-1/consume"), FFlockFakeTransport::Ok(ConsumeResultBody()));
	FlockTestRetries::Pump();
	TestEqual(TEXT("the purchase was sent on each attempt of its policy, under its key, and no more"), Fx.Fake->CountTo(TEXT("shop/transaction")), 4);
	TestEqual(TEXT("the consume was sent once"), Fx.Fake->CountTo(TEXT("player_inventory/inv-1/consume")), 1);

	// Contrast: a read made with the server unreachable is tried again under the same policy.
	Fx.Fake->On(TEXT("player_inventory/player/player-a"), FFlockFakeTransport::Offline());
	Fx.Provider->GetPlayerInventory(FString(), 1, 100, [](TFlockResult<FFlockPlayerInventoryPage>) {});
	FlockTestRetries::Pump();
	TestTrue(TEXT("contrast: a read is tried again"), Fx.Fake->CountTo(TEXT("player_inventory/player/player-a")) > 1);
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
// ── A purchase says where it has got to: paid by default, any other status the server sends read as it spelled it ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopPurchaseStatusTest, "Flock.Shop.Provider.PurchaseReadsItsStatus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopPurchaseStatusTest::RunTest(const FString& Parameters)
{
	auto Purchase = [this](const FString& Body)
	{
		FFixture Fx;
		Fx.SignIn();
		Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(RewardItemBody(TEXT("item-1"), 100)));
		Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Ok(Body));
		FFlockPurchaseResult Purchased;
		Fx.Provider->Purchase(TEXT("item-1"), FString(), [&Purchased](TFlockResult<FFlockPurchaseResult> R) { Purchased = R.Value; });
		return Purchased;
	};

	const FFlockPurchaseResult Pending = Purchase(TEXT("{\"purchase_id\":\"pur-2\",\"item_type\":\"standard\",\"status\":\"pending\"}"));
	TestEqualSensitive(TEXT("The status is read as the server spelled it"), Pending.Status, FString(FlockPurchaseStatuses::Pending));
	TestFalse(TEXT("A pending purchase is not paid"), Pending.IsPaid());
	TestFalse(TEXT("Nor to a graph"), UFlockShopLibrary::IsPurchasePaid(Pending));

	const FFlockPurchaseResult Unsaid = Purchase(PurchaseResultBody());
	TestEqualSensitive(TEXT("A status left out reads as paid, the schema's default"), Unsaid.Status, FString(FlockPurchaseStatuses::Paid));
	TestTrue(TEXT("And is paid"), Unsaid.IsPaid());
	TestTrue(TEXT("To a graph too"), UFlockShopLibrary::IsPurchasePaid(Unsaid));
	return true;
}

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
		FlockTestRetries::Pump();

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
		FlockTestRetries::Pump();

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

	FFlockPurchaseResult Pending;
	Pending.Status = FlockPurchaseStatuses::Pending;
	TestEqual(TEXT("Is Purchase Paid matches the struct"), UFlockShopLibrary::IsPurchasePaid(Pending), Pending.IsPaid());
	TestFalse(TEXT("and a pending purchase is not paid"), UFlockShopLibrary::IsPurchasePaid(Pending));
	TestTrue(TEXT("a purchase with no status read is paid"), UFlockShopLibrary::IsPurchasePaid(Bare));

	TestEqualSensitive(TEXT("pending is the wire spelling"), UFlockShopLibrary::PurchaseStatusPending(), FString(FlockPurchaseStatuses::Pending));
	TestEqualSensitive(TEXT("paid is the wire spelling"), UFlockShopLibrary::PurchaseStatusPaid(), FString(FlockPurchaseStatuses::Paid));
	TestEqualSensitive(TEXT("failed is the wire spelling"), UFlockShopLibrary::PurchaseStatusFailed(), FString(FlockPurchaseStatuses::Failed));
	TestEqualSensitive(TEXT("expired is the wire spelling"), UFlockShopLibrary::PurchaseStatusExpired(), FString(FlockPurchaseStatuses::Expired));
	TestEqualSensitive(TEXT("refunded is the wire spelling"), UFlockShopLibrary::PurchaseStatusRefunded(), FString(FlockPurchaseStatuses::Refunded));
	TestEqualSensitive(TEXT("and the constants are the schema's"), FString(FlockPurchaseStatuses::Paid), FString(TEXT("paid")));

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

// ── A purchase refused for the wallet drops the cached wallet, so the read its fix asks for asks the server ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopUnaffordableDropsCachedWalletTest, "Flock.Shop.Provider.APurchaseRefusedForTheWalletDropsTheCachedWallet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopUnaffordableDropsCachedWalletTest::RunTest(const FString& Parameters)
{
	struct FRefusal
	{
		const TCHAR* What;
		int32 Status;
		const TCHAR* Body;
		bool bDropsTheWallet;
	};
	// The backend's own messages (shop helpers/transaction.py); a refusal for another reason keeps the cached wallet.
	const FRefusal Refusals[] = {
		{ TEXT("not enough funds"), 422, TEXT("{\"detail\":{\"code\":\"shop.insufficient_funds\",\"message\":\"Insufficient funds: have 50 GOLD, need 100\"}}"), true },
		{ TEXT("no balance in the currency"), 422, TEXT("{\"detail\":{\"code\":\"shop.currency_not_held\",\"message\":\"Currency 'GOLD' not found in player wallet\"}}"), true },
		{ TEXT("an item switched off"), 409, TEXT("{\"detail\":{\"code\":\"shop.item_not_for_sale\",\"message\":\"This item is not for sale right now.\"}}"), false },
	};
	for (const FRefusal& Refusal : Refusals)
	{
		FFixture Fx;
		Fx.SignIn();
		Fx.WirePlayers();
		TestTrue(FString::Printf(TEXT("%s: player cache primed"), Refusal.What), Fx.PrimePlayerCache(/*Gold*/ 100));
		Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));
		Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Status(Refusal.Status, Refusal.Body));

		bool bRefused = false;
		Fx.Provider->Purchase(TEXT("item-1"), FString(), [&bRefused](TFlockResult<FFlockPurchaseResult> R) { bRefused = !R.bSuccess; });
		TestTrue(FString::Printf(TEXT("%s: refused"), Refusal.What), bRefused);
		TestEqual(FString::Printf(TEXT("%s: %s"), Refusal.What,
			Refusal.bDropsTheWallet ? TEXT("the cached wallet is dropped") : TEXT("the cached wallet is kept")),
			Fx.CachedGold(), Refusal.bDropsTheWallet ? -1 : 100);
		Cleanup(Fx.Dir);
	}
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

// ── A purchase acts for the sign-in it started under: a switch while its item is read sends nothing and records nothing ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopPurchaseKeepsItsSignInTest, "Flock.Shop.Provider.PurchaseIsNotSentAfterItsSignInEnds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopPurchaseKeepsItsSignInTest::RunTest(const FString& Parameters)
{
	auto RunPurchase = [this](bool bSwitchPlayerMeanwhile, int32& OutPurchasePosts, int32& OutRecords, EFlockErrorType& OutErrorType)
	{
		FFixture Fx;
		Fx.SignIn(TEXT("player-a"));
		Fx.WireAnalytics();
		Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));
		Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Ok(PurchaseResultBody()));
		Fx.Fake->On(TEXT("analytics/transactions"), FFlockFakeTransport::Ok(TEXT("{}")));

		// The item read is held, so another player signs in between the purchase and its send.
		Fx.Fake->bDeferred = true;
		bool bDone = false;
		Fx.Provider->Purchase(TEXT("item-1"), FString(), [&](TFlockResult<FFlockPurchaseResult> R)
		{
			bDone = true;
			OutErrorType = R.bSuccess ? EFlockErrorType::None : R.Error.Type;
		});
		TestFalse(TEXT("the purchase waits for its item"), bDone);
		if (bSwitchPlayerMeanwhile)
		{
			Fx.SignIn(TEXT("player-b"));
		}
		Fx.Fake->bDeferred = false;
		Fx.Fake->FlushPending();

		TestTrue(TEXT("the purchase answered"), bDone);
		OutPurchasePosts = Fx.Fake->CountTo(TEXT("shop/transaction"));
		OutRecords = Fx.Fake->CountTo(TEXT("analytics/transactions"));
		Cleanup(Fx.Dir);
	};

	int32 Posts = -1;
	int32 Records = -1;
	EFlockErrorType Type = EFlockErrorType::None;

	// Control: the same sign-in all along sends the purchase and records Started and Purchased.
	RunPurchase(false, Posts, Records, Type);
	TestEqual(TEXT("control: one purchase POST"), Posts, 1);
	TestEqual(TEXT("control: Started + Purchased recorded"), Records, 2);
	TestEqual(TEXT("control: succeeded"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::None));

	RunPurchase(true, Posts, Records, Type);
	TestEqual(TEXT("after the switch: no purchase POST as the next player"), Posts, 0);
	TestEqual(TEXT("after the switch: nothing recorded for the next player"), Records, 0);
	TestEqual(TEXT("after the switch: cancelled"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Cancelled));
	return true;
}

// ── Real-money items: the server's flag and each store's product id, on every catalog read ──
// Bodies as the backend answered them for the payment probe's shop (main 644bf45 on 2026-10-26; the same bytes from
// fix/tap-checkout e601aa97 on 2026-11-05): two items priced in USD, a real currency there, and two in Gold. shop/{id} and
// shop/by-name answer the shop at the root, the shop list pages it, and shop_item/shop/{id} wraps the items in {"result":...}.
namespace FlockShopRealMoneyFixtures
{
	const TCHAR* const ProbeShopId = TEXT("01M5EM6PVCVP955VDV3FJ8N3BH");
	const TCHAR* const GemsPackId = TEXT("01M5EM6PVZN764Q4NWT3PRGM3T");

	const TCHAR* const SwordItem = TEXT(R"JSON({"id":"01M5EM6PVZ7ZMR42VRKY3HPHYD","name":"pv00 sword","status":"active","type":"standard","shop_id":"01M5EM6PVCVP955VDV3FJ8N3BH","patch_id":null,"price":10,"currency":"Gold","is_real_money":false,"rewards":[],"store_skus":{"apple_app_store":"com.flock.pv00.sword"},"data":{},"created_at":"2026-10-21T07:00:57.855275Z","updated_at":"2026-10-21T07:00:57.855276Z"})JSON");
	const TCHAR* const ShieldItem = TEXT(R"JSON({"id":"01M5EM6PVZPG7EWSZ49TXARBYV","name":"pv00 shield","status":"active","type":"standard","shop_id":"01M5EM6PVCVP955VDV3FJ8N3BH","patch_id":null,"price":10,"currency":"Gold","is_real_money":false,"rewards":[],"store_skus":{},"data":{},"created_at":"2026-10-21T07:00:57.855309Z","updated_at":"2026-10-21T07:00:57.855311Z"})JSON");
	const TCHAR* const GemsPackItem = TEXT(R"JSON({"id":"01M5EM6PVZN764Q4NWT3PRGM3T","name":"pv00 gems pack","status":"active","type":"currency_pack","shop_id":"01M5EM6PVCVP955VDV3FJ8N3BH","patch_id":null,"price":199,"currency":"USD","is_real_money":true,"rewards":[{"type":"currency","code":"Gold","amount":100}],"store_skus":{"google_play":"com.flock.pv00.gems","apple_app_store":"com.flock.pv00.gems"},"data":{},"created_at":"2026-10-21T07:00:57.855168Z","updated_at":"2026-10-21T07:00:57.855173Z"})JSON");
	const TCHAR* const GemsPackWithoutProductIdsItem = TEXT(R"JSON({"id":"01M5EM6PVZJT2C0K06M61RZFGB","name":"pv00 gems pack without product ids","status":"active","type":"currency_pack","shop_id":"01M5EM6PVCVP955VDV3FJ8N3BH","patch_id":null,"price":199,"currency":"USD","is_real_money":true,"rewards":[{"type":"currency","code":"Gold","amount":100}],"store_skus":{},"data":{},"created_at":"2026-10-21T07:00:57.855245Z","updated_at":"2026-10-21T07:00:57.855247Z"})JSON");

	// The same item from a server without the real-money flag (main aa402fc, the payment probe of 2026-10-21).
	const TCHAR* const GemsPackFromAServerWithoutTheFlag = TEXT(R"JSON({"id":"01M5EM6PVZN764Q4NWT3PRGM3T","name":"pv00 gems pack","status":"active","type":"currency_pack","shop_id":"01M5EM6PVCVP955VDV3FJ8N3BH","patch_id":null,"price":199,"currency":"USD","rewards":[{"type":"currency","code":"Gold","amount":100}],"store_skus":{"google_play":"com.flock.pv00.gems","apple_app_store":"com.flock.pv00.gems"},"data":{},"created_at":"2026-10-21T07:00:57.855168Z","updated_at":"2026-10-21T07:00:57.855173Z"})JSON");

	FString ProbeShopItems()
	{
		return FString::Printf(TEXT("[%s,%s,%s,%s]"), SwordItem, ShieldItem, GemsPackItem, GemsPackWithoutProductIdsItem);
	}

	FString ProbeShop()
	{
		return FString::Printf(
			TEXT(R"JSON({"id":"01M5EM6PVCVP955VDV3FJ8N3BH","name":"pv00-shop","status":"active","game_id":"01M5EM6PTHKSYB8ZFPSSRAJ7C2","game_version_id":"01M5EM6PTY4M5PA1A93GQXQYDP","data":{"stats":null,"web_shop_url":null,"pwa_shop_url":null},"shop_items":%s,"created_at":"2026-10-21T07:00:57.836159Z","updated_at":"2026-10-21T07:00:57.836163Z"})JSON"),
			*ProbeShopItems());
	}

	/** The item named Name, or null; the caller gates on it, so a missing item fails the test rather than the process. */
	const FFlockShopItem* Named(const TArray<FFlockShopItem>& Items, const TCHAR* Name)
	{
		return Items.FindByPredicate([Name](const FFlockShopItem& Item) { return Item.Name.Equals(Name, ESearchCase::CaseSensitive); });
	}

	/** Each store's product id, spelled exactly as the item holds it; empty when the item has none in Store. */
	FString IdIn(const FFlockShopItem& Item, const TCHAR* Store)
	{
		FString ProductId;
		Item.FindStoreProductId(Store, ProductId);
		return ProductId;
	}

	/** Every store key the item holds, sorted, spelled as held: a map's own Find ignores case, so this checks spelling. */
	FString StoreKeys(const FFlockShopItem& Item)
	{
		TArray<FString> Keys;
		Item.StoreProductIds.GetKeys(Keys);
		Keys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
		return FString::Join(Keys, TEXT(","));
	}

	void TestProbeShopItems(FAutomationTestBase& Test, const TArray<FFlockShopItem>& Items)
	{
		Test.TestEqual(TEXT("the probe shop's four items"), Items.Num(), 4);

		if (const FFlockShopItem* Pack = Named(Items, TEXT("pv00 gems pack")); Test.TestNotNull(TEXT("the gems pack"), Pack))
		{
			Test.TestTrue(TEXT("priced in USD, which the game's currency config lists as real money"), Pack->IsRealMoney);
			Test.TestEqual(TEXT("a real-money price is in the currency's smallest unit"), Pack->Price, 199);
			FlockEngineCompat::CheckEqualSpelledExactly(Test, TEXT("its Google Play product id"), IdIn(*Pack, FlockPaymentProviders::GooglePlay), FString(TEXT("com.flock.pv00.gems")));
			FlockEngineCompat::CheckEqualSpelledExactly(Test, TEXT("its App Store product id"), IdIn(*Pack, FlockPaymentProviders::AppleAppStore), FString(TEXT("com.flock.pv00.gems")));
			FlockEngineCompat::CheckEqualSpelledExactly(Test, TEXT("each store's name as the server spells it"), StoreKeys(*Pack), FString(TEXT("apple_app_store,google_play")));
		}
		if (const FFlockShopItem* Bare = Named(Items, TEXT("pv00 gems pack without product ids")); Test.TestNotNull(TEXT("the pack without ids"), Bare))
		{
			Test.TestTrue(TEXT("a real-money item with no product id is still real money"), Bare->IsRealMoney);
			Test.TestEqual(TEXT("no product id reads as an empty map"), Bare->StoreProductIds.Num(), 0);
		}
		if (const FFlockShopItem* Sword = Named(Items, TEXT("pv00 sword")); Test.TestNotNull(TEXT("the sword"), Sword))
		{
			Test.TestFalse(TEXT("priced in Gold, a game currency"), Sword->IsRealMoney);
			FlockEngineCompat::CheckEqualSpelledExactly(Test, TEXT("the sword's one store"), StoreKeys(*Sword), FString(FlockPaymentProviders::AppleAppStore));
		}
		if (const FFlockShopItem* Shield = Named(Items, TEXT("pv00 shield")); Test.TestNotNull(TEXT("the shield"), Shield))
		{
			Test.TestFalse(TEXT("the shield is not real money"), Shield->IsRealMoney);
			Test.TestEqual(TEXT("the shield has no product id"), Shield->StoreProductIds.Num(), 0);
		}
	}

	/** One item read by id through the provider, from Body. False when the read failed. */
	bool ReadItem(FAutomationTestBase& Test, const FString& Body, FFlockShopItem& OutItem)
	{
		FFixture Fx;
		Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(Body));
		bool bRead = false;
		Fx.Provider->GetItem(TEXT("item-1"), [&](TFlockResult<FFlockShopItem> R) { bRead = R.bSuccess; OutItem = R.Value; });
		Cleanup(Fx.Dir);
		return Test.TestTrue(TEXT("the item read succeeds"), bRead);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopRealMoneyEveryReadTest, "Flock.Shop.RealMoney.EveryCatalogReadCarriesEachItemsFlagAndStoreProductIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopRealMoneyEveryReadTest::RunTest(const FString& Parameters)
{
	using namespace FlockShopRealMoneyFixtures;
	{
		FFixture Fx;
		Fx.Fake->On(TEXT("shop/by-name/pv00-shop"), FFlockFakeTransport::Ok(ProbeShop()));
		FFlockShop Shop;
		bool bRead = false;
		Fx.Provider->GetByName(TEXT("pv00-shop"), [&](TFlockResult<FFlockShop> R) { bRead = R.bSuccess; Shop = R.Value; });
		AddInfo(TEXT("By name:"));
		if (TestTrue(TEXT("by name"), bRead)) { TestProbeShopItems(*this, Shop.ShopItems); }
		Cleanup(Fx.Dir);
	}
	{
		FFixture Fx;
		Fx.Fake->On(FString::Printf(TEXT("shop/%s"), ProbeShopId), FFlockFakeTransport::Ok(ProbeShop()));
		FFlockShop Shop;
		bool bRead = false;
		Fx.Provider->GetById(ProbeShopId, [&](TFlockResult<FFlockShop> R) { bRead = R.bSuccess; Shop = R.Value; });
		AddInfo(TEXT("By id:"));
		if (TestTrue(TEXT("by id"), bRead)) { TestProbeShopItems(*this, Shop.ShopItems); }
		Cleanup(Fx.Dir);
	}
	{
		FFixture Fx;
		Fx.Fake->On(TEXT("shop?page=1"), FFlockFakeTransport::Ok(
			FString::Printf(TEXT("{\"items\":[%s],\"total\":1,\"page\":1,\"limit\":100}"), *ProbeShop())));
		FFlockShopPage Page;
		bool bRead = false;
		Fx.Provider->GetAll(1, 100, [&](TFlockResult<FFlockShopPage> R) { bRead = R.bSuccess; Page = R.Value; });
		AddInfo(TEXT("The shop list:"));
		if (TestTrue(TEXT("the list"), bRead) && TestEqual(TEXT("one shop"), Page.Items.Num(), 1))
		{
			TestProbeShopItems(*this, Page.Items[0].ShopItems);
		}
		Cleanup(Fx.Dir);
	}
	{
		FFixture Fx;
		Fx.Fake->On(FString::Printf(TEXT("shop_item/shop/%s"), ProbeShopId), FFlockFakeTransport::Ok(FString::Printf(
			TEXT("{\"error\":{\"code\":null},\"response\":{\"message\":null,\"code\":null},\"result\":%s}"), *ProbeShopItems())));
		TArray<FFlockShopItem> Items;
		bool bRead = false;
		Fx.Provider->GetItemsByShop(ProbeShopId, FString(), [&](TFlockResult<TArray<FFlockShopItem>> R) { bRead = R.bSuccess; Items = R.Value; });
		AddInfo(TEXT("The items of a shop:"));
		if (TestTrue(TEXT("the items of a shop"), bRead)) { TestProbeShopItems(*this, Items); }
		Cleanup(Fx.Dir);
	}
	{
		FFixture Fx;
		Fx.Fake->On(FString::Printf(TEXT("shop_item/%s"), GemsPackId), FFlockFakeTransport::Ok(GemsPackItem));
		FFlockShopItem Item;
		bool bRead = false;
		Fx.Provider->GetItem(GemsPackId, [&](TFlockResult<FFlockShopItem> R) { bRead = R.bSuccess; Item = R.Value; });
		if (TestTrue(TEXT("one item by id"), bRead))
		{
			TestTrue(TEXT("the item read by id is real money"), Item.IsRealMoney);
			TestEqualSensitive(TEXT("its kind"), Item.Type, FString(FlockShopItemTypes::CurrencyPack));
			TestEqual(TEXT("two stores"), Item.StoreProductIds.Num(), 2);
			TestEqualSensitive(TEXT("its Google Play product id"), IdIn(Item, FlockPaymentProviders::GooglePlay), FString(TEXT("com.flock.pv00.gems")));
		}
		Cleanup(Fx.Dir);
	}
	return true;
}

// The flag is the server's word: an older server leaves it out, and the SDK never guesses it from the currency.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopRealMoneyServersWordTest, "Flock.Shop.RealMoney.TheFlagIsTheServersWordAndNeverFailsTheRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopRealMoneyServersWordTest::RunTest(const FString& Parameters)
{
	using namespace FlockShopRealMoneyFixtures;
	FFlockShopItem Item;
	if (ReadItem(*this, GemsPackFromAServerWithoutTheFlag, Item))
	{
		TestFalse(TEXT("no flag reads false, even for an item priced in USD"), Item.IsRealMoney);
		TestEqualSensitive(TEXT("and its product ids are still read"), IdIn(Item, FlockPaymentProviders::AppleAppStore), FString(TEXT("com.flock.pv00.gems")));
	}
	if (ReadItem(*this, TEXT(R"JSON({"id":"item-1","price":10,"currency":"Gold","is_real_money":null,"store_skus":{}})JSON"), Item))
	{
		TestEqualSensitive(TEXT("a null flag does not fail the read"), Item.Id, FString(TEXT("item-1")));
		TestFalse(TEXT("a null flag reads false"), Item.IsRealMoney);
	}
	if (ReadItem(*this, TEXT(R"JSON({"id":"item-1","price":199,"currency":"USD","is_real_money":"true","store_skus":{}})JSON"), Item))
	{
		TestFalse(TEXT("a flag that is not a JSON true reads false"), Item.IsRealMoney);
	}
	// Control: the same body with a JSON true reads true, so the case above is the value's type and not the read.
	if (ReadItem(*this, TEXT(R"JSON({"id":"item-1","price":199,"currency":"USD","is_real_money":true,"store_skus":{}})JSON"), Item))
	{
		TestTrue(TEXT("control: a JSON true reads true"), Item.IsRealMoney);
	}
	return true;
}

// Each store's product id is kept exactly as the dashboard saved it, a store this SDK does not know included.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopStoreProductIdsVerbatimTest, "Flock.Shop.RealMoney.StoreProductIdsAreKeptVerbatim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopStoreProductIdsVerbatimTest::RunTest(const FString& Parameters)
{
	using namespace FlockShopRealMoneyFixtures;
	FFlockShopItem Item;
	if (ReadItem(*this, TEXT(R"JSON({"id":"item-1","price":10,"currency":"Gold","is_real_money":false,"store_skus":null})JSON"), Item))
	{
		TestEqual(TEXT("a null map reads empty"), Item.StoreProductIds.Num(), 0);
	}
	if (ReadItem(*this, TEXT(R"JSON({"id":"item-1","price":10,"currency":"Gold"})JSON"), Item))
	{
		TestEqual(TEXT("an absent map reads empty"), Item.StoreProductIds.Num(), 0);
	}
	if (ReadItem(*this, TEXT(R"JSON({"id":"item-1","price":199,"currency":"USD","is_real_money":true,"store_skus":{"google_play":"com.flock.gems","steam":"480"}})JSON"), Item))
	{
		TestEqualSensitive(TEXT("a store this SDK does not know is kept"), IdIn(Item, TEXT("steam")), FString(TEXT("480")));
		TestEqualSensitive(TEXT("beside the ones it knows"), IdIn(Item, FlockPaymentProviders::GooglePlay), FString(TEXT("com.flock.gems")));
	}
	// A store's name is kept as the server spells it, capitals included: every name above is lower case, which a read
	// that lower-cased names would pass.
	if (ReadItem(*this, TEXT(R"JSON({"id":"item-1","price":199,"currency":"USD","is_real_money":true,"store_skus":{"Steam":"480"}})JSON"), Item))
	{
		TestEqualSensitive(TEXT("a name with capitals keeps them"), StoreKeys(Item), FString(TEXT("Steam")));
	}
	if (ReadItem(*this, TEXT(R"JSON({"id":"item-1","price":199,"currency":"USD","is_real_money":true,"store_skus":{"apple_app_store":"com.Flock.Gems_Pack.V2"}})JSON"), Item))
	{
		TestEqualSensitive(TEXT("a store compares ids exactly, so each is kept as saved"), IdIn(Item, FlockPaymentProviders::AppleAppStore), FString(TEXT("com.Flock.Gems_Pack.V2")));
	}
	if (ReadItem(*this, TEXT(R"JSON({"id":"item-1","price":199,"currency":"USD","is_real_money":true,"store_skus":{"google_play":42,"apple_app_store":"com.flock.gems"}})JSON"), Item))
	{
		TestEqual(TEXT("an id that is not a string is left out"), Item.StoreProductIds.Num(), 1);
		TestEqualSensitive(TEXT("and the item keeps the others"), IdIn(Item, FlockPaymentProviders::AppleAppStore), FString(TEXT("com.flock.gems")));
	}
	return true;
}

// The lookup matches a store's name with its letter case; a map's own Find, and TestEqual on strings, ignore it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopFindStoreProductIdTest, "Flock.Shop.RealMoney.FindStoreProductIdMatchesTheStoreNameExactly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopFindStoreProductIdTest::RunTest(const FString& Parameters)
{
	FFlockShopItem Item;
	Item.StoreProductIds.Add(FlockPaymentProviders::GooglePlay, TEXT("com.flock.gems"));

	FString ProductId = TEXT("left over");
	TestTrue(TEXT("the store's own name finds it"), Item.FindStoreProductId(FlockPaymentProviders::GooglePlay, ProductId));
	TestEqualSensitive(TEXT("its id"), ProductId, FString(TEXT("com.flock.gems")));
	TestFalse(TEXT("another spelling of the name does not"), Item.FindStoreProductId(TEXT("Google_Play"), ProductId));
	TestTrue(TEXT("and leaves no id behind"), ProductId.IsEmpty());
	TestFalse(TEXT("a store the item is not in"), Item.FindStoreProductId(FlockPaymentProviders::AppleAppStore, ProductId));

	FString FromTheNode;
	TestEqual(TEXT("the Blueprint node answers as C++ does"),
		UFlockShopLibrary::FindStoreProductId(Item, FlockPaymentProviders::GooglePlay, FromTheNode), true);
	TestEqualSensitive(TEXT("with the same id"), FromTheNode, FString(TEXT("com.flock.gems")));
	TestFalse(TEXT("and refuses another spelling as C++ does"), UFlockShopLibrary::FindStoreProductId(Item, TEXT("GOOGLE_PLAY"), FromTheNode));

	// The reward kind is compared with its letter case too.
	FFlockShopItemReward Reward;
	Reward.Type = TEXT("Currency");
	TestFalse(TEXT("a reward kind spelled otherwise is not currency"), Reward.IsCurrency());
	return true;
}

// A relaunch with no network serves the catalog from the snapshot the last online read wrote, flag and ids included.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopRealMoneySnapshotTest, "Flock.Shop.RealMoney.AnOfflineRelaunchKeepsTheFlagAndStoreProductIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopRealMoneySnapshotTest::RunTest(const FString& Parameters)
{
	using namespace FlockShopRealMoneyFixtures;
	const FString Dir = TempRoot();
	const FString Route = FString::Printf(TEXT("shop_item/%s"), GemsPackId);
	const FString ListRoute = FString::Printf(TEXT("shop_item/shop/%s"), ProbeShopId);
	const TCHAR* const CapitalsRoute = TEXT("shop_item/item-capitals");
	{
		FFixture Online(NoRetry(), Dir);
		Online.Fake->On(Route, FFlockFakeTransport::Ok(GemsPackItem));
		Online.Fake->On(ListRoute, FFlockFakeTransport::Ok(FString::Printf(
			TEXT("{\"error\":{\"code\":null},\"response\":{\"message\":null,\"code\":null},\"result\":%s}"), *ProbeShopItems())));
		Online.Fake->On(CapitalsRoute, FFlockFakeTransport::Ok(TEXT(R"JSON({"id":"item-capitals","price":199,"currency":"USD","is_real_money":true,"store_skus":{"Steam":"480"}})JSON")));
		bool bRead = false;
		Online.Provider->GetItem(GemsPackId, [&](TFlockResult<FFlockShopItem> R) { bRead = R.bSuccess; });
		TestTrue(TEXT("the online read succeeds"), bRead);
		bool bListRead = false;
		Online.Provider->GetItemsByShop(ProbeShopId, FString(), [&](TFlockResult<TArray<FFlockShopItem>> R) { bListRead = R.bSuccess; });
		TestTrue(TEXT("the online list read succeeds"), bListRead);
		bool bCapitalsRead = false;
		Online.Provider->GetItem(TEXT("item-capitals"), [&](TFlockResult<FFlockShopItem> R) { bCapitalsRead = R.bSuccess; });
		TestTrue(TEXT("the online read of a store named with capitals succeeds"), bCapitalsRead);
	}
	{
		// The list of a shop's items takes the snapshot's list path, not the one-item path.
		FFixture Offline(NoRetry(), Dir);
		Offline.Provider->SetReachabilityProbe([]() { return false; });
		Offline.Fake->On(ListRoute, FFlockFakeTransport::Offline());
		Offline.Fake->On(CapitalsRoute, FFlockFakeTransport::Offline());
		TArray<FFlockShopItem> Items;
		bool bServed = false;
		Offline.Provider->GetItemsByShop(ProbeShopId, FString(), [&](TFlockResult<TArray<FFlockShopItem>> R) { bServed = R.bSuccess; Items = R.Value; });
		AddInfo(TEXT("The items of a shop, offline:"));
		if (TestTrue(TEXT("the list is served offline"), bServed)) { TestProbeShopItems(*this, Items); }
		TestEqual(TEXT("the list from the snapshot, not the network"), Offline.Fake->CountTo(ListRoute), 0);

		FFlockShopItem Capitals;
		bool bCapitalsServed = false;
		Offline.Provider->GetItem(TEXT("item-capitals"), [&](TFlockResult<FFlockShopItem> R) { bCapitalsServed = R.bSuccess; Capitals = R.Value; });
		TestTrue(TEXT("the item named with capitals is served offline"), bCapitalsServed);
		TestEqualSensitive(TEXT("a store's name keeps its capitals through the snapshot"), StoreKeys(Capitals), FString(TEXT("Steam")));
	}
	{
		FFixture Offline(NoRetry(), Dir);
		Offline.Provider->SetReachabilityProbe([]() { return false; });
		Offline.Fake->On(Route, FFlockFakeTransport::Offline());
		FFlockShopItem Item;
		bool bServed = false;
		Offline.Provider->GetItem(GemsPackId, [&](TFlockResult<FFlockShopItem> R) { bServed = R.bSuccess; Item = R.Value; });
		TestTrue(TEXT("served offline"), bServed);
		TestEqual(TEXT("from the snapshot, not the network"), Offline.Fake->CountTo(Route), 0);
		TestTrue(TEXT("the flag survives the snapshot"), Item.IsRealMoney);
		TestEqualSensitive(TEXT("each store's name survives spelled as sent"), StoreKeys(Item), FString(TEXT("apple_app_store,google_play")));
		TestEqualSensitive(TEXT("the Google Play id survives"), IdIn(Item, FlockPaymentProviders::GooglePlay), FString(TEXT("com.flock.pv00.gems")));
		TestEqualSensitive(TEXT("the App Store id survives"), IdIn(Item, FlockPaymentProviders::AppleAppStore), FString(TEXT("com.flock.pv00.gems")));
	}
	Cleanup(Dir);
	return true;
}

// The names the backend's ShopItemType and PaymentProvider use (read 2026-11-05); the body-read tests above prove the two
// stores and two kinds a live answer carried. The Blueprint nodes hand over the same constants.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopItemKindsAndStoresTest, "Flock.Shop.RealMoney.ItemKindsAndStoreNamesAreTheServersSpelling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockShopItemKindsAndStoresTest::RunTest(const FString& Parameters)
{
	TestEqualSensitive(TEXT("standard"), FString(FlockShopItemTypes::Standard), FString(TEXT("standard")));
	TestEqualSensitive(TEXT("currency_pack"), FString(FlockShopItemTypes::CurrencyPack), FString(TEXT("currency_pack")));
	TestEqualSensitive(TEXT("consumable"), FString(FlockShopItemTypes::Consumable), FString(TEXT("consumable")));
	TestEqualSensitive(TEXT("apple_app_store"), FString(FlockPaymentProviders::AppleAppStore), FString(TEXT("apple_app_store")));
	TestEqualSensitive(TEXT("google_play"), FString(FlockPaymentProviders::GooglePlay), FString(TEXT("google_play")));
	TestEqualSensitive(TEXT("stripe"), FString(FlockPaymentProviders::Stripe), FString(TEXT("stripe")));
	TestEqualSensitive(TEXT("tap"), FString(FlockPaymentProviders::Tap), FString(TEXT("tap")));
	TestEqualSensitive(TEXT("moyasar"), FString(FlockPaymentProviders::Moyasar), FString(TEXT("moyasar")));

	TestEqualSensitive(TEXT("node: standard"), UFlockShopLibrary::ShopItemTypeStandard(), FString(FlockShopItemTypes::Standard));
	TestEqualSensitive(TEXT("node: currency pack"), UFlockShopLibrary::ShopItemTypeCurrencyPack(), FString(FlockShopItemTypes::CurrencyPack));
	TestEqualSensitive(TEXT("node: consumable"), UFlockShopLibrary::ShopItemTypeConsumable(), FString(FlockShopItemTypes::Consumable));
	TestEqualSensitive(TEXT("node: App Store"), UFlockShopLibrary::PaymentProviderAppleAppStore(), FString(FlockPaymentProviders::AppleAppStore));
	TestEqualSensitive(TEXT("node: Google Play"), UFlockShopLibrary::PaymentProviderGooglePlay(), FString(FlockPaymentProviders::GooglePlay));
	TestEqualSensitive(TEXT("node: Stripe"), UFlockShopLibrary::PaymentProviderStripe(), FString(FlockPaymentProviders::Stripe));
	TestEqualSensitive(TEXT("node: Tap"), UFlockShopLibrary::PaymentProviderTap(), FString(FlockPaymentProviders::Tap));
	TestEqualSensitive(TEXT("node: Moyasar"), UFlockShopLibrary::PaymentProviderMoyasar(), FString(FlockPaymentProviders::Moyasar));
	return true;
}

// ── Redeeming a store's purchase (shop/validate-receipt) ──
// The answer is the purchase at the root, as the wallet purchase's is; a delivered store purchase reads paid. Refusals as the
// backend sent them to the payment probe on 2026-10-26 (main 644bf45); the last three are the backend's own messages for causes
// no probe can reach without a real store purchase.
namespace FlockStoreRedemptionFixtures
{
	const TCHAR* const RedeemedPack =
		TEXT("{\"purchase_id\":\"pur-9\",\"item_type\":\"currency_pack\",\"status\":\"paid\",\"inventory\":null,")
		TEXT("\"granted\":[{\"type\":\"currency\",\"code\":\"gold\",\"amount\":100}],\"wallet\":");

	/** The wallet in the shape the player cache keeps (the write-through tests' row), with an explicit Gold balance. */
	FString WalletJson(int32 Gold)
	{
		return FString::Printf(
			TEXT("{\"id\":\"pd-1\",\"player_template_id\":\"tmpl-1\",\"game_id\":\"g\",\"player_id\":\"player-a\",")
			TEXT("\"data\":[{\"type\":\"int\",\"field_name\":\"gold\",\"value\":%d}],\"created_at\":\"\",\"updated_at\":\"\"}"), Gold);
	}

	struct FReceiptRefusal
	{
		int32 Status;
		const TCHAR* Body;
		EFlockErrorCode Expected;
	};

	const FReceiptRefusal ReceiptRefusals[] = {
		{ 422, TEXT("{\"detail\":{\"code\":\"payment_provider.store_credentials_rejected\",\"message\":\"Apple rejected this game's store credentials (HTTP 401); the studio's payment settings need fixing before receipts can be verified\"}}"), EFlockErrorCode::PaymentProviderStoreCredentialsRejected },
		{ 422, TEXT("{\"detail\":{\"code\":\"payment_provider.receipt_invalid\",\"message\":\"Receipt rejected: no transaction_id supplied\"}}"), EFlockErrorCode::PaymentProviderReceiptInvalid },
		{ 422, TEXT("{\"detail\":{\"code\":\"payment_provider.receipt_product_mismatch\",\"message\":\"This item has no apple_app_store product id configured\"}}"), EFlockErrorCode::PaymentProviderReceiptProductMismatch },
		{ 404, TEXT("{\"detail\":{\"code\":\"payment_provider.provider_not_found\",\"message\":\"Provider 'apple_app_store' is not configured for this game\"}}"), EFlockErrorCode::PaymentProviderProviderNotFound },
		{ 404, TEXT("{\"detail\":{\"code\":\"shop.item_not_found\",\"message\":\"Shop item not found\"}}"), EFlockErrorCode::ShopItemNotFound },
		{ 422, TEXT("{\"detail\":{\"code\":\"payment_provider.receipt_pending\",\"message\":\"Google reports this payment as still in progress; ask again later\"}}"), EFlockErrorCode::PaymentProviderReceiptPending },
		{ 403, TEXT("{\"detail\":{\"code\":\"payment_provider.receipt_belongs_to_another_player\",\"message\":\"Receipt refused: this store transaction was redeemed by another player\"}}"), EFlockErrorCode::PaymentProviderReceiptBelongsToAnotherPlayer },
		{ 422, TEXT("{\"detail\":{\"code\":\"payment_provider.receipt_product_mismatch\",\"message\":\"Receipt is for 'com.flock.gems.small', but this item sells 'com.flock.gems'\"}}"), EFlockErrorCode::PaymentProviderReceiptProductMismatch },
	};

	const TCHAR* const StoreUnavailable =
		TEXT("{\"detail\":{\"code\":\"payment_provider.store_unavailable\",\"message\":\"apple_app_store could not be asked about this receipt ([Errno 111] Connection refused); try again later\"}}");
	const TCHAR* const UnreadableKeys =
		TEXT("{\"detail\":{\"code\":\"payment_provider.credentials_unreadable\",\"message\":\"Apple credentials could not be used to sign a request\"}}");
	/** The backend's own message when the server has no address for a payment provider to confirm a payment (a checkout's). */
	const TCHAR* const NoWebhookAddress =
		TEXT("{\"detail\":{\"code\":\"shop.real_currency_settlement_unavailable\",\"message\":\"PAYMENT_WEBHOOK_BASE_URL is not configured\"}}");

	const TCHAR* const Route = TEXT("shop/validate-receipt");
	const TCHAR* const AppleTransactionId = TEXT("2000000999999999");

	/** Redeems and hands back what the call completed with; false when it never completed. */
	bool Redeem(FlockShopProviderTestHelpers::FFixture& Fx, const FFlockStoreReceipt& Receipt,
		TFlockResult<FFlockPurchaseResult>& OutResult, const FString& ShopItemId = TEXT("item-1"))
	{
		bool bDone = false;
		Fx.Provider->RedeemStoreReceipt(ShopItemId, Receipt, [&](TFlockResult<FFlockPurchaseResult> R)
		{
			bDone = true;
			OutResult = R;
		});
		FlockTestRetries::Pump();
		return bDone;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRedeemRequestShapeTest, "Flock.Shop.Redeem.EachStoresIdsAreSentAsTheSignedInPlayerWithNoPlayerId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockRedeemRequestShapeTest::RunTest(const FString& Parameters)
{
	using namespace FlockStoreRedemptionFixtures;
	{
		FFixture Fx;
		Fx.SignIn(TEXT("player-a"));
		Fx.Fake->On(Route, FFlockFakeTransport::Ok(FString(RedeemedPack) + TEXT("null}")));
		TFlockResult<FFlockPurchaseResult> Result;
		TestTrue(TEXT("App Store: the call completed"), Redeem(Fx, FFlockStoreReceipt::FromAppleAppStore(AppleTransactionId), Result));
		TestTrue(TEXT("App Store: redeemed"), Result.bSuccess);
		if (TestEqual(TEXT("App Store: one request"), Fx.Fake->CountTo(Route), 1))
		{
			const FFlockHttpRequest& Sent = Fx.Fake->Requests.Last();
			TestEqualSensitive(TEXT("App Store: a POST"), Sent.Method, FString(TEXT("POST")));
			TestEqualSensitive(TEXT("App Store: no player_id, the server redeems for the player the sign-in names"), Sent.JsonBody,
				FString(TEXT("{\"shop_item_id\":\"item-1\",\"provider\":\"apple_app_store\",\"receipt\":{\"transaction_id\":\"2000000999999999\"}}")));
			TestTrue(TEXT("App Store: sent with the player's sign-in"), Sent.Headers.Contains(TEXT("Authorization")));
		}
		Cleanup(Fx.Dir);
	}
	{
		FFixture Fx;
		Fx.SignIn(TEXT("player-a"));
		Fx.Fake->On(Route, FFlockFakeTransport::Ok(FString(RedeemedPack) + TEXT("null}")));
		TFlockResult<FFlockPurchaseResult> Result;
		TestTrue(TEXT("Google Play: the call completed"),
			Redeem(Fx, FFlockStoreReceipt::FromGooglePlay(TEXT("com.flock.gems"), TEXT("token-abc.AO-J1Oz")), Result));
		if (TestEqual(TEXT("Google Play: one request"), Fx.Fake->CountTo(Route), 1))
		{
			TestEqualSensitive(TEXT("Google Play: its product id and purchase token, exactly as given"), Fx.Fake->Requests.Last().JsonBody,
				FString(TEXT("{\"shop_item_id\":\"item-1\",\"provider\":\"google_play\",\"receipt\":{\"product_id\":\"com.flock.gems\",\"purchase_token\":\"token-abc.AO-J1Oz\"}}")));
		}
		Cleanup(Fx.Dir);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRedeemDeliveredTest, "Flock.Shop.Redeem.ADeliveredPurchaseReturnsWhatItGrantedAndWritesTheWallet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockRedeemDeliveredTest::RunTest(const FString& Parameters)
{
	using namespace FlockStoreRedemptionFixtures;
	FFixture Fx;
	Fx.SignIn(TEXT("player-a"));
	Fx.WirePlayers();
	if (!TestTrue(TEXT("the player cache is primed"), Fx.PrimePlayerCache(500)))
	{
		Cleanup(Fx.Dir);
		return true;
	}
	Fx.Fake->On(Route, FFlockFakeTransport::Ok(FString(RedeemedPack) + WalletJson(600) + TEXT("}")));

	TFlockResult<FFlockPurchaseResult> Result;
	TestTrue(TEXT("the call completed"), Redeem(Fx, FFlockStoreReceipt::FromAppleAppStore(AppleTransactionId), Result));
	TestTrue(TEXT("redeemed"), Result.bSuccess);
	TestEqualSensitive(TEXT("the purchase the server made"), Result.Value.PurchaseId, FString(TEXT("pur-9")));
	TestTrue(TEXT("a delivered store purchase reads paid"), Result.Value.IsPaid());
	if (TestEqual(TEXT("one grant"), Result.Value.Granted.Num(), 1))
	{
		TestEqual(TEXT("what it granted"), Result.Value.Granted[0].Amount, 100);
	}
	TestEqual(TEXT("the cached wallet reads the balance the redemption reported"), Fx.CachedGold(), 600);
	Cleanup(Fx.Dir);
	return true;
}

// The server answers a repeat with what the first delivered, so the SDK keeps no answer of its own to hand back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRedeemSentAgainTest, "Flock.Shop.Redeem.SentAgainItAsksTheServerAgain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRedeemSentAgainTest::RunTest(const FString& Parameters)
{
	using namespace FlockStoreRedemptionFixtures;
	FFixture Fx;
	Fx.SignIn(TEXT("player-a"));
	Fx.Fake->OnSequence(Route, {
		FFlockFakeTransport::Ok(FString(RedeemedPack) + TEXT("null}")),
		FFlockFakeTransport::Ok(TEXT("{\"purchase_id\":\"pur-9\",\"item_type\":\"currency_pack\",\"status\":\"refunded\",\"granted\":[{\"type\":\"currency\",\"code\":\"gold\",\"amount\":100}]}")) });
	const FFlockStoreReceipt Receipt = FFlockStoreReceipt::FromAppleAppStore(AppleTransactionId);

	TFlockResult<FFlockPurchaseResult> First;
	TFlockResult<FFlockPurchaseResult> Again;
	Redeem(Fx, Receipt, First);
	Redeem(Fx, Receipt, Again);

	TestEqual(TEXT("asked twice"), Fx.Fake->CountTo(Route), 2);
	TestTrue(TEXT("the repeat succeeded"), Again.bSuccess);
	TestEqualSensitive(TEXT("the repeat reads the purchase as it is now"), Again.Value.Status, FString(FlockPurchaseStatuses::Refunded));
	if (TestEqual(TEXT("the repeat carries a grant"), Again.Value.Granted.Num(), 1))
	{
		TestEqual(TEXT("a repeat carries what the first redemption granted"), Again.Value.Granted[0].Amount, 100);
	}
	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRedeemNotSignedInTest, "Flock.Shop.Redeem.WithNobodySignedInItFailsBeforeSending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockRedeemNotSignedInTest::RunTest(const FString& Parameters)
{
	using namespace FlockStoreRedemptionFixtures;
	FFixture Fx;
	Fx.Fake->On(Route, FFlockFakeTransport::Ok(FString(RedeemedPack) + TEXT("null}")));

	TFlockResult<FFlockPurchaseResult> Result;
	TestTrue(TEXT("the call completed"), Redeem(Fx, FFlockStoreReceipt::FromAppleAppStore(AppleTransactionId), Result));
	TestFalse(TEXT("refused"), Result.bSuccess);
	TestEqual(TEXT("as a sign-in failure"), static_cast<int32>(Result.Error.Type), static_cast<int32>(EFlockErrorType::Auth));
	TestEqualSensitive(TEXT("whose fix is to sign in first"), Result.Error.Hint, FFlockErrorHints::SignInFirst());
	TestEqual(TEXT("nothing sent"), Fx.Fake->CountTo(Route), 0);
	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRedeemIdsAsGivenTest, "Flock.Shop.Redeem.AReceiptMissingAnIdOrWithSpacesAroundOneIsRefusedNotTrimmed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockRedeemIdsAsGivenTest::RunTest(const FString& Parameters)
{
	using namespace FlockStoreRedemptionFixtures;
	// Every id filled in, so only the store's name can refuse these two.
	FFlockStoreReceipt AppleInGoogleCase = FFlockStoreReceipt::FromGooglePlay(TEXT("com.flock.gems"), TEXT("token-abc"));
	AppleInGoogleCase.TransactionId = AppleTransactionId;
	AppleInGoogleCase.Store = TEXT("Apple_App_Store");
	FFlockStoreReceipt WebGateway = AppleInGoogleCase;
	WebGateway.Store = FlockPaymentProviders::Stripe;

	struct FCase
	{
		const TCHAR* Name;
		FFlockStoreReceipt Receipt;
	};
	const FCase Cases[] = {
		// Missing an id, a default-constructed receipt standing for none at all.
		{ TEXT("no receipt"), FFlockStoreReceipt() },
		{ TEXT("an empty App Store transaction id"), FFlockStoreReceipt::FromAppleAppStore(TEXT("")) },
		{ TEXT("an empty Google Play product id"), FFlockStoreReceipt::FromGooglePlay(TEXT(""), TEXT("token-abc")) },
		{ TEXT("an empty Google Play purchase token"), FFlockStoreReceipt::FromGooglePlay(TEXT("com.flock.gems"), TEXT("")) },
		// Spaces around an id were put there where it was copied; trimming would hide that, so it is refused.
		{ TEXT("a transaction id with a space before it"), FFlockStoreReceipt::FromAppleAppStore(TEXT(" 2000000999999999")) },
		{ TEXT("a transaction id with a line end after it"), FFlockStoreReceipt::FromAppleAppStore(TEXT("2000000999999999\n")) },
		{ TEXT("a purchase token with a space after it"), FFlockStoreReceipt::FromGooglePlay(TEXT("com.flock.gems"), TEXT("token-abc ")) },
		// The same on every platform, whatever its C runtime calls white space.
		{ TEXT("a transaction id with a no-break space after it"), FFlockStoreReceipt::FromAppleAppStore(TEXT("2000000999999999 ")) },
		{ TEXT("a product id with an ideographic space before it"), FFlockStoreReceipt::FromGooglePlay(TEXT("　com.flock.gems"), TEXT("token-abc")) },
		// A receipt's members can be written by hand here: a store name this call does not redeem is refused, never read as Google.
		{ TEXT("a store name in another letter case"), AppleInGoogleCase },
		{ TEXT("a web gateway"), WebGateway },
	};

	for (const FCase& Case : Cases)
	{
		FFixture Fx;
		Fx.SignIn(TEXT("player-a"));
		Fx.Fake->On(Route, FFlockFakeTransport::Ok(FString(RedeemedPack) + TEXT("null}")));
		TFlockResult<FFlockPurchaseResult> Result;
		TestTrue(FString::Printf(TEXT("%s: the call completed"), Case.Name), Redeem(Fx, Case.Receipt, Result));
		TestFalse(FString::Printf(TEXT("%s: refused"), Case.Name), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("%s: as the caller's mistake"), Case.Name),
			static_cast<int32>(Result.Error.Type), static_cast<int32>(EFlockErrorType::Validation));
		TestFalse(FString::Printf(TEXT("%s: with its fix"), Case.Name), Result.Error.Hint.IsEmpty());
		TestEqual(FString::Printf(TEXT("%s: nothing sent"), Case.Name), Fx.Fake->CountTo(Route), 0);
		Cleanup(Fx.Dir);
	}

	// Control: the same fixture with each id as the store gave it is sent, so the refusals above are the ids' doing.
	FFixture Fx;
	Fx.SignIn(TEXT("player-a"));
	Fx.Fake->On(Route, FFlockFakeTransport::Ok(FString(RedeemedPack) + TEXT("null}")));
	TFlockResult<FFlockPurchaseResult> Result;
	Redeem(Fx, FFlockStoreReceipt::FromGooglePlay(TEXT("com.flock.gems"), TEXT("token-abc")), Result);
	TestTrue(TEXT("control: a well-formed receipt is redeemed"), Result.bSuccess);
	TestEqual(TEXT("control: and sent"), Fx.Fake->CountTo(Route), 1);
	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRedeemRefusalsTest, "Flock.Shop.Redeem.EachRefusalReadsAsItsOwnCodeAndIsNotSentAgain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockRedeemRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace FlockStoreRedemptionFixtures;
	TestEqual(TEXT("every refusal kept"), static_cast<int32>(UE_ARRAY_COUNT(ReceiptRefusals)), 8);
	for (const FReceiptRefusal& Refusal : ReceiptRefusals)
	{
		FFixture Fx(Retrying());
		Fx.SignIn(TEXT("player-a"));
		Fx.Fake->On(Route, FFlockFakeTransport::Status(Refusal.Status, Refusal.Body));
		const FString Name = FString(Refusal.Body).Mid(23, 60);

		TFlockResult<FFlockPurchaseResult> Result;
		TestTrue(FString::Printf(TEXT("%s: the call completed"), *Name), Redeem(Fx, FFlockStoreReceipt::FromAppleAppStore(AppleTransactionId), Result));
		TestFalse(FString::Printf(TEXT("%s: refused"), *Name), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("%s: on its own code"), *Name),
			static_cast<int32>(Result.Error.ErrorCode), static_cast<int32>(Refusal.Expected));
		TestFalse(FString::Printf(TEXT("%s: with its fix"), *Name), Result.Error.Hint.IsEmpty());
		TestEqual(FString::Printf(TEXT("%s: the server's final word on this receipt for now"), *Name), Fx.Fake->CountTo(Route), 1);
		Cleanup(Fx.Dir);
	}
	return true;
}

// Unlike a wallet purchase, a redemption may be sent again after an answer that never arrived (payment plan D-P3), the store
// being unavailable included; never after a 5xx naming one of the game's own settings, which stays the same until it is fixed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRedeemRetriesTest, "Flock.Shop.Redeem.SentAgainAfterAnAnswerInDoubtButNeverAfterAGameSetting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockRedeemRetriesTest::RunTest(const FString& Parameters)
{
	using namespace FlockStoreRedemptionFixtures;
	struct FCase
	{
		const TCHAR* Name;
		FFlockHttpResponse Response;
		int32 ExpectedSends;
		EFlockErrorCode ExpectedCode;
	};
	const FCase Cases[] = {
		{ TEXT("the store unavailable"), FFlockFakeTransport::Status(503, StoreUnavailable), 4, EFlockErrorCode::PaymentProviderStoreUnavailable },
		{ TEXT("a dropped connection"), FFlockFakeTransport::Offline(), 4, EFlockErrorCode::Unknown },
		{ TEXT("a timeout"), FFlockFakeTransport::Timeout(), 4, EFlockErrorCode::Unknown },
		{ TEXT("keys the server cannot read"), FFlockFakeTransport::Status(503, UnreadableKeys), 1, EFlockErrorCode::PaymentProviderCredentialsUnreadable },
		{ TEXT("no address to confirm a payment"), FFlockFakeTransport::Status(503, NoWebhookAddress), 1, EFlockErrorCode::ShopRealCurrencySettlementUnavailable },
	};
	for (const FCase& Case : Cases)
	{
		FFixture Fx(Retrying());
		Fx.SignIn(TEXT("player-a"));
		Fx.Fake->On(Route, Case.Response);
		TFlockResult<FFlockPurchaseResult> Result;
		TestTrue(FString::Printf(TEXT("%s: the call completed"), Case.Name), Redeem(Fx, FFlockStoreReceipt::FromAppleAppStore(AppleTransactionId), Result));
		TestFalse(FString::Printf(TEXT("%s: failed"), Case.Name), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("%s: sends (the first try and three retries, or the first alone)"), Case.Name),
			Fx.Fake->CountTo(Route), Case.ExpectedSends);
		if (Case.ExpectedCode != EFlockErrorCode::Unknown)
		{
			TestEqual(FString::Printf(TEXT("%s: on its own code"), Case.Name),
				static_cast<int32>(Result.Error.ErrorCode), static_cast<int32>(Case.ExpectedCode));
		}
		Cleanup(Fx.Dir);
	}

	// The rule lives in the retry handler, so it holds for a call that is not a redemption too.
	const FFlockError Unreadable = FFlockError::Make(EFlockErrorType::Network, TEXT("Service Unavailable"), 503, UnreadableKeys,
		TEXT("payment_provider.credentials_unreadable"));
	TestEqual(TEXT("the code is read off the body"), static_cast<int32>(Unreadable.ErrorCode),
		static_cast<int32>(EFlockErrorCode::PaymentProviderCredentialsUnreadable));
	TestFalse(TEXT("the handler never re-sends unreadable keys"), FFlockRetryHandler::ShouldRetry(Unreadable, /*bIdempotent*/ true));
	const FFlockError Busy = FFlockError::Make(EFlockErrorType::Network, TEXT("Service Unavailable"), 503, StoreUnavailable,
		TEXT("payment_provider.store_unavailable"));
	TestTrue(TEXT("control: the handler re-sends a coded 503 that names no setting"), FFlockRetryHandler::ShouldRetry(Busy, /*bIdempotent*/ true));
	return true;
}

// A retry goes out as the player who redeemed or not at all: another player's sign-in never redeems the first player's receipt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRedeemKeepsItsSignInTest, "Flock.Shop.Redeem.ARetryIsNeverSentAsTheNextPlayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRedeemKeepsItsSignInTest::RunTest(const FString& Parameters)
{
	using namespace FlockStoreRedemptionFixtures;
	auto RunRedeem = [this](int32 Change, int32& OutSends, EFlockErrorType& OutType)
	{
		FFixture Fx(Retrying());
		Fx.SignIn(TEXT("player-a"));
		Fx.Fake->On(Route, FFlockFakeTransport::Status(503, StoreUnavailable));
		bool bDone = false;
		Fx.Provider->RedeemStoreReceipt(TEXT("item-1"), FFlockStoreReceipt::FromAppleAppStore(AppleTransactionId),
			[&](TFlockResult<FFlockPurchaseResult> R)
			{
				bDone = true;
				OutType = R.bSuccess ? EFlockErrorType::None : R.Error.Type;
			});
		// The first try has been answered; its retry waits on the ticker.
		TestFalse(TEXT("the retry waits on the ticker"), bDone);
		if (Change == 1)
		{
			Fx.SignIn(TEXT("player-b"));
		}
		else if (Change == 2)
		{
			Fx.Session->ClearTokens();
		}
		FlockTestRetries::Pump();
		TestTrue(TEXT("the call completed"), bDone);
		OutSends = Fx.Fake->CountTo(Route);
		Cleanup(Fx.Dir);
	};

	int32 Sends = -1;
	EFlockErrorType Type = EFlockErrorType::None;
	RunRedeem(0, Sends, Type);
	TestEqual(TEXT("control: the same player all along gets every retry"), Sends, 4);

	RunRedeem(1, Sends, Type);
	TestEqual(TEXT("after the switch: the retry would have gone out as player-b"), Sends, 1);
	TestEqual(TEXT("after the switch: cancelled"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Cancelled));

	RunRedeem(2, Sends, Type);
	TestEqual(TEXT("after a sign-out: no retry goes out"), Sends, 1);
	TestEqual(TEXT("after a sign-out: cancelled"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Cancelled));
	return true;
}

// A 2xx that names no purchase says nothing about one; the purchase result's default status would read it as paid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRedeemNamesNoPurchaseTest, "Flock.Shop.Redeem.AnAnswerNamingNoPurchaseFailsTheCall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockRedeemNamesNoPurchaseTest::RunTest(const FString& Parameters)
{
	using namespace FlockStoreRedemptionFixtures;
	const TCHAR* const Bodies[] = {
		TEXT("{}"),
		TEXT("{\"item_type\":\"currency_pack\",\"granted\":[]}"),
		// A 2xx with nothing in it, or a page that is not Flock's (a captive portal), says no more.
		TEXT(""),
		TEXT("<html><body>Sign in to the network</body></html>"),
		// It carries a wallet, which must not reach the cache: the answer is not about a purchase.
		TEXT("{\"item_type\":\"currency_pack\",\"granted\":[],\"wallet\":{\"id\":\"pd-1\",\"player_template_id\":\"tmpl-1\",\"game_id\":\"g\",\"player_id\":\"player-a\",\"data\":[{\"type\":\"int\",\"field_name\":\"gold\",\"value\":999}],\"created_at\":\"\",\"updated_at\":\"\"}}"),
	};
	for (const TCHAR* Body : Bodies)
	{
		FFixture Fx(Retrying());
		Fx.SignIn(TEXT("player-a"));
		Fx.WirePlayers();
		if (!TestTrue(TEXT("the player cache is primed"), Fx.PrimePlayerCache(500)))
		{
			Cleanup(Fx.Dir);
			continue;
		}
		Fx.Fake->On(Route, FFlockFakeTransport::Ok(Body));
		TFlockResult<FFlockPurchaseResult> Result;
		TestTrue(FString::Printf(TEXT("%s: the call completed"), Body), Redeem(Fx, FFlockStoreReceipt::FromAppleAppStore(AppleTransactionId), Result));
		TestFalse(FString::Printf(TEXT("%s: failed"), Body), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("%s: as an answer that could not be read"), Body),
			static_cast<int32>(Result.Error.Type), static_cast<int32>(EFlockErrorType::Serialization));
		TestTrue(FString::Printf(TEXT("%s: whose fix is to send the same receipt again"), Body),
			Result.Error.Hint.Contains(TEXT("send the same receipt again"), ESearchCase::CaseSensitive));
		TestEqual(FString::Printf(TEXT("%s: not sent again by itself"), Body), Fx.Fake->CountTo(Route), 1);
		TestFalse(FString::Printf(TEXT("%s: its wallet never reaches the cache"), Body), Fx.CachedGold() == 999);
		TestEqual(FString::Printf(TEXT("%s: the cached player data is dropped, so a wallet read asks the server"), Body), Fx.CachedGold(), -1);
		TestEqualSensitive(FString::Printf(TEXT("%s: the failure names the call"), Body), Result.Error.Operation, FString(TEXT("Redeem store receipt")));
		Cleanup(Fx.Dir);
	}
	return true;
}

// The same check guards the wallet purchase, where a 2xx naming no purchase may still have moved money: nothing is recorded as
// its outcome, and the wallet the fix asks the game to read comes from the server.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPurchaseNamesNoPurchaseTest, "Flock.Shop.Provider.APurchaseAnswerNamingNoPurchaseFailsTheCall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPurchaseNamesNoPurchaseTest::RunTest(const FString& Parameters)
{
	auto RunPurchase = [this](const FString& Answer, bool& bOutSucceeded, FString& OutHint, int32& OutRecords, bool& bOutCacheKept)
	{
		FFixture Fx;
		Fx.SignIn(TEXT("player-a"));
		Fx.WireAnalytics();
		Fx.WirePlayers();
		TestTrue(TEXT("the player cache is primed"), Fx.PrimePlayerCache(500));
		Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));
		Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Ok(Answer));
		Fx.Fake->On(TEXT("analytics/transactions"), FFlockFakeTransport::Ok(TEXT("{}")));
		bool bDone = false;
		Fx.Provider->Purchase(TEXT("item-1"), [&](TFlockResult<FFlockPurchaseResult> R)
		{
			bDone = true;
			bOutSucceeded = R.bSuccess;
			OutHint = R.Error.Hint;
		});
		TestTrue(TEXT("the purchase answered"), bDone);
		OutRecords = Fx.Fake->CountTo(TEXT("analytics/transactions"));
		FFlockPlayerData Row;
		bOutCacheKept = Fx.Players->TryGetCachedRow(TEXT("pd-1"), Row);
		Cleanup(Fx.Dir);
	};

	bool bSucceeded = false;
	FString Hint;
	int32 Records = -1;
	bool bCacheKept = false;

	// Control: an answer naming its purchase succeeds, records Started and Purchased, and keeps the cache.
	RunPurchase(PurchaseResultBodyWithWalletGold(600), bSucceeded, Hint, Records, bCacheKept);
	TestTrue(TEXT("control: succeeded"), bSucceeded);
	TestEqual(TEXT("control: Started and Purchased recorded"), Records, 2);
	TestTrue(TEXT("control: the cache is kept"), bCacheKept);

	for (const TCHAR* Answer : { TEXT("{}"), TEXT("") })
	{
		RunPurchase(Answer, bSucceeded, Hint, Records, bCacheKept);
		TestFalse(FString::Printf(TEXT("'%s': an answer naming no purchase fails"), Answer), bSucceeded);
		TestTrue(FString::Printf(TEXT("'%s': whose fix is to read the inventory and wallet"), Answer),
			Hint.Contains(TEXT("inventory and wallet"), ESearchCase::CaseSensitive));
		TestEqual(FString::Printf(TEXT("'%s': only Started is recorded: neither Purchased nor Failed is known"), Answer), Records, 1);
		TestFalse(FString::Printf(TEXT("'%s': the cached player data is dropped, so reading it asks the server"), Answer), bCacheKept);
	}
	return true;
}

// An expired token is renewed once and the redemption sent again with the new one, read on that attempt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRedeemRenewsAnExpiredTokenTest, "Flock.Shop.Redeem.AnExpiredTokenIsRenewedAndTheRedemptionSentAgain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRedeemRenewsAnExpiredTokenTest::RunTest(const FString& Parameters)
{
	using namespace FlockStoreRedemptionFixtures;
	FFixture Fx;
	Fx.SignIn(TEXT("player-a"));
	const FString Renewed = MakeTestJwt(TEXT("player-a")) + TEXT("x");
	Fx.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Ok(FString::Printf(
		TEXT("{\"player_id\":\"player-a\",\"access_token\":\"%s\",\"refresh_token\":\"r-2\"}"), *Renewed)));
	Fx.Fake->OnSequence(Route, {
		FFlockFakeTransport::Status(401, TEXT("{\"detail\":{\"code\":\"player.expired_token\",\"message\":\"Expired token.\"}}")),
		FFlockFakeTransport::Ok(FString(RedeemedPack) + TEXT("null}")) });

	TFlockResult<FFlockPurchaseResult> Result;
	TestTrue(TEXT("the call completed"), Redeem(Fx, FFlockStoreReceipt::FromAppleAppStore(AppleTransactionId), Result));
	TestTrue(TEXT("redeemed once the token was renewed"), Result.bSuccess);
	TestEqual(TEXT("the token was renewed once"), Fx.Fake->CountTo(TEXT("token/refresh")), 1);
	if (TestEqual(TEXT("sent twice: the expired try and the one after renewing"), Fx.Fake->CountTo(Route), 2))
	{
		const FFlockHttpRequest& Last = Fx.Fake->Requests.Last();
		const FString* Authorization = Last.Headers.Find(TEXT("Authorization"));
		TestTrue(TEXT("the second carries the renewed token"),
			Authorization != nullptr && Authorization->EndsWith(Renewed, ESearchCase::CaseSensitive));
	}
	Cleanup(Fx.Dir);
	return true;
}

// A purchase the engine cancelled while it was out (the HTTP module shutting down) may have gone out: neither Purchased nor
// Failed is known, so only Started is recorded. A refusal is a known failure and records Failed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPurchaseCancelledRecordsNoOutcomeTest, "Flock.Shop.Provider.APurchaseCancelledWhileOutRecordsNoOutcome",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPurchaseCancelledRecordsNoOutcomeTest::RunTest(const FString& Parameters)
{
	for (const bool bCancelled : { true, false })
	{
		FFixture Fx;
		Fx.SignIn(TEXT("player-a"));
		Fx.WireAnalytics();
		Fx.Fake->On(TEXT("shop_item/item-1"), FFlockFakeTransport::Ok(ItemBody(TEXT("item-1"), 100)));
		FFlockHttpResponse Aborted;
		Aborted.Result = EFlockHttpResult::Cancelled;
		Fx.Fake->On(TEXT("shop/transaction"), bCancelled ? Aborted : FFlockFakeTransport::Coded(400, TEXT("shop.insufficient_funds")));
		Fx.Fake->On(TEXT("analytics/transactions"), FFlockFakeTransport::Ok(TEXT("{}")));
		EFlockErrorType Failure = EFlockErrorType::None;
		Fx.Provider->Purchase(TEXT("item-1"), [&Failure](TFlockResult<FFlockPurchaseResult> R) { Failure = R.Error.Type; });
		if (bCancelled)
		{
			TestEqual(TEXT("cancelled while out: a cancel"), Failure, EFlockErrorType::Cancelled);
			TestEqual(TEXT("cancelled while out: only Started is recorded"), Fx.Fake->CountTo(TEXT("analytics/transactions")), 1);
		}
		else
		{
			TestEqual(TEXT("control, refused: Started and Failed are recorded"), Fx.Fake->CountTo(TEXT("analytics/transactions")), 2);
		}
		Cleanup(Fx.Dir);
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
