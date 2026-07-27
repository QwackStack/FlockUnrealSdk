// Copyright 2022, Qwacks. All Rights Reserved.

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
#include "HAL/FileManager.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockSnapshotStore.h"
#include "Misc/Base64.h"
#include "Misc/Paths.h"
#include "Providers/FlockAnalyticsProvider.h"
#include "Providers/FlockShopProvider.h"
#include "Tests/Support/FlockFakeTransport.h"
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

	// ── Real wire shapes (bare/paginated/enveloped-list), NOT the enveloped fixtures the Unity tests
	// use against bare routes — those hide a shape mismatch and only assert non-null. ──

	inline FString ItemBody(const FString& Id, int32 Price)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"name\":\"Sword\",\"status\":\"active\",\"shop_id\":\"shop-1\",\"patch_id\":null,")
			TEXT("\"price\":%d,\"currency\":\"GOLD\",\"data\":{\"rarity\":\"epic\"},\"created_at\":\"\",\"updated_at\":\"\"}"),
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

	inline FString InventoryBody(const FString& Id)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"player_id\":\"player-a\",\"shop_item_id\":\"item-1\",\"status\":\"owned\",")
			TEXT("\"created_at\":\"\",\"used_at\":null}"),
			*Id);
	}

	inline FString ShopPageBody()
	{
		return FString::Printf(TEXT("{\"items\":[%s],\"total\":1,\"page\":1,\"limit\":100}"), *ShopBody(TEXT("shop-1")));
	}

	inline FString InventoryPageBody()
	{
		return FString::Printf(TEXT("{\"items\":[%s],\"total\":1,\"page\":1,\"limit\":100}"), *InventoryBody(TEXT("inv-1")));
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
	TestTrue(TEXT("free-form stats kept verbatim"), Shop.Data.StatsJson.Contains(TEXT("visits")));
	if (TestEqual(TEXT("one nested item"), Shop.ShopItems.Num(), 1))
	{
		TestEqual(TEXT("nested item id"), Shop.ShopItems[0].Id, FString(TEXT("item-1")));
		TestEqual(TEXT("nested item price"), Shop.ShopItems[0].Price, 100);
		TestTrue(TEXT("nested item free-form data kept verbatim"), Shop.ShopItems[0].DataJson.Contains(TEXT("rarity")));
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
		TestTrue(TEXT("item free-form data kept verbatim"), Items[0].DataJson.Contains(TEXT("rarity")));
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
		Fx.Provider->Purchase(FString(), FString(), [&](TFlockResult<FFlockPlayerInventory> R) { bFailed = !R.bSuccess && R.Error.Type == EFlockErrorType::Validation; });
		TestTrue(TEXT("empty item id -> validation"), bFailed);
		TestEqual(TEXT("no request"), Fx.Fake->Requests.Num(), 0);
		Cleanup(Fx.Dir);
	}
	{
		// Item id present, but signed out -> validation (player-scoped route).
		FFixture Fx;
		bool bFailed = false;
		Fx.Provider->Purchase(TEXT("item-1"), FString(), [&](TFlockResult<FFlockPlayerInventory> R) { bFailed = !R.bSuccess && R.Error.Type == EFlockErrorType::Validation; });
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
	Fx.Provider->Purchase(TEXT("item-1"), FString(), [&](TFlockResult<FFlockPlayerInventory> R) { bFailed = !R.bSuccess; });
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
	Fx.Fake->On(TEXT("shop/transaction"), FFlockFakeTransport::Ok(InventoryBody(TEXT("inv-1"))));
	Fx.Fake->On(TEXT("analytics/transactions"), FFlockFakeTransport::Ok(TEXT("{}")));

	FFlockPlayerInventory Inventory;
	bool bDone = false;
	Fx.Provider->Purchase(TEXT("item-1"), FString(), [&](TFlockResult<FFlockPlayerInventory> R) { bDone = R.bSuccess; Inventory = R.Value; });
	TestTrue(TEXT("purchase succeeds"), bDone);
	TestEqual(TEXT("inventory id returned"), Inventory.Id, FString(TEXT("inv-1")));
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

#endif // WITH_AUTOMATION_TESTS
