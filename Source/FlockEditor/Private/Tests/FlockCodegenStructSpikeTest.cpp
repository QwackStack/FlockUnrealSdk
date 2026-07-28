// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "EdGraphSchema_K2.h"
#include "Http/FlockJsonUtils.h"
#include "JsonObjectConverter.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockStructuredData.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"

/**
 * Codegen spike: can a Blueprint-authored struct (UUserDefinedStruct) stand in for a generated USTRUCT,
 * so a Blueprint-only project gets typed pins with no C++ compile?
 *
 * The whole Blueprint tier of codegen rests on three answers:
 *   1. Can the editor build one of these programmatically with typed members?
 *   2. Its properties are NOT named as authored — they carry a generated GUID suffix — so can the
 *      authored name still be recovered at runtime, to match a wire field to a member?
 *   3. Can an instance be filled from a row's data and read back into a write body keyed by the
 *      template's declared names (the read/write asymmetry codegen exists to erase)?
 *
 * If all three hold, "fetch object -> change field -> update" works in Blueprint exactly as it does in
 * C++. These tests are the pin for that; they are not testing SDK behaviour, they are testing the engine
 * mechanism the design depends on.
 */
namespace FlockCodegenSpike
{
	/** Adds a member of the given pin type and renames it to the authored name codegen would use. */
	bool AddNamedMember(UUserDefinedStruct* Struct, const FEdGraphPinType& PinType, const FString& AuthoredName)
	{
		const TArray<FStructVariableDescription>& Before = FStructureEditorUtils::GetVarDesc(Struct);
		const int32 CountBefore = Before.Num();
		if (!FStructureEditorUtils::AddVariable(Struct, PinType))
		{
			return false;
		}
		const TArray<FStructVariableDescription>& After = FStructureEditorUtils::GetVarDesc(Struct);
		if (After.Num() != CountBefore + 1)
		{
			return false;
		}
		return FStructureEditorUtils::RenameVariable(Struct, After.Last().VarGuid, AuthoredName);
	}

	FEdGraphPinType IntPin()
	{
		FEdGraphPinType Pin;
		Pin.PinCategory = UEdGraphSchema_K2::PC_Int;
		return Pin;
	}

	FEdGraphPinType StringPin()
	{
		FEdGraphPinType Pin;
		Pin.PinCategory = UEdGraphSchema_K2::PC_String;
		return Pin;
	}

	FEdGraphPinType BoolPin()
	{
		FEdGraphPinType Pin;
		Pin.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		return Pin;
	}

	/**
	 * Builds a throwaway struct shaped like a generated player template. The first member is the one
	 * CreateUserDefinedStruct seeds automatically, renamed rather than added.
	 */
	UUserDefinedStruct* MakeTemplateStruct()
	{
		UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(
			GetTransientPackage(), TEXT("FlockSpike_PlayerProgress"), RF_Transient | RF_Public);
		if (!Struct)
		{
			return nullptr;
		}

		// The seeded member becomes "Level"; the rest are added.
		const TArray<FStructVariableDescription>& Seeded = FStructureEditorUtils::GetVarDesc(Struct);
		if (Seeded.Num() != 1
			|| !FStructureEditorUtils::ChangeVariableType(Struct, Seeded[0].VarGuid, IntPin())
			|| !FStructureEditorUtils::RenameVariable(Struct, Seeded[0].VarGuid, TEXT("Level")))
		{
			return nullptr;
		}
		if (!AddNamedMember(Struct, IntPin(), TEXT("Xp"))
			|| !AddNamedMember(Struct, StringPin(), TEXT("Title"))
			|| !AddNamedMember(Struct, BoolPin(), TEXT("Flawless")))
		{
			return nullptr;
		}
		return Struct;
	}

	/**
	 * The reflective filler the Blueprint tier would ship at runtime: match each member to a wire field by
	 * its **authored** name and convert through the JSON converter so every pin type is handled.
	 *
	 * The source is indexed under both its verbatim key and that key's Pascal form, because the transform
	 * runs the other way round from how it first looks: the source carries the wire spelling
	 * ("max_health") and the member carries the Pascal one ("MaxHealth"), so it is the *source key* that
	 * has to be Pascal-cased, not the member name. Getting that backwards silently binds nothing.
	 */
	int32 FillFromJson(const UStruct* Struct, void* StructMemory, const TSharedRef<FJsonObject>& Source)
	{
		TMap<FString, TSharedPtr<FJsonValue>> ByName;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
		{
			ByName.Add(Pair.Key, Pair.Value);
			const FString Pascal = FFlockJsonUtils::SnakeToPascal(Pair.Key);
			if (!ByName.Contains(Pascal))
			{
				ByName.Add(Pascal, Pair.Value);
			}
		}

		int32 Filled = 0;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Property = *It;
			const TSharedPtr<FJsonValue>* Value = ByName.Find(Struct->GetAuthoredNameForField(Property));
			if (!Value || !Value->IsValid())
			{
				continue;
			}
			if (FJsonObjectConverter::JsonValueToUProperty(*Value, Property, Property->ContainerPtrToValuePtr<void>(StructMemory)))
			{
				++Filled;
			}
		}
		return Filled;
	}

	/** Reads one int member back by authored name; -1 when absent. */
	int32 ReadInt(const UStruct* Struct, const void* StructMemory, const TCHAR* Authored)
	{
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (Struct->GetAuthoredNameForField(*It) == Authored)
			{
				if (const FIntProperty* IntProp = CastField<FIntProperty>(*It))
				{
					return IntProp->GetPropertyValue_InContainer(StructMemory);
				}
			}
		}
		return -1;
	}

	/**
	 * The reverse: read the instance back into a command bag keyed by the template's DECLARED names.
	 * DeclaredByAuthored maps the struct's authored member name to the wire name the server validates
	 * against ("Level" -> "level"), which is what codegen bakes in.
	 */
	FFlockCommandData ToCommandData(const UStruct* Struct, const void* StructMemory,
		const TMap<FString, FString>& DeclaredByAuthored)
	{
		FFlockCommandData Data;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Property = *It;
			const FString Authored = Struct->GetAuthoredNameForField(Property);
			const FString* Declared = DeclaredByAuthored.Find(Authored);
			const TSharedPtr<FJsonValue> Value =
				FJsonObjectConverter::UPropertyToJsonValue(Property, Property->ContainerPtrToValuePtr<void>(StructMemory));
			if (Value.IsValid())
			{
				Data.Set(Declared ? *Declared : Authored, FFlockCommandValue::FromJsonValue(Value));
			}
		}
		return Data;
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
		return (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid()) ? Parsed : nullptr;
	}
}

using namespace FlockCodegenSpike;

// ── Q1: the editor can build a typed struct programmatically ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSpikeCreateStructTest, "Flock.Editor.CodegenSpike.CreatesTypedStruct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSpikeCreateStructTest::RunTest(const FString& Parameters)
{
	UUserDefinedStruct* Struct = MakeTemplateStruct();
	if (!TestNotNull(TEXT("struct created"), Struct))
	{
		return false;
	}

	int32 PropertyCount = 0;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		++PropertyCount;
	}
	TestEqual(TEXT("four typed members"), PropertyCount, 4);

	// The member types are what drive Blueprint's pin types, so assert them rather than just the count.
	TMap<FString, FString> TypeByAuthored;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		TypeByAuthored.Add(Struct->GetAuthoredNameForField(*It), It->GetClass()->GetName());
	}
	TestEqual(TEXT("Level is an int"), TypeByAuthored.FindRef(TEXT("Level")), FString(TEXT("IntProperty")));
	TestEqual(TEXT("Xp is an int"), TypeByAuthored.FindRef(TEXT("Xp")), FString(TEXT("IntProperty")));
	TestEqual(TEXT("Title is a string"), TypeByAuthored.FindRef(TEXT("Title")), FString(TEXT("StrProperty")));
	TestEqual(TEXT("Flawless is a bool"), TypeByAuthored.FindRef(TEXT("Flawless")), FString(TEXT("BoolProperty")));

	return true;
}

// ── Q2: the authored name survives the GUID mangling, and the stock converter does NOT see it ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSpikeAuthoredNameTest, "Flock.Editor.CodegenSpike.AuthoredNameIsRecoverable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSpikeAuthoredNameTest::RunTest(const FString& Parameters)
{
	UUserDefinedStruct* Struct = MakeTemplateStruct();
	if (!TestNotNull(TEXT("struct created"), Struct))
	{
		return false;
	}

	bool bFoundLevel = false;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		const FString Raw = It->GetName();
		const FString Authored = Struct->GetAuthoredNameForField(*It);
		if (Authored == TEXT("Level"))
		{
			bFoundLevel = true;
			// This is the whole risk the spike exists to settle: the raw name is mangled, so any
			// name-matching that uses it (JsonObjectToUStruct does) cannot bind a wire field.
			AddInfo(FString::Printf(TEXT("raw property name = '%s', authored = '%s'"), *Raw, *Authored));
			TestNotEqual(TEXT("raw name is mangled, not the authored one"), Raw, Authored);
			TestTrue(TEXT("raw name still starts with the authored name"), Raw.StartsWith(Authored));
		}
	}
	TestTrue(TEXT("found the Level member by its authored name"), bFoundLevel);

	// The stock converter turns out to handle the mangling itself: it binds authored-name JSON despite the
	// GUID-suffixed property names. That removes the reason to hand-roll the Pascal-name path.
	const TSharedPtr<FJsonObject> Pascal = ParseObject(TEXT("{\"Level\":5,\"Xp\":1200}"));
	if (!TestTrue(TEXT("fixture parses"), Pascal.IsValid()))
	{
		return false;
	}
	FStructOnScope PascalScope(Struct);
	TestTrue(TEXT("stock converter accepts a user-defined struct"), FJsonObjectConverter::JsonObjectToUStruct(
		Pascal.ToSharedRef(), Struct, PascalScope.GetStructMemory(), 0, 0));
	TestEqual(TEXT("stock converter binds by AUTHORED name, mangling and all"),
		ReadInt(Struct, PascalScope.GetStructMemory(), TEXT("Level")), 5);

	// Where it stops: a wire spelling that differs from the member's. The flatten Pascal-cases DataField
	// names, but a legacy flat row (or a dict-typed field) keeps author keys verbatim, so this case is
	// real — and it is the only reason the tier ships its own filler rather than calling the stock one.
	const TSharedPtr<FJsonObject> Snake = ParseObject(TEXT("{\"level\":5,\"xp\":1200}"));
	FStructOnScope SnakeScope(Struct);
	FJsonObjectConverter::JsonObjectToUStruct(Snake.ToSharedRef(), Struct, SnakeScope.GetStructMemory(), 0, 0);
	AddInfo(FString::Printf(TEXT("stock converter against lowercase wire names set Level=%d"),
		ReadInt(Struct, SnakeScope.GetStructMemory(), TEXT("Level"))));

	return true;
}

// ── Q3: fetch -> change -> update round-trips, with declared names on the way out ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSpikeRoundTripTest, "Flock.Editor.CodegenSpike.FillMutateAndWriteBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSpikeRoundTripTest::RunTest(const FString& Parameters)
{
	UUserDefinedStruct* Struct = MakeTemplateStruct();
	if (!TestNotNull(TEXT("struct created"), Struct))
	{
		return false;
	}

	// "Fetch": a row's flattened data, exactly as FFlockStructuredData would hand it over. The wire
	// declares snake_case; the flatten Pascal-cases — both spellings must bind.
	FFlockStructuredData RowData;
	RowData.FlatJson = TEXT("{\"Level\":5,\"Xp\":1200,\"Title\":\"Champion\",\"Flawless\":true}");
	const TSharedPtr<FJsonObject> Source = ParseObject(RowData.ToJsonString());
	if (!TestTrue(TEXT("row data parses"), Source.IsValid()))
	{
		return false;
	}

	FStructOnScope Scope(Struct);
	const int32 Filled = FillFromJson(Struct, Scope.GetStructMemory(), Source.ToSharedRef());
	TestEqual(TEXT("every member bound"), Filled, 4);

	// Read the filled values back out to prove the fill was real.
	auto FindProperty = [Struct](const TCHAR* Authored) -> FProperty*
	{
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (Struct->GetAuthoredNameForField(*It) == Authored)
			{
				return *It;
			}
		}
		return nullptr;
	};

	FIntProperty* LevelProp = CastField<FIntProperty>(FindProperty(TEXT("Level")));
	FStrProperty* TitleProp = CastField<FStrProperty>(FindProperty(TEXT("Title")));
	FBoolProperty* FlawlessProp = CastField<FBoolProperty>(FindProperty(TEXT("Flawless")));
	if (!TestNotNull(TEXT("Level property"), LevelProp) || !TestNotNull(TEXT("Title property"), TitleProp)
		|| !TestNotNull(TEXT("Flawless property"), FlawlessProp))
	{
		return false;
	}

	TestEqual(TEXT("Level filled"), LevelProp->GetPropertyValue_InContainer(Scope.GetStructMemory()), 5);
	TestEqual(TEXT("Title filled"), TitleProp->GetPropertyValue_InContainer(Scope.GetStructMemory()), FString(TEXT("Champion")));
	TestTrue(TEXT("Flawless filled"), FlawlessProp->GetPropertyValue_InContainer(Scope.GetStructMemory()));

	// "Change field" — the middle step of the target ergonomics.
	LevelProp->SetPropertyValue_InContainer(Scope.GetStructMemory(), 6);

	// "Update": back to a command bag keyed by the template's declared (wire) names, which is the
	// asymmetry codegen exists to hide from the caller.
	const TMap<FString, FString> DeclaredByAuthored =
	{
		{ TEXT("Level"), TEXT("level") },
		{ TEXT("Xp"), TEXT("xp") },
		{ TEXT("Title"), TEXT("title") },
		{ TEXT("Flawless"), TEXT("flawless") },
	};
	const FFlockCommandData Body = ToCommandData(Struct, Scope.GetStructMemory(), DeclaredByAuthored);
	const FString Json = Body.ToJsonString();
	AddInfo(FString::Printf(TEXT("write body = %s"), *Json));

	TestEqual(TEXT("all four fields written"), Body.GetFieldNames().Num(), 4);
	TestTrue(TEXT("mutated value carried, under the DECLARED name"), Json.Contains(TEXT("\"level\":6")));
	TestTrue(TEXT("int stays an int"), Json.Contains(TEXT("\"xp\":1200")));
	TestTrue(TEXT("string stays a string"), Json.Contains(TEXT("\"title\":\"Champion\"")));
	TestTrue(TEXT("bool stays a bool"), Json.Contains(TEXT("\"flawless\":true")));
	// Case-sensitive on purpose: FString::Contains ignores case by default, which would match the
	// declared "level" and make this assertion pass for the wrong reason.
	TestFalse(TEXT("no authored name leaked into the body"),
		Json.Contains(TEXT("\"Level\""), ESearchCase::CaseSensitive));

	return true;
}

// ── The snake_case half of the binding: a template declaring snake_case still fills a Pascal member ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSpikeSnakeBindingTest, "Flock.Editor.CodegenSpike.BindsSnakeCaseWireNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSpikeSnakeBindingTest::RunTest(const FString& Parameters)
{
	UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(
		GetTransientPackage(), TEXT("FlockSpike_Wallet"), RF_Transient | RF_Public);
	if (!TestNotNull(TEXT("struct created"), Struct))
	{
		return false;
	}
	const TArray<FStructVariableDescription>& Seeded = FStructureEditorUtils::GetVarDesc(Struct);
	TestTrue(TEXT("seeded member retyped"), FStructureEditorUtils::ChangeVariableType(Struct, Seeded[0].VarGuid, IntPin()));
	TestTrue(TEXT("seeded member renamed"), FStructureEditorUtils::RenameVariable(Struct, Seeded[0].VarGuid, TEXT("MaxHealth")));

	// A legacy/dict row can carry the wire spelling verbatim; the member is PascalCase either way.
	const TSharedPtr<FJsonObject> Source = ParseObject(TEXT("{\"max_health\":250}"));
	if (!TestTrue(TEXT("fixture parses"), Source.IsValid()))
	{
		return false;
	}
	FStructOnScope Scope(Struct);
	TestEqual(TEXT("snake_case source bound the Pascal member"),
		FillFromJson(Struct, Scope.GetStructMemory(), Source.ToSharedRef()), 1);
	TestEqual(TEXT("value bound"), ReadInt(Struct, Scope.GetStructMemory(), TEXT("MaxHealth")), 250);

	// The stock converter is the thing that cannot do this, which is what justifies the custom filler.
	FStructOnScope StockScope(Struct);
	FJsonObjectConverter::JsonObjectToUStruct(Source.ToSharedRef(), Struct, StockScope.GetStructMemory(), 0, 0);
	AddInfo(FString::Printf(TEXT("stock converter against the same snake_case source set MaxHealth=%d"),
		ReadInt(Struct, StockScope.GetStructMemory(), TEXT("MaxHealth"))));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
