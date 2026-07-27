// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Blueprint/FlockCommandDataLibrary.h"
#include "Http/FlockJsonUtils.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockStructuredData.h"

// ── Every setter keeps its JSON type; the wire is not a bag of strings ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandDataTypesTest, "Flock.Command.Data.SettersKeepJsonTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandDataTypesTest::RunTest(const FString& Parameters)
{
	const FFlockCommandData Data = FFlockCommandData()
		.Set(TEXT("level"), 7)
		.Set(TEXT("ratio"), 0.5f)
		.Set(TEXT("name"), TEXT("Ada"))
		.Set(TEXT("flawless"), true)
		.Set(TEXT("tags"), TArray<FString>{ TEXT("a"), TEXT("b") });

	const FString Json = Data.ToJsonString();
	TestTrue(TEXT("int unquoted"), Json.Contains(TEXT("\"level\":7")));
	TestTrue(TEXT("float unquoted"), Json.Contains(TEXT("\"ratio\":0.5")));
	TestTrue(TEXT("string quoted"), Json.Contains(TEXT("\"name\":\"Ada\"")));
	TestTrue(TEXT("bool unquoted"), Json.Contains(TEXT("\"flawless\":true")));
	TestTrue(TEXT("array preserved"), Json.Contains(TEXT("\"tags\":[\"a\",\"b\"]")));
	TestEqual(TEXT("five fields"), Data.GetFieldNames().Num(), 5);
	TestFalse(TEXT("not empty"), Data.IsEmpty());

	return true;
}

// ── Field names go out exactly as typed: only the author knows what the template declares ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandDataVerbatimKeysTest, "Flock.Command.Data.KeysAreVerbatim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandDataVerbatimKeysTest::RunTest(const FString& Parameters)
{
	const FFlockCommandData Data = FFlockCommandData().Set(TEXT("max_health"), 100).Set(TEXT("MaxMana"), 50);
	const FString Json = Data.ToJsonString();

	TestTrue(TEXT("snake_case key untouched"), Json.Contains(TEXT("\"max_health\":100")));
	TestTrue(TEXT("Pascal key untouched"), Json.Contains(TEXT("\"MaxMana\":50")));
	TestFalse(TEXT("no snake->Pascal transform"), Json.Contains(TEXT("MaxHealth")));

	return true;
}

// ── A value with JSON-significant characters survives, because the serializer does the escaping ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandDataEscapingTest, "Flock.Command.Data.EscapesAwkwardStrings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandDataEscapingTest::RunTest(const FString& Parameters)
{
	const FString Awkward = TEXT("he said \"hi\"\\ then left\nline2");
	const FFlockCommandData Data = FFlockCommandData().Set(TEXT("note"), Awkward);

	// Round-tripping through the JSON parser is the real assertion: a hand-rolled escape would not survive it.
	FString RoundTripped;
	TestTrue(TEXT("re-parses"), Data.ToJsonObject()->TryGetStringField(TEXT("note"), RoundTripped));
	TestEqual(TEXT("value intact"), RoundTripped, Awkward);

	return true;
}

// ── Later sets replace earlier ones for the same key ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandDataOverwriteTest, "Flock.Command.Data.LastSetWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandDataOverwriteTest::RunTest(const FString& Parameters)
{
	const FFlockCommandData Data = FFlockCommandData().Set(TEXT("coins"), 1).Set(TEXT("coins"), 99);
	TestEqual(TEXT("one field"), Data.GetFieldNames().Num(), 1);
	TestTrue(TEXT("latest value"), Data.ToJsonString().Contains(TEXT("\"coins\":99")));

	return true;
}

// ── The raw-JSON escape hatch carries a nested shape; invalid JSON degrades to null, never to corruption ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandDataRawJsonTest, "Flock.Command.Data.RawJsonNestsAndFailsSafe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandDataRawJsonTest::RunTest(const FString& Parameters)
{
	const FFlockCommandData Nested = FFlockCommandData().SetRawJson(TEXT("loadout"), TEXT("{\"weapon\":\"bow\"}"));
	TestTrue(TEXT("nested object spliced in"), Nested.ToJsonString().Contains(TEXT("\"loadout\":{\"weapon\":\"bow\"}")));

	const FFlockCommandValue Broken = FFlockCommandValue::FromRawJson(TEXT("{not json"));
	TestEqual(TEXT("unparseable becomes null"), Broken.ToJsonString(), FString(TEXT("null")));

	return true;
}

// ── An empty bag is an empty object, not a null: the route requires `data` ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandDataEmptyTest, "Flock.Command.Data.EmptyIsAnObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandDataEmptyTest::RunTest(const FString& Parameters)
{
	const FFlockCommandData Empty;
	TestTrue(TEXT("reports empty"), Empty.IsEmpty());
	TestEqual(TEXT("serializes as {}"), Empty.ToJsonString(), FString(TEXT("{}")));
	TestEqual(TEXT("no field names"), Empty.GetFieldNames().Num(), 0);

	return true;
}

// ── A queued command round-trips through the snapshot format it is persisted in ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandPendingRoundTripTest, "Flock.Command.Data.PendingCommandRoundTrips",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandPendingRoundTripTest::RunTest(const FString& Parameters)
{
	TArray<FFlockPendingCommand> Written;
	FFlockPendingCommand Entry;
	Entry.Path = TEXT("game_command/update_player_data");
	Entry.PayloadJson = TEXT("{\"player_data_id\":\"pd-1\",\"data\":{\"coins\":250}}");
	Entry.Context = TEXT("Update player data");
	Entry.PlayerDataId = TEXT("pd-1");
	Written.Add(Entry);

	FString Json;
	TestTrue(TEXT("serializes"), FFlockJsonUtils::ArrayToPlainJson(Written, Json));

	TArray<FFlockPendingCommand> Read;
	TestTrue(TEXT("deserializes"), FFlockJsonUtils::ArrayFromPlainJson(Json, Read));
	TestEqual(TEXT("one entry"), Read.Num(), 1);
	if (Read.Num() == 1)
	{
		TestEqual(TEXT("path intact"), Read[0].Path, Entry.Path);
		// The body must survive verbatim — a replay that reshapes it is a replay that can drift from the
		// call it stands in for.
		TestEqual(TEXT("payload intact"), Read[0].PayloadJson, Entry.PayloadJson);
		TestEqual(TEXT("row id intact"), Read[0].PlayerDataId, Entry.PlayerDataId);
	}

	return true;
}

// ── The overlay resolves a caller's key against the flattened one, and doesn't disturb other fields ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandOverlayTest, "Flock.Command.Data.OverlayResolvesFlattenedKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandOverlayTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonValue> Wire = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
		MakeShared<FJsonValueObject>(([]
		{
			TSharedRef<FJsonObject> Field = MakeShared<FJsonObject>();
			Field->SetStringField(TEXT("type"), TEXT("int"));
			Field->SetStringField(TEXT("field_name"), TEXT("max_health"));
			Field->SetNumberField(TEXT("value"), 100);
			return Field;
		})()),
		MakeShared<FJsonValueObject>(([]
		{
			TSharedRef<FJsonObject> Field = MakeShared<FJsonObject>();
			Field->SetStringField(TEXT("type"), TEXT("int"));
			Field->SetStringField(TEXT("field_name"), TEXT("coins"));
			Field->SetNumberField(TEXT("value"), 5);
			return Field;
		})())
	});

	FFlockStructuredData Data = FFlockStructuredData::FromWireData(Wire);
	const FFlockStructuredData Copy = Data; // shares the cached parse until the overlay rebuilds it

	Data.OverlayFields(FFlockCommandData().Set(TEXT("max_health"), 250).Set(TEXT("armour"), 3).ToJsonObject());

	int32 Value = 0;
	TestTrue(TEXT("flattened key updated"), Data.TryGetInt(TEXT("MaxHealth"), Value));
	TestEqual(TEXT("via the caller's snake_case name"), Value, 250);
	TestTrue(TEXT("untouched field survives"), Data.TryGetInt(TEXT("Coins"), Value));
	TestEqual(TEXT("untouched value"), Value, 5);
	TestTrue(TEXT("new field added verbatim"), Data.TryGetInt(TEXT("armour"), Value));
	TestEqual(TEXT("new value"), Value, 3);
	TestEqual(TEXT("no duplicate key"), Data.GetFieldNames().Num(), 3);

	// Copies share the lazily-parsed object, so the overlay must rebuild rather than write through it.
	TestTrue(TEXT("copy unaffected"), Copy.TryGetInt(TEXT("MaxHealth"), Value));
	TestEqual(TEXT("copy still original"), Value, 100);

	return true;
}

// ── The Blueprint nodes are the struct's API, so a graph and C++ cannot drift ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandLibraryParityTest, "Flock.Command.Library.CppParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandLibraryParityTest::RunTest(const FString& Parameters)
{
	FFlockCommandData ViaNodes;
	ViaNodes = UFlockCommandDataLibrary::SetCommandInt(ViaNodes, TEXT("level"), 7);
	ViaNodes = UFlockCommandDataLibrary::SetCommandBool(ViaNodes, TEXT("flawless"), true);
	ViaNodes = UFlockCommandDataLibrary::SetCommandString(ViaNodes, TEXT("name"), TEXT("Ada"));
	ViaNodes = UFlockCommandDataLibrary::SetCommandStringArray(ViaNodes, TEXT("tags"), TArray<FString>{ TEXT("a") });

	const FFlockCommandData ViaCpp = FFlockCommandData()
		.Set(TEXT("level"), 7)
		.Set(TEXT("flawless"), true)
		.Set(TEXT("name"), TEXT("Ada"))
		.Set(TEXT("tags"), TArray<FString>{ TEXT("a") });

	TestEqual(TEXT("same JSON"), UFlockCommandDataLibrary::CommandDataToJsonString(ViaNodes), ViaCpp.ToJsonString());
	TestFalse(TEXT("empty check matches"), UFlockCommandDataLibrary::IsEmptyCommandData(ViaNodes));
	TestEqual(TEXT("field count matches"), UFlockCommandDataLibrary::GetCommandFieldNames(ViaNodes).Num(), 4);

	// A Set node returns a new bag rather than mutating its input — that is what makes it chainable.
	const FFlockCommandData Source = FFlockCommandData().Set(TEXT("coins"), 1);
	const FFlockCommandData Derived = UFlockCommandDataLibrary::SetCommandInt(Source, TEXT("coins"), 2);
	TestTrue(TEXT("source unchanged"), Source.ToJsonString().Contains(TEXT("\"coins\":1")));
	TestTrue(TEXT("derived updated"), Derived.ToJsonString().Contains(TEXT("\"coins\":2")));

	TestEqual(TEXT("value node parity (int)"),
		UFlockCommandDataLibrary::CommandValueToJsonString(UFlockCommandDataLibrary::CommandValueInt(7)),
		FFlockCommandValue(7).ToJsonString());
	TestEqual(TEXT("value node parity (string)"),
		UFlockCommandDataLibrary::CommandValueToJsonString(UFlockCommandDataLibrary::CommandValueString(TEXT("x"))),
		FFlockCommandValue(TEXT("x")).ToJsonString());
	TestEqual(TEXT("value node parity (bool)"),
		UFlockCommandDataLibrary::CommandValueToJsonString(UFlockCommandDataLibrary::CommandValueBool(true)),
		FFlockCommandValue(true).ToJsonString());

	return true;
}

#endif // WITH_AUTOMATION_TESTS
