// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Blueprint/FlockGameConfigLibrary.h"
#include "Http/FlockJsonUtils.h"
#include "Models/FlockConfigModels.h"

namespace
{
	FFlockGameConfigData MakeData()
	{
		// A config `data` with a scalar int, a nested object, a bool, a string, and a string list.
		const FString Body =
			TEXT("{\"result\":{\"id\":\"cfg\",\"name\":\"n\",\"game_id\":\"g\",\"tag\":\"gameplay\",")
			TEXT("\"created_at\":\"\",\"updated_at\":\"\",\"data\":[")
			TEXT("{\"type\":\"int\",\"field_name\":\"max_health\",\"value\":100},")
			TEXT("{\"type\":\"string\",\"field_name\":\"boss_name\",\"value\":\"Duckzilla\"},")
			TEXT("{\"type\":\"bool\",\"field_name\":\"hardcore\",\"value\":true},")
			TEXT("{\"type\":\"object\",\"field_name\":\"stats\",\"value\":[")
			TEXT("{\"type\":\"float\",\"field_name\":\"crit_chance\",\"value\":0.25}]},")
			TEXT("{\"type\":\"list\",\"field_name\":\"tiers\",\"value\":[")
			TEXT("{\"type\":\"string\",\"field_name\":\"\",\"value\":\"bronze\"},")
			TEXT("{\"type\":\"string\",\"field_name\":\"\",\"value\":\"gold\"}]}")
			TEXT("]}}");
		TSharedPtr<FJsonObject> Object;
		FFlockJsonUtils::TryParseObject(Body, Object);
		const TSharedPtr<FJsonObject>* Result = nullptr;
		Object->TryGetObjectField(TEXT("result"), Result);
		FFlockGameConfigSchema Config;
		FString Error;
		FFlockGameConfigSchema::FromWireObject((*Result).ToSharedRef(), Config, Error);
		return Config.Data;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigLibraryReadsTest, "Flock.Config.Library.Reads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigLibraryReadsTest::RunTest(const FString& Parameters)
{
	const FFlockGameConfigData Data = MakeData();

	// Pascal name and the dashboard's snake name both resolve.
	TestEqual(TEXT("int by Pascal"), UFlockGameConfigLibrary::GetConfigInt(Data, TEXT("MaxHealth"), -1), 100);
	TestEqual(TEXT("int by snake"), UFlockGameConfigLibrary::GetConfigInt(Data, TEXT("max_health"), -1), 100);
	TestEqual(TEXT("string"), UFlockGameConfigLibrary::GetConfigString(Data, TEXT("boss_name"), TEXT("x")), FString(TEXT("Duckzilla")));
	TestTrue(TEXT("bool"), UFlockGameConfigLibrary::GetConfigBool(Data, TEXT("hardcore"), false));

	// Dotted path into the nested object, either casing.
	TestEqual(TEXT("nested float Pascal"), UFlockGameConfigLibrary::GetConfigFloat(Data, TEXT("Stats.CritChance"), -1.f), 0.25f);
	TestEqual(TEXT("nested float snake"), UFlockGameConfigLibrary::GetConfigFloat(Data, TEXT("stats.crit_chance"), -1.f), 0.25f);

	// String array.
	const TArray<FString> Tiers = UFlockGameConfigLibrary::GetConfigStringArray(Data, TEXT("tiers"));
	TestEqual(TEXT("two tiers"), Tiers.Num(), 2);
	if (Tiers.Num() == 2)
	{
		TestEqual(TEXT("tier 0"), Tiers[0], FString(TEXT("bronze")));
		TestEqual(TEXT("tier 1"), Tiers[1], FString(TEXT("gold")));
	}

	// Presence, field names, validity, json.
	TestTrue(TEXT("has present field"), UFlockGameConfigLibrary::HasConfigField(Data, TEXT("MaxHealth")));
	TestFalse(TEXT("missing field absent"), UFlockGameConfigLibrary::HasConfigField(Data, TEXT("nope")));
	TestTrue(TEXT("data valid"), UFlockGameConfigLibrary::IsConfigDataValid(Data));
	TestTrue(TEXT("field names include Stats"), UFlockGameConfigLibrary::GetConfigFieldNames(Data).Contains(TEXT("Stats")));
	TestFalse(TEXT("json not empty"), UFlockGameConfigLibrary::ConfigDataToJsonString(Data).IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigLibraryFallbackTest, "Flock.Config.Library.Fallbacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigLibraryFallbackTest::RunTest(const FString& Parameters)
{
	const FFlockGameConfigData Data = MakeData();

	// A missing path returns the fallback.
	TestEqual(TEXT("missing int -> fallback"), UFlockGameConfigLibrary::GetConfigInt(Data, TEXT("absent"), 42), 42);
	TestEqual(TEXT("missing string -> fallback"), UFlockGameConfigLibrary::GetConfigString(Data, TEXT("absent"), TEXT("def")), FString(TEXT("def")));

	// A wrong-type read returns the fallback rather than a coerced value (boss_name is a string).
	TestEqual(TEXT("string read as int -> fallback"), UFlockGameConfigLibrary::GetConfigInt(Data, TEXT("boss_name"), 7), 7);

	// Empty / invalid data returns fallbacks throughout.
	const FFlockGameConfigData Empty;
	TestFalse(TEXT("empty data invalid"), UFlockGameConfigLibrary::IsConfigDataValid(Empty));
	TestEqual(TEXT("empty int -> fallback"), UFlockGameConfigLibrary::GetConfigInt(Empty, TEXT("anything"), 9), 9);
	TestEqual(TEXT("empty field names"), UFlockGameConfigLibrary::GetConfigFieldNames(Empty).Num(), 0);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
