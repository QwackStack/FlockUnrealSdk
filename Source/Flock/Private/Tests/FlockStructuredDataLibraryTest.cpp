// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Blueprint/FlockStructuredDataLibrary.h"
#include "Http/FlockJsonUtils.h"
#include "Models/FlockConfigModels.h"

namespace
{
	FFlockStructuredData MakeData()
	{
		// A `data` with a scalar int, a nested object, a bool, a string, and a string list — built through a
		// config schema, a convenient producer of the shared handle.
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockStructuredDataLibraryReadsTest, "Flock.StructuredData.Library.Reads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockStructuredDataLibraryReadsTest::RunTest(const FString& Parameters)
{
	const FFlockStructuredData Data = MakeData();

	// Pascal name and the dashboard's snake name both resolve.
	TestEqual(TEXT("int by Pascal"), UFlockStructuredDataLibrary::GetDataInt(Data, TEXT("MaxHealth"), -1), 100);
	TestEqual(TEXT("int by snake"), UFlockStructuredDataLibrary::GetDataInt(Data, TEXT("max_health"), -1), 100);
	TestEqual(TEXT("string"), UFlockStructuredDataLibrary::GetDataString(Data, TEXT("boss_name"), TEXT("x")), FString(TEXT("Duckzilla")));
	TestTrue(TEXT("bool"), UFlockStructuredDataLibrary::GetDataBool(Data, TEXT("hardcore"), false));

	// Dotted path into the nested object, either casing.
	TestEqual(TEXT("nested float Pascal"), UFlockStructuredDataLibrary::GetDataFloat(Data, TEXT("Stats.CritChance"), -1.f), 0.25f);
	TestEqual(TEXT("nested float snake"), UFlockStructuredDataLibrary::GetDataFloat(Data, TEXT("stats.crit_chance"), -1.f), 0.25f);

	// String array.
	const TArray<FString> Tiers = UFlockStructuredDataLibrary::GetDataStringArray(Data, TEXT("tiers"));
	TestEqual(TEXT("two tiers"), Tiers.Num(), 2);
	if (Tiers.Num() == 2)
	{
		TestEqual(TEXT("tier 0"), Tiers[0], FString(TEXT("bronze")));
		TestEqual(TEXT("tier 1"), Tiers[1], FString(TEXT("gold")));
	}

	// Presence, field names, validity, json.
	TestTrue(TEXT("has present field"), UFlockStructuredDataLibrary::HasDataField(Data, TEXT("MaxHealth")));
	TestFalse(TEXT("missing field absent"), UFlockStructuredDataLibrary::HasDataField(Data, TEXT("nope")));
	TestTrue(TEXT("data valid"), UFlockStructuredDataLibrary::IsValidData(Data));
	TestTrue(TEXT("field names include Stats"), UFlockStructuredDataLibrary::GetDataFieldNames(Data).Contains(TEXT("Stats")));
	TestFalse(TEXT("json not empty"), UFlockStructuredDataLibrary::DataToJsonString(Data).IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockStructuredDataLibraryFallbackTest, "Flock.StructuredData.Library.Fallbacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockStructuredDataLibraryFallbackTest::RunTest(const FString& Parameters)
{
	const FFlockStructuredData Data = MakeData();

	// A missing path returns the fallback.
	TestEqual(TEXT("missing int -> fallback"), UFlockStructuredDataLibrary::GetDataInt(Data, TEXT("absent"), 42), 42);
	TestEqual(TEXT("missing string -> fallback"), UFlockStructuredDataLibrary::GetDataString(Data, TEXT("absent"), TEXT("def")), FString(TEXT("def")));

	// A wrong-type read returns the fallback rather than a coerced value (boss_name is a string).
	TestEqual(TEXT("string read as int -> fallback"), UFlockStructuredDataLibrary::GetDataInt(Data, TEXT("boss_name"), 7), 7);

	// Empty / invalid data returns fallbacks throughout.
	const FFlockStructuredData Empty;
	TestFalse(TEXT("empty data invalid"), UFlockStructuredDataLibrary::IsValidData(Empty));
	TestEqual(TEXT("empty int -> fallback"), UFlockStructuredDataLibrary::GetDataInt(Empty, TEXT("anything"), 9), 9);
	TestEqual(TEXT("empty field names"), UFlockStructuredDataLibrary::GetDataFieldNames(Empty).Num(), 0);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
