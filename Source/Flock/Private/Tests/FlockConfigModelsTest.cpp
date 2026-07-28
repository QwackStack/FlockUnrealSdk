// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockJsonUtils.h"
#include "Models/FlockConfigModels.h"
#include "Models/FlockGameModels.h"
#include "Tests/Support/FlockConfigCodegenFixture.h"

namespace
{
	// A config whose `data` exercises every node type: a scalar, an object (field names), a dict (author
	// keys), and a list. `schema` is present so the verbatim-passthrough can be checked.
	const TCHAR* const ConfigBody =
		TEXT("{")
		TEXT("\"id\":\"cfg-1\",\"name\":\"Balance\",\"game_id\":\"game-1\",\"game_version_id\":\"ver-1\",")
		TEXT("\"tag\":\"gameplay\",\"created_at\":\"2026-01-01T00:00:00Z\",\"updated_at\":\"2026-01-02T00:00:00Z\",")
		TEXT("\"schema\":[{\"type\":\"int\",\"field_name\":\"max_health\",\"type_name\":\"int\"}],")
		TEXT("\"data\":[")
		TEXT("{\"type\":\"int\",\"field_name\":\"max_health\",\"value\":100},")
		TEXT("{\"type\":\"object\",\"field_name\":\"stats\",\"value\":[")
		TEXT("{\"type\":\"float\",\"field_name\":\"crit_chance\",\"value\":0.25}]},")
		TEXT("{\"type\":\"dict\",\"field_name\":\"loot_table\",\"value\":{")
		TEXT("\"rare_sword\":{\"type\":\"int\",\"field_name\":\"\",\"value\":5},")
		TEXT("\"epic_shield\":{\"type\":\"int\",\"field_name\":\"\",\"value\":2}}},")
		TEXT("{\"type\":\"list\",\"field_name\":\"spawn_points\",\"value\":[")
		TEXT("{\"type\":\"string\",\"field_name\":\"\",\"value\":\"north\"},")
		TEXT("{\"type\":\"string\",\"field_name\":\"\",\"value\":\"south\"}]}")
		TEXT("]}");

	bool ParseConfig(const FString& Body, FFlockGameConfigSchema& OutConfig)
	{
		TSharedPtr<FJsonObject> Object;
		if (!FFlockJsonUtils::TryParseObject(Body, Object) || !Object.IsValid())
		{
			return false;
		}
		FString Error;
		// Through WireObjectToStruct on purpose: proves the model is routed to its custom FromWireObject.
		return FFlockJsonUtils::WireObjectToStruct(Object.ToSharedRef(), OutConfig, Error);
	}
}

// The load-bearing test: an `object` node's children are field names (snake -> Pascal), while a `dict`
// node's children are author-supplied keys (verbatim). A blind key transform cannot tell them apart and
// would corrupt every dictionary; this pins that the flatten keeps them distinct.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigDictVsFieldNameTest, "Flock.Config.Models.DictVsFieldName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigDictVsFieldNameTest::RunTest(const FString& Parameters)
{
	FFlockGameConfigSchema Config;
	TestTrue(TEXT("config parses"), ParseConfig(ConfigBody, Config));

	const FString Flat = Config.Data.ToJsonString();

	// Field names were Pascal-cased.
	TestTrue(TEXT("object field name -> Pascal (LootTable)"), Flat.Contains(TEXT("\"LootTable\"")));
	TestFalse(TEXT("no snake field name survives (loot_table)"), Flat.Contains(TEXT("\"loot_table\"")));

	// Author dict keys were NOT transformed.
	TestTrue(TEXT("dict key kept verbatim (rare_sword)"), Flat.Contains(TEXT("\"rare_sword\"")));
	TestTrue(TEXT("dict key kept verbatim (epic_shield)"), Flat.Contains(TEXT("\"epic_shield\"")));
	TestFalse(TEXT("dict key NOT Pascal-cased (RareSword)"), Flat.Contains(TEXT("\"RareSword\"")));

	// And they resolve accordingly: the dict entry reads by its verbatim key, not a Pascal one.
	int32 RareCount = 0;
	TestTrue(TEXT("verbatim dict path resolves"), Config.Data.TryGetInt(TEXT("LootTable.rare_sword"), RareCount));
	TestEqual(TEXT("dict value intact"), RareCount, 5);
	int32 Miss = 0;
	TestFalse(TEXT("Pascal dict path does NOT resolve"), Config.Data.TryGetInt(TEXT("LootTable.RareSword"), Miss));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigFlattenTest, "Flock.Config.Models.Flatten",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigFlattenTest::RunTest(const FString& Parameters)
{
	FFlockGameConfigSchema Config;
	TestTrue(TEXT("config parses"), ParseConfig(ConfigBody, Config));

	// Scalar and nested object reads; snake input resolves via Pascal fallback.
	int32 Health = 0;
	TestTrue(TEXT("scalar reads (Pascal)"), Config.Data.TryGetInt(TEXT("MaxHealth"), Health));
	TestEqual(TEXT("scalar value"), Health, 100);
	int32 HealthSnake = 0;
	TestTrue(TEXT("scalar reads (snake input)"), Config.Data.TryGetInt(TEXT("max_health"), HealthSnake));
	TestEqual(TEXT("snake resolves same value"), HealthSnake, 100);

	float Crit = 0.f;
	TestTrue(TEXT("nested float reads"), Config.Data.TryGetFloat(TEXT("Stats.CritChance"), Crit));
	TestEqual(TEXT("nested float value"), Crit, 0.25f);
	float CritSnake = 0.f;
	TestTrue(TEXT("nested float reads (snake path)"), Config.Data.TryGetFloat(TEXT("stats.crit_chance"), CritSnake));

	// List of scalars.
	TArray<FString> Spawns;
	TestTrue(TEXT("string array reads"), Config.Data.TryGetStringArray(TEXT("SpawnPoints"), Spawns));
	TestEqual(TEXT("two spawn points"), Spawns.Num(), 2);
	if (Spawns.Num() == 2)
	{
		TestEqual(TEXT("spawn 0"), Spawns[0], FString(TEXT("north")));
		TestEqual(TEXT("spawn 1"), Spawns[1], FString(TEXT("south")));
	}

	// Top-level field names, presence, and misses.
	TestTrue(TEXT("HasField true for present"), Config.Data.HasField(TEXT("MaxHealth")));
	TestFalse(TEXT("HasField false for absent"), Config.Data.HasField(TEXT("nope")));
	int32 Absent = 7;
	TestFalse(TEXT("absent read returns false"), Config.Data.TryGetInt(TEXT("nope"), Absent));
	const TArray<FString> Names = Config.Data.GetFieldNames();
	TestTrue(TEXT("top-level names include MaxHealth"), Names.Contains(TEXT("MaxHealth")));
	TestTrue(TEXT("top-level names include Stats"), Names.Contains(TEXT("Stats")));

	// Scalar fields on the schema itself.
	TestEqual(TEXT("id"), Config.Id, FString(TEXT("cfg-1")));
	TestEqual(TEXT("game_id -> GameId"), Config.GameId, FString(TEXT("game-1")));
	TestEqual(TEXT("tag stays a string"), Config.Tag, FString(TEXT("gameplay")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigSchemaVerbatimTest, "Flock.Config.Models.SchemaVerbatim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigSchemaVerbatimTest::RunTest(const FString& Parameters)
{
	FFlockGameConfigSchema Config;
	TestTrue(TEXT("config parses"), ParseConfig(ConfigBody, Config));

	// `schema` is kept as verbatim JSON for codegen — not flattened, not case-transformed.
	TestFalse(TEXT("schema captured"), Config.SchemaJson.IsEmpty());
	TestTrue(TEXT("schema keeps snake field_name"), Config.SchemaJson.Contains(TEXT("field_name")));
	TestTrue(TEXT("schema keeps the raw value"), Config.SchemaJson.Contains(TEXT("max_health")));
	TestTrue(TEXT("schema kept its structure key type_name"), Config.SchemaJson.Contains(TEXT("type_name")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigGetDataAsTest, "Flock.Config.Models.GetDataAs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigGetDataAsTest::RunTest(const FString& Parameters)
{
	// GetDataAs<T> binds the flattened object to a USTRUCT by reflection with no key transform. Reuse an
	// existing reflected model (FFlockGameVersionSchema: Id, ReleaseType) as the target, driving the
	// flatten with matching field names so the Pascal keys line up.
	const FString Body =
		TEXT("{\"id\":\"x\",\"name\":\"n\",\"game_id\":\"g\",\"tag\":\"gameplay\",")
		TEXT("\"created_at\":\"\",\"updated_at\":\"\",\"data\":[")
		TEXT("{\"type\":\"string\",\"field_name\":\"id\",\"value\":\"payload-id\"},")
		TEXT("{\"type\":\"string\",\"field_name\":\"release_type\",\"value\":\"prod\"}]}");

	FFlockGameConfigSchema Config;
	TestTrue(TEXT("config parses"), ParseConfig(Body, Config));

	FFlockGameVersionSchema Bound;
	TestTrue(TEXT("GetDataAs binds"), Config.Data.GetDataAs(Bound));
	TestEqual(TEXT("Id bound"), Bound.Id, FString(TEXT("payload-id")));
	TestEqual(TEXT("release_type -> ReleaseType bound"), Bound.ReleaseType, FString(TEXT("prod")));

	return true;
}

// The codegen-critical path: GetDataAs<T> binds the shapes a generated config type actually has — a
// nested struct (object node), a list, and a dictionary (dict node → TMap with verbatim keys) — end to
// end through UE reflection, with no key transform beyond the one the flatten already applied.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigGetDataAsNestedTest, "Flock.Config.Models.GetDataAsNested",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigGetDataAsNestedTest::RunTest(const FString& Parameters)
{
	const FString Body =
		TEXT("{\"id\":\"cfg\",\"name\":\"n\",\"game_id\":\"g\",\"tag\":\"gameplay\",\"created_at\":\"\",\"updated_at\":\"\",")
		TEXT("\"data\":[")
		TEXT("{\"type\":\"int\",\"field_name\":\"max_health\",\"value\":100},")
		TEXT("{\"type\":\"string\",\"field_name\":\"boss_name\",\"value\":\"Duckzilla\"},")
		TEXT("{\"type\":\"bool\",\"field_name\":\"hardcore\",\"value\":true},")
		TEXT("{\"type\":\"object\",\"field_name\":\"stats\",\"value\":[")
		TEXT("{\"type\":\"int\",\"field_name\":\"max_health\",\"value\":50},")
		TEXT("{\"type\":\"float\",\"field_name\":\"crit_chance\",\"value\":0.25}]},")
		TEXT("{\"type\":\"list\",\"field_name\":\"tiers\",\"value\":[")
		TEXT("{\"type\":\"string\",\"field_name\":\"\",\"value\":\"bronze\"},")
		TEXT("{\"type\":\"string\",\"field_name\":\"\",\"value\":\"gold\"}]},")
		TEXT("{\"type\":\"dict\",\"field_name\":\"loot_table\",\"value\":{")
		TEXT("\"rare_sword\":{\"type\":\"int\",\"field_name\":\"\",\"value\":5},")
		TEXT("\"epic_shield\":{\"type\":\"int\",\"field_name\":\"\",\"value\":2}}}")
		TEXT("]}");

	FFlockGameConfigSchema Config;
	TestTrue(TEXT("config parses"), ParseConfig(Body, Config));

	FFlockCodegenConfigFixture Bound;
	TestTrue(TEXT("GetDataAs binds the whole shape"), Config.Data.GetDataAs(Bound));

	// Scalars.
	TestEqual(TEXT("int"), Bound.MaxHealth, 100);
	TestEqual(TEXT("string"), Bound.BossName, FString(TEXT("Duckzilla")));
	TestTrue(TEXT("bool"), Bound.Hardcore);

	// Nested struct from an object node.
	TestEqual(TEXT("nested int"), Bound.Stats.MaxHealth, 50);
	TestEqual(TEXT("nested float"), Bound.Stats.CritChance, 0.25f);

	// List.
	TestEqual(TEXT("two tiers"), Bound.Tiers.Num(), 2);
	if (Bound.Tiers.Num() == 2)
	{
		TestEqual(TEXT("tier 0"), Bound.Tiers[0], FString(TEXT("bronze")));
		TestEqual(TEXT("tier 1"), Bound.Tiers[1], FString(TEXT("gold")));
	}

	// Dictionary -> TMap, keyed by the verbatim author keys.
	TestEqual(TEXT("two loot entries"), Bound.LootTable.Num(), 2);
	const int32* Rare = Bound.LootTable.Find(TEXT("rare_sword"));
	const int32* Epic = Bound.LootTable.Find(TEXT("epic_shield"));
	TestTrue(TEXT("rare_sword present and correct"), Rare != nullptr && *Rare == 5);
	TestTrue(TEXT("epic_shield present and correct"), Epic != nullptr && *Epic == 2);
	// The author keys were NOT Pascal-cased on the way into the map.
	TestFalse(TEXT("no Pascal-cased dict key"), Bound.LootTable.Contains(TEXT("RareSword")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigLegacyAndEmptyTest, "Flock.Config.Models.LegacyAndEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigLegacyAndEmptyTest::RunTest(const FString& Parameters)
{
	// Legacy `data`: a plain dict[str, Any] is already flat and is adopted verbatim (Unity would throw here).
	const FString Legacy =
		TEXT("{\"id\":\"cfg-legacy\",\"name\":\"n\",\"game_id\":\"g\",\"tag\":\"gameplay\",")
		TEXT("\"created_at\":\"\",\"updated_at\":\"\",\"data\":{\"max_health\":50,\"label\":\"boss\"}}");
	FFlockGameConfigSchema LegacyConfig;
	TestTrue(TEXT("legacy parses"), ParseConfig(Legacy, LegacyConfig));
	int32 LegacyHealth = 0;
	TestTrue(TEXT("legacy value reads verbatim key"), LegacyConfig.Data.TryGetInt(TEXT("max_health"), LegacyHealth));
	TestEqual(TEXT("legacy value intact"), LegacyHealth, 50);
	TestTrue(TEXT("legacy key kept verbatim"), LegacyConfig.Data.ToJsonString().Contains(TEXT("\"max_health\"")));

	// Absent/null `data`: a valid-but-empty handle.
	const FString Empty =
		TEXT("{\"id\":\"cfg-empty\",\"name\":\"n\",\"game_id\":\"g\",\"tag\":\"gameplay\",")
		TEXT("\"created_at\":\"\",\"updated_at\":\"\"}");
	FFlockGameConfigSchema EmptyConfig;
	TestTrue(TEXT("empty parses"), ParseConfig(Empty, EmptyConfig));
	TestFalse(TEXT("empty data is not valid"), EmptyConfig.Data.IsValid());
	TestEqual(TEXT("empty data has no fields"), EmptyConfig.Data.GetFieldNames().Num(), 0);
	FString NoValue;
	TestFalse(TEXT("empty read returns false"), EmptyConfig.Data.TryGetString(TEXT("anything"), NoValue));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigSnapshotRoundTripTest, "Flock.Config.Models.SnapshotRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigSnapshotRoundTripTest::RunTest(const FString& Parameters)
{
	// A parsed config survives the snapshot's plain (PascalCase, no-transform) round-trip: the flattened
	// data and the verbatim schema both come back intact, which is what makes offline caching correct.
	FFlockGameConfigSchema Config;
	TestTrue(TEXT("config parses"), ParseConfig(ConfigBody, Config));

	FString Snapshot;
	TestTrue(TEXT("serializes to snapshot"), FFlockJsonUtils::StructToPlainJson(Config, Snapshot));

	FFlockGameConfigSchema Restored;
	TestTrue(TEXT("restores from snapshot"), FFlockJsonUtils::PlainJsonToStruct(Snapshot, Restored));

	TestEqual(TEXT("id survives"), Restored.Id, FString(TEXT("cfg-1")));
	int32 Health = 0;
	TestTrue(TEXT("flattened data survives"), Restored.Data.TryGetInt(TEXT("MaxHealth"), Health));
	TestEqual(TEXT("data value intact"), Health, 100);
	int32 Rare = 0;
	TestTrue(TEXT("dict entry survives with verbatim key"), Restored.Data.TryGetInt(TEXT("LootTable.rare_sword"), Rare));
	TestEqual(TEXT("dict value intact"), Rare, 5);
	TestTrue(TEXT("schema survives verbatim"), Restored.SchemaJson.Contains(TEXT("field_name")));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
