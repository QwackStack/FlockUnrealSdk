// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Codegen/FlockStructBinder.h"
#include "Codegen/FlockStructEmitter.h"
#include "EdGraphSchema_K2.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockJsonData.h"
#include "Models/FlockStructuredData.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"

namespace FlockStructEmitterTestHelpers
{
	inline UUserDefinedStruct* Build(const FString& SchemaJson, TArray<FString>& Warnings,
		const FString& Name = TEXT("SpikeStruct"))
	{
		// Transient outer: these are throwaway assets, so nothing touches disk.
		return FFlockStructEmitter::BuildStruct(GetTransientPackage(),
			Name + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(6), SchemaJson, Warnings);
	}

	inline FProperty* FindMember(const UStruct* Struct, const TCHAR* Authored)
	{
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (FFlockStructBinder::GetMemberName(Struct, *It) == Authored)
			{
				return *It;
			}
		}
		return nullptr;
	}

	inline int32 MemberCount(const UStruct* Struct)
	{
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			++Count;
		}
		return Count;
	}

	inline FString Field(const TCHAR* Name, const TCHAR* Type)
	{
		return FString::Printf(TEXT("{\"type\":\"%s\",\"field_name\":\"%s\",\"type_name\":\"%s\"}"), Type, Name, Type);
	}
}

using namespace FlockStructEmitterTestHelpers;

// ── Every scalar the dashboard declares maps to the pin type a graph expects ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockStructEmitterScalarTest, "Flock.Editor.StructEmitter.MapsScalarTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockStructEmitterScalarTest::RunTest(const FString& Parameters)
{
	TArray<FString> Warnings;
	const FString Schema = FString::Printf(TEXT("[%s,%s,%s,%s,%s,%s]"),
		*Field(TEXT("level"), TEXT("int")),
		*Field(TEXT("score"), TEXT("long")),
		*Field(TEXT("ratio"), TEXT("float")),
		*Field(TEXT("title"), TEXT("string")),
		*Field(TEXT("flawless"), TEXT("bool")),
		*Field(TEXT("created"), TEXT("datetime")));

	UUserDefinedStruct* Struct = Build(Schema, Warnings);
	if (!TestNotNull(TEXT("struct built"), Struct))
	{
		return false;
	}

	TestEqual(TEXT("six members, no placeholder left"), MemberCount(Struct), 6);
	TestEqual(TEXT("no warnings"), Warnings.Num(), 0);

	TestTrue(TEXT("int"), CastField<FIntProperty>(FindMember(Struct, TEXT("level"))) != nullptr);
	TestTrue(TEXT("long"), CastField<FInt64Property>(FindMember(Struct, TEXT("score"))) != nullptr);
	TestTrue(TEXT("float"), CastField<FFloatProperty>(FindMember(Struct, TEXT("ratio"))) != nullptr);
	TestTrue(TEXT("string"), CastField<FStrProperty>(FindMember(Struct, TEXT("title"))) != nullptr);
	TestTrue(TEXT("bool"), CastField<FBoolProperty>(FindMember(Struct, TEXT("flawless"))) != nullptr);
	// Timestamps stay strings, matching CreatedAt/UpdatedAt on every hand-written model.
	TestTrue(TEXT("datetime is a string"), CastField<FStrProperty>(FindMember(Struct, TEXT("created"))) != nullptr);

	// A trailing '?' marks the field nullable on the dashboard and says nothing about its type. Real
	// backends emit these, and before they were handled every optional field degraded to a JSON handle.
	TArray<FString> NullableWarnings;
	UUserDefinedStruct* Nullable = Build(FString::Printf(TEXT("[%s,%s]"),
		*Field(TEXT("last_seen"), TEXT("datetime?")), *Field(TEXT("streak"), TEXT("int?"))),
		NullableWarnings, TEXT("Nullable"));
	if (TestNotNull(TEXT("nullable struct built"), Nullable))
	{
		TestTrue(TEXT("nullable datetime is still a string"),
			CastField<FStrProperty>(FindMember(Nullable, TEXT("last_seen"))) != nullptr);
		TestTrue(TEXT("nullable int is still an int"),
			CastField<FIntProperty>(FindMember(Nullable, TEXT("streak"))) != nullptr);
		TestEqual(TEXT("and nothing is reported"), NullableWarnings.Num(), 0);
	}

	return true;
}

// ── Members carry the declared name, so a write goes out in the spelling the server validates ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockStructEmitterNameTest, "Flock.Editor.StructEmitter.MembersUseDeclaredNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockStructEmitterNameTest::RunTest(const FString& Parameters)
{
	TArray<FString> Warnings;
	UUserDefinedStruct* Struct = Build(FString::Printf(TEXT("[%s]"), *Field(TEXT("game_currencies"), TEXT("int"))), Warnings);
	if (!TestNotNull(TEXT("struct built"), Struct))
	{
		return false;
	}

	// The snake_case declared name survives — not Pascal-cased into the flattened read spelling.
	TestNotNull(TEXT("member named as declared"), FindMember(Struct, TEXT("game_currencies")));
	TestNull(TEXT("not the flattened spelling"), FindMember(Struct, TEXT("GameCurrencies")));

	// Which is what makes a write come out right without any member->wire map.
	FStructOnScope Scope(Struct);
	FFlockStructuredData Row;
	Row.FlatJson = TEXT("{\"GameCurrencies\":250}"); // as a read hands it back
	TestEqual(TEXT("read binds via the spelling bridge"),
		FFlockStructBinder::FillStruct(Struct, Scope.GetStructMemory(), Row), 1);
	const FString Body = FFlockStructBinder::ToCommandData(Struct, Scope.GetStructMemory()).ToJsonString();
	TestTrue(TEXT("write uses the declared name"), Body.Contains(TEXT("\"game_currencies\":250")));
	TestFalse(TEXT("write does not use the read spelling"),
		Body.Contains(TEXT("\"GameCurrencies\""), ESearchCase::CaseSensitive));

	return true;
}

// ── A name that cannot be a member is skipped and reported, never silently renamed ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockStructEmitterBadNameTest, "Flock.Editor.StructEmitter.SkipsUnusableFieldNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockStructEmitterBadNameTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("snake_case is usable"), FFlockStructEmitter::IsUsableMemberName(TEXT("max_health")));
	TestTrue(TEXT("PascalCase is usable"), FFlockStructEmitter::IsUsableMemberName(TEXT("MaxHealth")));
	TestFalse(TEXT("a space is not"), FFlockStructEmitter::IsUsableMemberName(TEXT("max health")));
	TestFalse(TEXT("a dot is not"), FFlockStructEmitter::IsUsableMemberName(TEXT("max.health")));
	TestFalse(TEXT("a leading digit is not"), FFlockStructEmitter::IsUsableMemberName(TEXT("1st_place")));
	TestFalse(TEXT("empty is not"), FFlockStructEmitter::IsUsableMemberName(FString()));

	TArray<FString> Warnings;
	const FString Schema = FString::Printf(TEXT("[%s,{\"type\":\"int\",\"field_name\":\"bad name\"}]"),
		*Field(TEXT("good_name"), TEXT("int")));
	UUserDefinedStruct* Struct = Build(Schema, Warnings);
	if (!TestNotNull(TEXT("struct built"), Struct))
	{
		return false;
	}

	// The good field is there, the bad one is absent, and the designer is told which and why — renaming
	// it would produce writes the server rejects, which is worse than a missing field.
	TestEqual(TEXT("only the usable field"), MemberCount(Struct), 1);
	TestNotNull(TEXT("good field present"), FindMember(Struct, TEXT("good_name")));
	TestEqual(TEXT("one warning"), Warnings.Num(), 1);
	TestTrue(TEXT("warning names the field"), Warnings.Num() == 1 && Warnings[0].Contains(TEXT("bad name")));

	return true;
}

// ── Containers and nested objects become real typed pins ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockStructEmitterContainerTest, "Flock.Editor.StructEmitter.EmitsContainersAndNestedObjects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockStructEmitterContainerTest::RunTest(const FString& Parameters)
{
	TArray<FString> Warnings;
	const FString Schema =
		TEXT("[{\"type\":\"list\",\"field_name\":\"tags\",\"schema\":{\"type\":\"string\",\"field_name\":\"item\"}},")
		TEXT("{\"type\":\"dict\",\"field_name\":\"wallet\",\"schema\":{\"type\":\"int\",\"field_name\":\"value\"}},")
		TEXT("{\"type\":\"object\",\"field_name\":\"stats\",\"schema\":[")
		TEXT("{\"type\":\"int\",\"field_name\":\"hp\"},{\"type\":\"int\",\"field_name\":\"mp\"}]}]");

	UUserDefinedStruct* Struct = Build(Schema, Warnings, TEXT("Nested"));
	if (!TestNotNull(TEXT("struct built"), Struct))
	{
		return false;
	}
	TestEqual(TEXT("three members"), MemberCount(Struct), 3);
	TestEqual(TEXT("no warnings"), Warnings.Num(), 0);

	// list -> array pin
	FArrayProperty* Tags = CastField<FArrayProperty>(FindMember(Struct, TEXT("tags")));
	if (TestNotNull(TEXT("tags is an array"), Tags))
	{
		TestTrue(TEXT("array of strings"), CastField<FStrProperty>(Tags->Inner) != nullptr);
	}

	// dict -> map pin, string-keyed (dict keys are author data and always arrive as strings)
	FMapProperty* Wallet = CastField<FMapProperty>(FindMember(Struct, TEXT("wallet")));
	if (TestNotNull(TEXT("wallet is a map"), Wallet))
	{
		TestTrue(TEXT("string keys"), CastField<FStrProperty>(Wallet->KeyProp) != nullptr);
		TestTrue(TEXT("int values"), CastField<FIntProperty>(Wallet->ValueProp) != nullptr);
	}

	// object -> its own generated struct, so Break Struct works all the way down
	FStructProperty* Stats = CastField<FStructProperty>(FindMember(Struct, TEXT("stats")));
	if (TestNotNull(TEXT("stats is a struct"), Stats))
	{
		TestNotNull(TEXT("nested struct generated"), Stats->Struct.Get());
		TestEqual(TEXT("nested has both fields"), Stats->Struct ? MemberCount(Stats->Struct) : 0, 2);
	}

	return true;
}

// ── Shapes Blueprint cannot express degrade to the opaque handle rather than breaking the asset ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockStructEmitterFallbackTest, "Flock.Editor.StructEmitter.DegradesInexpressibleShapes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockStructEmitterFallbackTest::RunTest(const FString& Parameters)
{
	auto IsJsonHandle = [](FProperty* Property)
	{
		const FStructProperty* AsStruct = CastField<FStructProperty>(Property);
		return AsStruct && AsStruct->Struct == FFlockJsonData::StaticStruct();
	};

	// A list of lists: Unreal has no container-of-containers property.
	{
		TArray<FString> Warnings;
		UUserDefinedStruct* Struct = Build(
			TEXT("[{\"type\":\"list\",\"field_name\":\"grid\",\"schema\":{\"type\":\"list\",\"field_name\":\"row\",")
			TEXT("\"schema\":{\"type\":\"int\",\"field_name\":\"cell\"}}}]"), Warnings, TEXT("Grid"));
		if (TestNotNull(TEXT("struct built"), Struct))
		{
			TestTrue(TEXT("nested container becomes a JSON handle"), IsJsonHandle(FindMember(Struct, TEXT("grid"))));
			TestTrue(TEXT("and says so"), Warnings.Num() > 0);
		}
	}

	// An unknown type, and an object with no declared body.
	{
		TArray<FString> Warnings;
		UUserDefinedStruct* Struct = Build(
			TEXT("[{\"type\":\"quaternion\",\"field_name\":\"spin\"},{\"type\":\"object\",\"field_name\":\"blob\"}]"),
			Warnings, TEXT("Odd"));
		if (TestNotNull(TEXT("struct built"), Struct))
		{
			TestTrue(TEXT("unknown type becomes a JSON handle"), IsJsonHandle(FindMember(Struct, TEXT("spin"))));
			TestTrue(TEXT("bodyless object becomes a JSON handle"), IsJsonHandle(FindMember(Struct, TEXT("blob"))));
			TestEqual(TEXT("both reported"), Warnings.Num(), 2);
		}
	}

	// An empty or unreadable schema yields an empty struct plus a warning, never a null asset.
	{
		TArray<FString> Warnings;
		UUserDefinedStruct* Struct = Build(TEXT("{not json"), Warnings, TEXT("Broken"));
		TestNotNull(TEXT("struct still built"), Struct);
		TestTrue(TEXT("emptiness reported"), Warnings.Num() > 0);
	}

	return true;
}

// ── Asset names are stable, legal, and never collide ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockStructEmitterNamingTest, "Flock.Editor.StructEmitter.NamesStructsSafely",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockStructEmitterNamingTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("spaces become PascalCase"),
		FFlockStructEmitter::MakeStructName(TEXT("Player Progress"), TEXT("Template")), FString(TEXT("PlayerProgressTemplate")));
	TestEqual(TEXT("snake_case too"),
		FFlockStructEmitter::MakeStructName(TEXT("player_progress"), TEXT("Template")), FString(TEXT("PlayerProgressTemplate")));
	TestEqual(TEXT("already Pascal is unchanged"),
		FFlockStructEmitter::MakeStructName(TEXT("Gameplay"), TEXT("Config")), FString(TEXT("GameplayConfig")));
	// A leading digit is not a legal identifier, and an unnamed entity still needs a stable asset name.
	TestEqual(TEXT("leading digit is prefixed"),
		FFlockStructEmitter::MakeStructName(TEXT("2ndWind"), TEXT("Template")), FString(TEXT("_2ndWindTemplate")));
	TestEqual(TEXT("empty name is handled"),
		FFlockStructEmitter::MakeStructName(FString(), TEXT("Template")), FString(TEXT("UnnamedTemplate")));

	// Two entities that collapse to the same name must not overwrite each other's asset.
	FFlockSchemaSnapshot Snapshot;
	FFlockPlayerTemplateSchema First;
	First.Id = TEXT("a");
	First.Name = TEXT("Player Progress");
	First.SchemaJson = FString::Printf(TEXT("[%s]"), *Field(TEXT("level"), TEXT("int")));
	FFlockPlayerTemplateSchema Second = First;
	Second.Id = TEXT("b");
	Second.Name = TEXT("player_progress");
	Snapshot.PlayerTemplates = { First, Second };

	const FFlockStructEmitter::FEmitResult Result = FFlockStructEmitter::BuildAll(Snapshot, GetTransientPackage());
	TestEqual(TEXT("both structs built"), Result.StructCount, 2);
	TestEqual(TEXT("both recorded"), Result.StructNameById.Num(), 2);
	TestNotEqual(TEXT("colliding names are disambiguated"),
		Result.StructNameById[TEXT("a")], Result.StructNameById[TEXT("b")]);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
