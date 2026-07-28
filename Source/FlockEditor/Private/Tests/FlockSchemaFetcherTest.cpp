// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Codegen/FlockSchemaFetcher.h"
#include "FlockLogger.h"
#include "Http/FlockHttpClient.h"
#include "Tests/Support/FlockFakeTransport.h"

namespace FlockSchemaFetcherTestHelpers
{
	const TCHAR* const ApiUrl = TEXT("http://x");
	const TCHAR* const VersionName = TEXT("1.0.0");

	inline FString Enveloped(const FString& ResultJson)
	{
		return FString::Printf(TEXT("{\"error\":null,\"response\":null,\"result\":%s}"), *ResultJson);
	}

	inline FString VersionBody(const FString& Id)
	{
		return Enveloped(FString::Printf(TEXT("{\"id\":\"%s\",\"name\":\"1.0.0\",\"game_id\":\"g\"}"), *Id));
	}

	inline FString TemplateObj(const FString& Id, const FString& Name, const FString& Tag)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"name\":\"%s\",\"game_version_id\":\"ver-1\",\"tag\":\"%s\",")
			TEXT("\"schema\":[{\"type\":\"int\",\"field_name\":\"coins\",\"type_name\":\"int\"}],\"data\":[]}"),
			*Id, *Name, *Tag);
	}

	inline FString ConfigObj(const FString& Id, const FString& Name)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"name\":\"%s\",\"game_version_id\":\"ver-1\",\"tag\":\"gameplay\",")
			TEXT("\"schema\":[{\"type\":\"string\",\"field_name\":\"mode\",\"type_name\":\"str\"}],\"data\":[]}"),
			*Id, *Name);
	}

	inline FString ShopObj(const FString& Id, const FString& Name, bool bWithItems)
	{
		const FString Items = bWithItems
			? TEXT("[{\"id\":\"item-embedded\",\"name\":\"Embedded\",\"shop_id\":\"shop-2\",\"price\":5,\"currency\":\"Gold\"}]")
			: TEXT("[]");
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"name\":\"%s\",\"status\":\"active\",\"game_id\":\"g\",")
			TEXT("\"game_version_id\":\"ver-1\",\"shop_items\":%s}"), *Id, *Name, *Items);
	}

	inline FString ShopPage(const FString& ItemsJson, int32 Total, int32 Page, int32 Limit)
	{
		return FString::Printf(TEXT("{\"items\":[%s],\"total\":%d,\"page\":%d,\"limit\":%d}"),
			*ItemsJson, Total, Page, Limit);
	}

	struct FFixture
	{
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockHttpClient> Client;

		FFixture()
			: Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
		{
		}

		/** Routes every step to a healthy default; a test overrides just the one it is about. */
		void RouteHappyPath()
		{
			// Registered most-specific first: the fake matches by URL fragment in insertion order, and
			// "shop" would otherwise swallow "shop_item/shop/...".
			Fake->On(TEXT("game_version/by-name"), FFlockFakeTransport::Ok(VersionBody(TEXT("ver-1"))));
			Fake->On(TEXT("player_template"), FFlockFakeTransport::Ok(
				Enveloped(FString::Printf(TEXT("[%s]"), *TemplateObj(TEXT("tmpl-1"), TEXT("Wallet"), TEXT("currency"))))));
			Fake->On(TEXT("game_config/version"), FFlockFakeTransport::Ok(
				Enveloped(FString::Printf(TEXT("[%s]"), *ConfigObj(TEXT("cfg-1"), TEXT("Gameplay"))))));
			Fake->On(TEXT("shop_item/shop/shop-1"), FFlockFakeTransport::Ok(
				Enveloped(TEXT("[{\"id\":\"item-1\",\"name\":\"GemPack\",\"shop_id\":\"shop-1\",\"price\":100,\"currency\":\"Gold\"}]"))));
			Fake->On(TEXT("shop?"), FFlockFakeTransport::Ok(
				ShopPage(ShopObj(TEXT("shop-1"), TEXT("Starter"), /*bWithItems*/ false), 1, 1, 100)));
		}

		TFlockResult<FFlockSchemaSnapshot> Run()
		{
			TFlockResult<FFlockSchemaSnapshot> Captured = TFlockResult<FFlockSchemaSnapshot>::Fail(
				FFlockError::Make(EFlockErrorType::None, TEXT("never completed")));
			FFlockSchemaFetcher::Fetch(Client, ApiUrl, TEXT("key"), VersionName, TEXT("ver-1"),
				FFlockSchemaFetcher::FOnSchemaFetched::CreateLambda(
					[&Captured](TFlockResult<FFlockSchemaSnapshot> Result) { Captured = Result; }));
			return Captured;
		}
	};
}

using namespace FlockSchemaFetcherTestHelpers;

// ── The happy path assembles every part of the snapshot ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockFetcherHappyPathTest, "Flock.Editor.SchemaFetcher.AssemblesFullSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockFetcherHappyPathTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.RouteHappyPath();

	const TFlockResult<FFlockSchemaSnapshot> Result = Fx.Run();
	if (!TestTrue(TEXT("fetch succeeds"), Result.bSuccess))
	{
		AddError(Result.Error.Message);
		return false;
	}

	TestEqual(TEXT("version resolved by name"), Result.Value.GameVersionId, FString(TEXT("ver-1")));
	TestEqual(TEXT("one template"), Result.Value.PlayerTemplates.Num(), 1);
	TestEqual(TEXT("one config"), Result.Value.GameConfigs.Num(), 1);
	TestEqual(TEXT("one shop"), Result.Value.Shops.Num(), 1);

	// The schema is what codegen actually emits from, so its verbatim survival is the point of the fetch.
	if (Result.Value.PlayerTemplates.Num() == 1)
	{
		TestTrue(TEXT("template schema kept verbatim"),
			Result.Value.PlayerTemplates[0].SchemaJson.Contains(TEXT("\"field_name\":\"coins\"")));
	}
	if (Result.Value.GameConfigs.Num() == 1)
	{
		TestTrue(TEXT("config schema kept verbatim"),
			Result.Value.GameConfigs[0].SchemaJson.Contains(TEXT("\"field_name\":\"mode\"")));
	}
	// Items arrive by backfill, because the shop list came back without them.
	if (Result.Value.Shops.Num() == 1)
	{
		TestEqual(TEXT("shop items backfilled"), Result.Value.Shops[0].ShopItems.Num(), 1);
	}
	TestFalse(TEXT("bake is current"), Result.Value.IsBakeStale());

	return true;
}

// ── The version id scopes every request after the resolve ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockFetcherHeaderTest, "Flock.Editor.SchemaFetcher.ScopesRequestsToResolvedVersion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockFetcherHeaderTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.RouteHappyPath();
	Fx.Run();

	int32 Scoped = 0;
	for (const FFlockHttpRequest& Request : Fx.Fake->Requests)
	{
		TestTrue(TEXT("every request carries the api key"), Request.Headers.Contains(TEXT("X-Flock-API-Key")));
		if (const FString* Version = Request.Headers.Find(TEXT("X-Game-Version-ID")))
		{
			TestEqual(TEXT("scoped to the resolved id"), *Version, FString(TEXT("ver-1")));
			++Scoped;
		}
	}
	// The resolve itself cannot carry the id — it is what produces it — so every other request should.
	TestEqual(TEXT("all but the resolve are version-scoped"), Scoped, Fx.Fake->Requests.Num() - 1);

	return true;
}

// ── A shop that already embeds its items is not re-fetched ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockFetcherEmbeddedItemsTest, "Flock.Editor.SchemaFetcher.SkipsBackfillWhenItemsEmbedded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockFetcherEmbeddedItemsTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.RouteHappyPath();
	Fx.Fake->On(TEXT("shop?"), FFlockFakeTransport::Ok(
		ShopPage(ShopObj(TEXT("shop-2"), TEXT("Embedded"), /*bWithItems*/ true), 1, 1, 100)));

	const TFlockResult<FFlockSchemaSnapshot> Result = Fx.Run();
	TestTrue(TEXT("fetch succeeds"), Result.bSuccess);
	TestEqual(TEXT("embedded items kept"), Result.Value.Shops.Num() == 1 ? Result.Value.Shops[0].ShopItems.Num() : -1, 1);
	TestEqual(TEXT("no per-shop item request"), Fx.Fake->CountTo(TEXT("shop_item/shop/")), 0);

	return true;
}

// ── Shops paginate until a short page ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockFetcherPagingTest, "Flock.Editor.SchemaFetcher.PagesShopsToCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockFetcherPagingTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.RouteHappyPath();

	// A full page (100) forces a second request; the second page is short, which ends the walk.
	FString FullPage;
	for (int32 Index = 0; Index < 100; ++Index)
	{
		if (Index > 0)
		{
			FullPage += TEXT(",");
		}
		FullPage += ShopObj(FString::Printf(TEXT("shop-p%d"), Index), FString::Printf(TEXT("Shop%d"), Index), true);
	}
	Fx.Fake->OnSequence(TEXT("shop?"), {
		FFlockFakeTransport::Ok(ShopPage(FullPage, 101, 1, 100)),
		FFlockFakeTransport::Ok(ShopPage(ShopObj(TEXT("shop-last"), TEXT("Last"), true), 101, 2, 100)),
	});

	const TFlockResult<FFlockSchemaSnapshot> Result = Fx.Run();
	TestTrue(TEXT("fetch succeeds"), Result.bSuccess);
	TestEqual(TEXT("both pages collected"), Result.Value.Shops.Num(), 101);
	TestEqual(TEXT("stopped after the short page"), Fx.Fake->CountTo(TEXT("shop?")), 2);

	return true;
}

// ── Any step failing aborts the whole run: a partial snapshot must never reach an emitter ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockFetcherAbortTest, "Flock.Editor.SchemaFetcher.AnyFailureAbortsWholeFetch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockFetcherAbortTest::RunTest(const FString& Parameters)
{
	// Configs fail: templates already succeeded, so this is exactly the partial-snapshot case.
	{
		FFixture Fx;
		Fx.RouteHappyPath();
		Fx.Fake->On(TEXT("game_config/version"), FFlockFakeTransport::Status(500, TEXT("{}")));

		const TFlockResult<FFlockSchemaSnapshot> Result = Fx.Run();
		TestFalse(TEXT("fetch fails"), Result.bSuccess);
		TestTrue(TEXT("error names the step"), Result.Error.Message.Contains(TEXT("Fetch game configs")));
		// Nothing downstream ran, so no emitter could have been handed a half-snapshot.
		TestEqual(TEXT("shops never fetched"), Fx.Fake->CountTo(TEXT("shop?")), 0);
	}

	// The resolve failing stops everything before a single scoped request goes out.
	{
		FFixture Fx;
		Fx.RouteHappyPath();
		Fx.Fake->On(TEXT("game_version/by-name"), FFlockFakeTransport::Offline());

		const TFlockResult<FFlockSchemaSnapshot> Result = Fx.Run();
		TestFalse(TEXT("fetch fails"), Result.bSuccess);
		TestTrue(TEXT("error names the step"), Result.Error.Message.Contains(TEXT("Resolve game version")));
		TestEqual(TEXT("only the resolve was attempted"), Fx.Fake->Requests.Num(), 1);
	}

	// A shop's items failing aborts too, rather than yielding a shop with no items.
	{
		FFixture Fx;
		Fx.RouteHappyPath();
		Fx.Fake->On(TEXT("shop_item/shop/shop-1"), FFlockFakeTransport::Status(503, TEXT("{}")));

		const TFlockResult<FFlockSchemaSnapshot> Result = Fx.Run();
		TestFalse(TEXT("fetch fails"), Result.bSuccess);
		TestTrue(TEXT("error names the shop"), Result.Error.Message.Contains(TEXT("shop-1")));
	}

	return true;
}

// ── A 2xx with no id is not a usable resolve ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockFetcherEmptyVersionTest, "Flock.Editor.SchemaFetcher.EmptyVersionIdIsAFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockFetcherEmptyVersionTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.RouteHappyPath();
	Fx.Fake->On(TEXT("game_version/by-name"), FFlockFakeTransport::Ok(Enveloped(TEXT("{\"id\":\"\",\"name\":\"1.0.0\"}"))));

	const TFlockResult<FFlockSchemaSnapshot> Result = Fx.Run();
	TestFalse(TEXT("fetch fails"), Result.bSuccess);
	TestEqual(TEXT("classified as validation"), Result.Error.Type, EFlockErrorType::Validation);
	TestEqual(TEXT("nothing else was attempted"), Fx.Fake->Requests.Num(), 1);

	return true;
}

// ── A stale bake is reported, not silently generated against ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockFetcherStaleBakeTest, "Flock.Editor.SchemaFetcher.ReportsStaleBakedVersion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockFetcherStaleBakeTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.RouteHappyPath();
	// The backend cut a new version under the same name; the project still has the old id baked.
	Fx.Fake->On(TEXT("game_version/by-name"), FFlockFakeTransport::Ok(VersionBody(TEXT("ver-2"))));

	TFlockResult<FFlockSchemaSnapshot> Captured = TFlockResult<FFlockSchemaSnapshot>::Fail(FFlockError());
	FFlockSchemaFetcher::Fetch(Fx.Client, ApiUrl, TEXT("key"), VersionName, TEXT("ver-1"),
		FFlockSchemaFetcher::FOnSchemaFetched::CreateLambda(
			[&Captured](TFlockResult<FFlockSchemaSnapshot> Result) { Captured = Result; }));

	TestTrue(TEXT("fetch still succeeds"), Captured.bSuccess);
	TestTrue(TEXT("staleness detected"), Captured.Value.IsBakeStale());
	TestEqual(TEXT("resolved id wins for the fetch"), Captured.Value.GameVersionId, FString(TEXT("ver-2")));
	TestEqual(TEXT("baked id preserved for reporting"), Captured.Value.BakedGameVersionId, FString(TEXT("ver-1")));

	// A matching bake is not stale, and neither is an unbaked project.
	TestFalse(TEXT("matching bake"), FFlockSchemaSnapshot{ TEXT("ver-1"), TEXT("ver-1") }.IsBakeStale());
	TestFalse(TEXT("no bake yet"), FFlockSchemaSnapshot{ TEXT("ver-1"), TEXT("") }.IsBakeStale());

	return true;
}

// ── An empty backend is a valid answer, not a failure ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockFetcherEmptyGameTest, "Flock.Editor.SchemaFetcher.EmptyGameIsASuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockFetcherEmptyGameTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("game_version/by-name"), FFlockFakeTransport::Ok(VersionBody(TEXT("ver-1"))));
	Fx.Fake->On(TEXT("player_template"), FFlockFakeTransport::Ok(Enveloped(TEXT("[]"))));
	Fx.Fake->On(TEXT("game_config/version"), FFlockFakeTransport::Ok(Enveloped(TEXT("[]"))));
	Fx.Fake->On(TEXT("shop?"), FFlockFakeTransport::Ok(ShopPage(TEXT(""), 0, 1, 100)));

	const TFlockResult<FFlockSchemaSnapshot> Result = Fx.Run();
	TestTrue(TEXT("fetch succeeds"), Result.bSuccess);
	TestEqual(TEXT("no templates"), Result.Value.PlayerTemplates.Num(), 0);
	TestEqual(TEXT("no configs"), Result.Value.GameConfigs.Num(), 0);
	TestEqual(TEXT("no shops"), Result.Value.Shops.Num(), 0);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
