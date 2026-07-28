// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Codegen/FlockCatalogBuilder.h"
#include "Codegen/FlockContentCatalog.h"
#include "Codegen/FlockSchemaSnapshot.h"

namespace FlockCatalogBuilderTestHelpers
{
	inline FFlockPlayerTemplateSchema Template(const FString& Id, const FString& Name, const FString& Tag,
		const FString& SchemaJson)
	{
		FFlockPlayerTemplateSchema Result;
		Result.Id = Id;
		Result.Name = Name;
		Result.Tag = Tag;
		Result.SchemaJson = SchemaJson;
		return Result;
	}

	inline FFlockShopItem Item(const FString& Id, const FString& Name, const FString& Currency, int32 Price)
	{
		FFlockShopItem Result;
		Result.Id = Id;
		Result.Name = Name;
		Result.Currency = Currency;
		Result.Price = Price;
		return Result;
	}

	/** Deliberately unsorted, so the builder's stable ordering is actually exercised. */
	inline FFlockSchemaSnapshot Snapshot()
	{
		FFlockSchemaSnapshot Result;
		Result.GameVersionId = TEXT("ver-1");
		Result.FetchedAtUtc = FDateTime(2026, 7, 28, 12, 0, 0);

		Result.PlayerTemplates.Add(Template(TEXT("tmpl-2"), TEXT("Trophies"), TEXT("achievement"),
			TEXT("[{\"type\":\"bool\",\"field_name\":\"first_win\",\"type_name\":\"bool\"},")
			TEXT("{\"type\":\"bool\",\"field_name\":\"flawless\",\"type_name\":\"bool\"}]")));
		Result.PlayerTemplates.Add(Template(TEXT("tmpl-1"), TEXT("Wallet"), TEXT("currency"),
			TEXT("[{\"type\":\"dict\",\"field_name\":\"game_currencies\",\"type_name\":\"dict\"}]")));

		FFlockGameConfigSchema Config;
		Config.Id = TEXT("cfg-1");
		Config.Name = TEXT("Gameplay");
		Config.Tag = TEXT("gameplay");
		Config.SchemaJson = TEXT("[{\"type\":\"float\",\"field_name\":\"move_speed\",\"type_name\":\"float\"}]");
		Result.GameConfigs.Add(Config);

		FFlockShop Shop;
		Shop.Id = TEXT("shop-1");
		Shop.Name = TEXT("Starter");
		Shop.ShopItems.Add(Item(TEXT("item-2"), TEXT("Shield"), TEXT("Shard"), 50));
		Shop.ShopItems.Add(Item(TEXT("item-1"), TEXT("GemPack"), TEXT("Gold"), 100));
		Result.Shops.Add(Shop);
		return Result;
	}

	inline UFlockContentCatalog* Build(const FFlockSchemaSnapshot& Snapshot)
	{
		UFlockContentCatalog* Catalog = NewObject<UFlockContentCatalog>();
		FFlockCatalogBuilder::Populate(Snapshot, *Catalog);
		return Catalog;
	}
}

using namespace FlockCatalogBuilderTestHelpers;

// ── Everything the snapshot declares lands in the catalog ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCatalogPopulateTest, "Flock.Editor.Catalog.PopulatesFromSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCatalogPopulateTest::RunTest(const FString& Parameters)
{
	UFlockContentCatalog* Catalog = Build(Snapshot());

	TestEqual(TEXT("version stamped"), Catalog->GameVersionId, FString(TEXT("ver-1")));
	TestFalse(TEXT("hash stamped"), Catalog->ContentHash.IsEmpty());
	TestFalse(TEXT("time stamped"), Catalog->GeneratedAtUtc.IsEmpty());

	TestEqual(TEXT("two templates"), Catalog->PlayerTemplates.Num(), 2);
	TestEqual(TEXT("one config"), Catalog->GameConfigs.Num(), 1);
	TestEqual(TEXT("one shop"), Catalog->Shops.Num(), 1);
	TestFalse(TEXT("not empty"), Catalog->IsEmptyCatalog());

	// Declared field names, not the flattened ones a read hands back — this is the writable spelling.
	const TArray<FString> WalletFields = Catalog->GetTemplateFieldNames(TEXT("Wallet"));
	TestEqual(TEXT("wallet has one field"), WalletFields.Num(), 1);
	TestEqual(TEXT("field name is the declared one"),
		WalletFields.Num() == 1 ? WalletFields[0] : FString(), FString(TEXT("game_currencies")));

	// The declared type rides along so a picker can label it.
	if (const FFlockCatalogTemplate* Wallet = Catalog->FindTemplateByName(TEXT("Wallet")))
	{
		TestEqual(TEXT("declared type kept"), Wallet->Fields[0].Type, FString(TEXT("dict")));
	}

	TestEqual(TEXT("config fields"), Catalog->GetConfigFieldNames(TEXT("Gameplay")).Num(), 1);

	return true;
}

// ── Achievements come from the tagged template's fields ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCatalogAchievementTest, "Flock.Editor.Catalog.AchievementsComeFromTaggedTemplate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCatalogAchievementTest::RunTest(const FString& Parameters)
{
	UFlockContentCatalog* Catalog = Build(Snapshot());

	TestEqual(TEXT("both achievements"), Catalog->Achievements.Num(), 2);
	TestTrue(TEXT("first_win listed"), Catalog->Achievements.Contains(TEXT("first_win")));
	TestTrue(TEXT("flawless listed"), Catalog->Achievements.Contains(TEXT("flawless")));
	// Sorted, so a backend reordering its fields does not churn the asset.
	TestEqual(TEXT("sorted"), Catalog->Achievements[0], FString(TEXT("first_win")));

	TestNotNull(TEXT("tag lookup finds it"), Catalog->FindTemplateByTag(TEXT("achievement")));
	TestNotNull(TEXT("tag lookup is case-insensitive"), Catalog->FindTemplateByTag(TEXT("Achievement")));
	TestNull(TEXT("unknown tag"), Catalog->FindTemplateByTag(TEXT("nope")));

	// A game with no achievement template simply has none — not an error.
	FFlockSchemaSnapshot NoAchievements = Snapshot();
	NoAchievements.PlayerTemplates.RemoveAll([](const FFlockPlayerTemplateSchema& T) { return T.Tag == TEXT("achievement"); });
	TestEqual(TEXT("no tagged template, no achievements"), Build(NoAchievements)->Achievements.Num(), 0);

	return true;
}

// ── Currencies are gathered across shops, de-duplicated and sorted ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCatalogCurrencyTest, "Flock.Editor.Catalog.GathersDistinctCurrencies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCatalogCurrencyTest::RunTest(const FString& Parameters)
{
	FFlockSchemaSnapshot Snap = Snapshot();
	// A second shop reusing Gold, so de-duplication is actually exercised.
	FFlockShop Second;
	Second.Id = TEXT("shop-2");
	Second.Name = TEXT("Premium");
	Second.ShopItems.Add(Item(TEXT("item-3"), TEXT("Bundle"), TEXT("Gold"), 500));
	Snap.Shops.Add(Second);

	UFlockContentCatalog* Catalog = Build(Snap);

	TestEqual(TEXT("two distinct currencies"), Catalog->Currencies.Num(), 2);
	TestEqual(TEXT("sorted"), Catalog->Currencies[0], FString(TEXT("Gold")));
	TestEqual(TEXT("sorted"), Catalog->Currencies[1], FString(TEXT("Shard")));

	// Item ids are unique game-wide, so a name lookup can span shops.
	TestEqual(TEXT("item id by name"), Catalog->FindShopItemId(TEXT("Bundle")), FString(TEXT("item-3")));
	TestEqual(TEXT("unknown item"), Catalog->FindShopItemId(TEXT("Nope")), FString());
	TestEqual(TEXT("all item names"), Catalog->GetShopItemNames().Num(), 3);

	// Price is recorded for browsing only; it must not be mistaken for live data.
	if (Catalog->Shops.Num() > 0 && Catalog->Shops[0].Items.Num() > 0)
	{
		TestEqual(TEXT("price snapshotted"), Catalog->Shops[0].Items[0].PriceAtSync, 100);
	}

	return true;
}

// ── Ordering is stable, so an unchanged re-sync is not a diff ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCatalogOrderingTest, "Flock.Editor.Catalog.OrdersStablyRegardlessOfServerOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCatalogOrderingTest::RunTest(const FString& Parameters)
{
	UFlockContentCatalog* First = Build(Snapshot());

	// The same content, delivered in the opposite order.
	FFlockSchemaSnapshot Reversed = Snapshot();
	Algo::Reverse(Reversed.PlayerTemplates);
	Algo::Reverse(Reversed.Shops[0].ShopItems);
	UFlockContentCatalog* Second = Build(Reversed);

	TestEqual(TEXT("templates sorted by id"), First->PlayerTemplates[0].Id, FString(TEXT("tmpl-1")));
	TestEqual(TEXT("same order regardless of server order"),
		Second->PlayerTemplates[0].Id, First->PlayerTemplates[0].Id);
	TestEqual(TEXT("items sorted by id too"),
		Second->Shops[0].Items[0].Id, First->Shops[0].Items[0].Id);
	// The catalog and the manifest must agree they describe the same sync.
	TestEqual(TEXT("hash agrees across orderings"), Second->ContentHash, First->ContentHash);

	return true;
}

// ── A malformed or absent schema costs that one entry's fields, not the sync ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCatalogBadSchemaTest, "Flock.Editor.Catalog.SurvivesUnreadableSchemas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCatalogBadSchemaTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("empty schema"), FFlockCatalogBuilder::ReadFields(FString()).Num(), 0);
	TestEqual(TEXT("unparseable schema"), FFlockCatalogBuilder::ReadFields(TEXT("{not json")).Num(), 0);
	// An entry with no field_name could not be written to, so it is not a field.
	TestEqual(TEXT("entry without a name is skipped"),
		FFlockCatalogBuilder::ReadFields(TEXT("[{\"type\":\"int\"},{\"type\":\"int\",\"field_name\":\"ok\"}]")).Num(), 1);

	// One broken template does not stop the others being browsable.
	FFlockSchemaSnapshot Snap = Snapshot();
	Snap.PlayerTemplates[0].SchemaJson = TEXT("{not json");
	UFlockContentCatalog* Catalog = Build(Snap);

	TestEqual(TEXT("both templates still listed"), Catalog->PlayerTemplates.Num(), 2);
	TestEqual(TEXT("the good one keeps its fields"), Catalog->GetTemplateFieldNames(TEXT("Wallet")).Num(), 1);
	TestEqual(TEXT("the broken one has none"), Catalog->GetTemplateFieldNames(TEXT("Trophies")).Num(), 0);

	return true;
}

// ── An empty game produces an empty catalog, not a failure ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCatalogEmptyTest, "Flock.Editor.Catalog.EmptyGameIsAnEmptyCatalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCatalogEmptyTest::RunTest(const FString& Parameters)
{
	FFlockSchemaSnapshot Empty;
	Empty.GameVersionId = TEXT("ver-1");
	UFlockContentCatalog* Catalog = Build(Empty);

	TestTrue(TEXT("reports empty"), Catalog->IsEmptyCatalog());
	TestEqual(TEXT("no names"), Catalog->GetTemplateNames().Num(), 0);
	TestEqual(TEXT("no fields for an unknown template"), Catalog->GetTemplateFieldNames(TEXT("Nope")).Num(), 0);
	TestEqual(TEXT("no currencies"), Catalog->Currencies.Num(), 0);
	TestNull(TEXT("no template by name"), Catalog->FindTemplateByName(TEXT("Nope")));
	TestNull(TEXT("no config by name"), Catalog->FindConfigByName(TEXT("Nope")));

	// Repopulating over a filled catalog must clear it, not append — a re-sync after content was deleted
	// on the backend has to shrink the asset.
	UFlockContentCatalog* Reused = Build(Snapshot());
	TestFalse(TEXT("filled first"), Reused->IsEmptyCatalog());
	FFlockCatalogBuilder::Populate(Empty, *Reused);
	TestTrue(TEXT("cleared on repopulate"), Reused->IsEmptyCatalog());
	TestEqual(TEXT("achievements cleared too"), Reused->Achievements.Num(), 0);
	TestEqual(TEXT("currencies cleared too"), Reused->Currencies.Num(), 0);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
