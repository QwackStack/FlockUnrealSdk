// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Blueprint/FlockStructLibrary.h"
#include "Codegen/FlockStructBinder.h"
#include "Models/FlockAnalyticsModels.h"
#include "Models/FlockPlayerModels.h"
#include "Models/FlockStructuredData.h"

namespace FlockStructBinderTestHelpers
{
	/**
	 * FFlockDeviceInfo stands in for a generated struct: it mixes FString, int32 and float members, which
	 * is the type spread a template schema produces. Using a real SDK struct keeps a test-only USTRUCT out
	 * of the shipping module; the Blueprint-struct flavour is covered by Flock.Editor.CodegenSpike.
	 */
	inline FFlockStructuredData DataFrom(const FString& FlatJson)
	{
		FFlockStructuredData Data;
		Data.FlatJson = FlatJson;
		return Data;
	}
}

using namespace FlockStructBinderTestHelpers;

// ── Members bind by name, keeping their types ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockBinderFillTest, "Flock.Codegen.Binder.FillsByMemberName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockBinderFillTest::RunTest(const FString& Parameters)
{
	FFlockDeviceInfo Info;
	const int32 Bound = FFlockStructBinder::FillStruct(FFlockDeviceInfo::StaticStruct(), &Info,
		DataFrom(TEXT("{\"Platform\":\"Windows\",\"ScreenWidth\":1920,\"ScreenDpi\":141.5}")));

	TestEqual(TEXT("three members bound"), Bound, 3);
	TestEqual(TEXT("string member"), Info.Platform, FString(TEXT("Windows")));
	TestEqual(TEXT("int member"), Info.ScreenWidth, 1920);
	TestEqual(TEXT("float member"), Info.ScreenDpi, 141.5f);
	// Absent fields are left alone rather than zeroed, so a partial row can't wipe defaults.
	TestEqual(TEXT("absent member untouched"), Info.ScreenHeight, 0);

	return true;
}

// ── The spelling split: a wire name and a member name that disagree still bind, both directions ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockBinderSpellingTest, "Flock.Codegen.Binder.BridgesSnakeAndPascalSpellings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockBinderSpellingTest::RunTest(const FString& Parameters)
{
	// A legacy flat row (or a dict-typed field) keeps author keys verbatim, so the source is snake_case
	// while the member is Pascal. This is precisely the case the stock JSON converter does not handle.
	FFlockDeviceInfo Snake;
	TestEqual(TEXT("snake_case source binds"),
		FFlockStructBinder::FillStruct(FFlockDeviceInfo::StaticStruct(), &Snake,
			DataFrom(TEXT("{\"screen_width\":800,\"system_memory_mb\":4096,\"device_model\":\"Pixel\"}"))),
		3);
	TestEqual(TEXT("int via snake_case"), Snake.ScreenWidth, 800);
	TestEqual(TEXT("second int via snake_case"), Snake.SystemMemoryMb, 4096);
	TestEqual(TEXT("string via snake_case"), Snake.DeviceModel, FString(TEXT("Pixel")));

	// And the ordinary flattened case, where the source is already Pascal.
	FFlockDeviceInfo Pascal;
	TestEqual(TEXT("PascalCase source binds"),
		FFlockStructBinder::FillStruct(FFlockDeviceInfo::StaticStruct(), &Pascal,
			DataFrom(TEXT("{\"ScreenWidth\":1280}"))),
		1);
	TestEqual(TEXT("int via PascalCase"), Pascal.ScreenWidth, 1280);

	return true;
}

// ── Nothing to bind is a clean zero, not a crash or a half-filled struct ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockBinderEmptyTest, "Flock.Codegen.Binder.EmptyAndUnknownDegradeCleanly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockBinderEmptyTest::RunTest(const FString& Parameters)
{
	FFlockDeviceInfo Info;
	Info.Platform = TEXT("untouched");

	TestEqual(TEXT("empty data binds nothing"),
		FFlockStructBinder::FillStruct(FFlockDeviceInfo::StaticStruct(), &Info, FFlockStructuredData()), 0);
	TestEqual(TEXT("unknown fields bind nothing"),
		FFlockStructBinder::FillStruct(FFlockDeviceInfo::StaticStruct(), &Info,
			DataFrom(TEXT("{\"NotAMember\":1,\"AlsoNot\":\"x\"}"))), 0);
	TestEqual(TEXT("existing value survives"), Info.Platform, FString(TEXT("untouched")));

	// A null-valued field is "no value", not a bind — the server sends null for an unset field.
	TestEqual(TEXT("null value binds nothing"),
		FFlockStructBinder::FillStruct(FFlockDeviceInfo::StaticStruct(), &Info,
			DataFrom(TEXT("{\"ScreenWidth\":null}"))), 0);

	// Null struct / null memory are caller errors that must not crash the game.
	TestEqual(TEXT("null struct"), FFlockStructBinder::FillStruct(nullptr, &Info, DataFrom(TEXT("{\"ScreenWidth\":1}"))), 0);
	TestEqual(TEXT("null memory"), FFlockStructBinder::FillStruct(FFlockDeviceInfo::StaticStruct(), nullptr, DataFrom(TEXT("{}"))), 0);

	return true;
}

// ── Write-back keys off the member name, with types preserved ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockBinderToCommandTest, "Flock.Codegen.Binder.ReadsBackIntoCommandData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockBinderToCommandTest::RunTest(const FString& Parameters)
{
	FFlockDeviceInfo Info;
	Info.Platform = TEXT("Windows");
	Info.ScreenWidth = 1920;
	Info.ScreenDpi = 141.5f;

	const FFlockCommandData Body = FFlockStructBinder::ToCommandData(FFlockDeviceInfo::StaticStruct(), &Info);
	const FString Json = Body.ToJsonString();

	TestTrue(TEXT("string stays a string"), Json.Contains(TEXT("\"Platform\":\"Windows\"")));
	TestTrue(TEXT("int stays an int"), Json.Contains(TEXT("\"ScreenWidth\":1920")));
	TestTrue(TEXT("float stays a number"), Json.Contains(TEXT("\"ScreenDpi\":141.5")));
	// Every member is written, so an update is a full statement of the struct rather than a diff.
	TestEqual(TEXT("every member written"), Body.GetFieldNames().Num(), 12);

	return true;
}

// ── A declared-name map renames on the way out; that is how the generated C++ tier writes ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockBinderDeclaredNameTest, "Flock.Codegen.Binder.WritesDeclaredNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockBinderDeclaredNameTest::RunTest(const FString& Parameters)
{
	FFlockDeviceInfo Info;
	Info.ScreenWidth = 1920;
	Info.Platform = TEXT("Windows");

	const TMap<FString, FString> Declared =
	{
		{ TEXT("ScreenWidth"), TEXT("screen_width") },
		{ TEXT("Platform"), TEXT("platform") },
	};
	const FString Json = FFlockStructBinder::ToCommandData(FFlockDeviceInfo::StaticStruct(), &Info, Declared).ToJsonString();

	TestTrue(TEXT("mapped member uses the declared name"), Json.Contains(TEXT("\"screen_width\":1920")));
	TestTrue(TEXT("second mapped member"), Json.Contains(TEXT("\"platform\":\"Windows\"")));
	// Case-sensitive: FString::Contains ignores case by default, which would match the declared spelling.
	TestFalse(TEXT("member name did not leak"), Json.Contains(TEXT("\"ScreenWidth\""), ESearchCase::CaseSensitive));
	// An unmapped member keeps its own name rather than being dropped.
	TestTrue(TEXT("unmapped member kept"), Json.Contains(TEXT("\"DeviceModel\""), ESearchCase::CaseSensitive));

	return true;
}

// ── Round trip: fill, mutate, read back — the shape the generated accessors expose ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockBinderRoundTripTest, "Flock.Codegen.Binder.FetchMutateUpdateRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockBinderRoundTripTest::RunTest(const FString& Parameters)
{
	FFlockDeviceInfo Info;
	FFlockStructBinder::FillStruct(FFlockDeviceInfo::StaticStruct(), &Info,
		DataFrom(TEXT("{\"Platform\":\"Windows\",\"ScreenWidth\":1920}")));

	Info.ScreenWidth = 2560; // "change field"

	const FString Json = FFlockStructBinder::ToCommandData(FFlockDeviceInfo::StaticStruct(), &Info).ToJsonString();
	TestTrue(TEXT("mutation carried"), Json.Contains(TEXT("\"ScreenWidth\":2560")));
	TestTrue(TEXT("untouched member carried"), Json.Contains(TEXT("\"Platform\":\"Windows\"")));

	return true;
}

// ── The wildcard nodes are registered as wildcards; a typo in the meta key would silently un-wildcard them ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockStructLibraryWildcardTest, "Flock.Codegen.Binder.BlueprintNodesAreWildcards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockStructLibraryWildcardTest::RunTest(const FString& Parameters)
{
	UClass* LibraryClass = UFlockStructLibrary::StaticClass();

	UFunction* ToStruct = LibraryClass->FindFunctionByName(TEXT("DataToStruct"));
	UFunction* ToCommand = LibraryClass->FindFunctionByName(TEXT("StructToCommandData"));
	if (!TestNotNull(TEXT("Data To Struct exists"), ToStruct) || !TestNotNull(TEXT("Struct To Command Data exists"), ToCommand))
	{
		return false;
	}

	// A CustomThunk function whose CustomStructureParam is misspelled still compiles and still runs — it
	// just presents a plain int32 pin, which no generated struct can connect to. Hence asserting the meta.
	TestTrue(TEXT("Data To Struct is a custom thunk"), ToStruct->HasAnyFunctionFlags(FUNC_Native));
	TestTrue(TEXT("Struct To Command Data is a custom thunk"), ToCommand->HasAnyFunctionFlags(FUNC_Native));

#if WITH_EDITORONLY_DATA
	TestEqual(TEXT("Data To Struct wildcard pin"),
		ToStruct->GetMetaData(TEXT("CustomStructureParam")), FString(TEXT("OutStruct")));
	TestEqual(TEXT("Struct To Command Data wildcard pin"),
		ToCommand->GetMetaData(TEXT("CustomStructureParam")), FString(TEXT("Struct")));
#endif

	return true;
}


// ── Nested member names and dict keys go out exactly as authored ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockBinderNestedNamesTest, "Flock.Codegen.Binder.KeepsNestedNamesVerbatim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockBinderNestedNamesTest::RunTest(const FString& Parameters)
{
	// FFlockPlayerBan carries both shapes the JSON converter renames behind our back: a TMap whose keys
	// are author data, and a nested struct whose members are Pascal-cased. A template declaring an object
	// field looks exactly like this once generated.
	FFlockPlayerBan Ban;
	Ban.Id = TEXT("ban-1");
	FFlockFeatureBan Feature;
	Feature.Reason = TEXT("cheating");
	Feature.BanDuration = TEXT("7d");
	Ban.Data.Add(TEXT("Leaderboard"), Feature);

	const FFlockCommandData Data = FFlockStructBinder::ToCommandData(FFlockPlayerBan::StaticStruct(), &Ban);
	const FString Json = Data.ToJsonString();
	AddInfo(Json);

	// Every check below is CaseSensitive on purpose. FString::Contains ignores case by default, which
	// makes each of these assertions pass whether or not the fix is present — the exact shape of vacuous
	// test that let this ship in the first place.
	auto Has = [&Json](const TCHAR* Needle) { return Json.Contains(Needle, ESearchCase::CaseSensitive); };

	// The bug this pins: the converter lower-cases the first letter of every name it writes, so a nested
	// `Reason` went out as `reason` and the server rejected the write for a missing required property that
	// was, as far as the graph was concerned, set.
	TestTrue(TEXT("a nested member keeps its authored case"), Has(TEXT("\"Reason\"")));
	TestFalse(TEXT("and is not lower-cased"), Has(TEXT("\"reason\"")));
	TestTrue(TEXT("a multi-word nested member survives too"), Has(TEXT("\"BanDuration\"")));

	// Dict keys are author data and must never be transformed — the same rule the flatten follows on read.
	TestTrue(TEXT("a dict key keeps its authored case"), Has(TEXT("\"Leaderboard\"")));
	TestFalse(TEXT("and is not lower-cased"), Has(TEXT("\"leaderboard\"")));

	// The top level was never affected, because the binder names those keys itself. Asserted so a future
	// change to the flag cannot quietly move the problem up a level.
	TestTrue(TEXT("top-level members are unchanged"), Has(TEXT("\"Id\"")));

	return true;
}


// -- A registered wire name maps a Pascal member to its declared name, both directions --
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockBinderWireNameTest, "Flock.Codegen.Binder.HonoursRegisteredWireNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockBinderWireNameTest::RunTest(const FString& Parameters)
{
	// FFlockFeatureBan stands in for a generated C++ struct: its members are Pascal (`BanDuration`) while
	// a template would declare them snake (`ban_duration`). Registering that mapping is exactly what the
	// generated module does on startup.
	FFlockStructBinder::RegisterWireNames(FFlockFeatureBan::StaticStruct(), {
		{ TEXT("Reason"), TEXT("why_banned") },
		{ TEXT("BanDuration"), TEXT("ban_duration") },
	});

	// Write: the declared name goes out, not the member's own spelling. Without this the server rejects
	// the write for a property the caller can see is set.
	FFlockFeatureBan Ban;
	Ban.Reason = TEXT("cheating");
	Ban.BanDuration = TEXT("7d");
	const FString Json = FFlockStructBinder::ToCommandData(FFlockFeatureBan::StaticStruct(), &Ban).ToJsonString();
	AddInfo(Json);

	auto Has = [&Json](const TCHAR* Needle) { return Json.Contains(Needle, ESearchCase::CaseSensitive); };
	TestTrue(TEXT("the declared name is written"), Has(TEXT("\"why_banned\"")));
	TestFalse(TEXT("and the member name is not"), Has(TEXT("\"Reason\"")));
	TestTrue(TEXT("a snake declared name too"), Has(TEXT("\"ban_duration\"")));

	// Read: the same mapping resolves the other way, so a fetch fills the Pascal member from the
	// declared key. A one-directional map would bind nothing and look exactly like an absent field.
	FFlockFeatureBan RoundTrip;
	FFlockStructuredData Source;
	Source.FlatJson = TEXT("{\"why_banned\":\"botting\",\"ban_duration\":\"30d\"}");
	FFlockStructBinder::FillStruct(FFlockFeatureBan::StaticStruct(), &RoundTrip, Source);
	TestEqual(TEXT("the member fills from the declared key"), RoundTrip.Reason, FString(TEXT("botting")));
	TestEqual(TEXT("including a snake one"), RoundTrip.BanDuration, FString(TEXT("30d")));

	// Unregistering restores the fallback: members are their own wire names, which is the Blueprint tier.
	FFlockStructBinder::UnregisterWireNames(FFlockFeatureBan::StaticStruct());
	const FString Plain = FFlockStructBinder::ToCommandData(FFlockFeatureBan::StaticStruct(), &Ban).ToJsonString();
	TestTrue(TEXT("an unregistered struct writes its member names"),
		Plain.Contains(TEXT("\"Reason\""), ESearchCase::CaseSensitive));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
