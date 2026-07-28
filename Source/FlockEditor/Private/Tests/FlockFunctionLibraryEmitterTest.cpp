// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Codegen/FlockFunctionLibraryEmitter.h"
#include "Codegen/FlockStructBinder.h"
#include "Codegen/FlockEnumEmitter.h"
#include "Codegen/FlockStructEmitter.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "Engine/UserDefinedEnum.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockStructuredData.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h"

namespace FlockFunctionLibraryEmitterTestHelpers
{
	inline FFlockPlayerTemplateSchema Template(const FString& Id, const FString& Name)
	{
		FFlockPlayerTemplateSchema Result;
		Result.Id = Id;
		Result.Name = Name;
		return Result;
	}

	inline FFlockSchemaSnapshot Snapshot()
	{
		FFlockSchemaSnapshot Result;
		Result.GameVersionId = TEXT("ver-1");
		Result.PlayerTemplates.Add(Template(TEXT("tmpl-1"), TEXT("Wallet")));
		Result.PlayerTemplates.Add(Template(TEXT("tmpl-2"), TEXT("player progress")));

		FFlockGameConfigSchema Config;
		Config.Id = TEXT("cfg-1");
		Config.Name = TEXT("Gameplay");
		Result.GameConfigs.Add(Config);
		return Result;
	}

	/**
	 * A fresh outer per test. The library asset has a fixed name, so tests sharing one package would build
	 * on top of each other's output — and the re-sync path is worth exercising deliberately (see
	 * ReSyncReplacesFunctions) rather than by accident.
	 */
	inline UPackage* FreshOuter()
	{
		return CreatePackage(*FString::Printf(TEXT("/Temp/FlockLibTest_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	/**
	 * Calls a generated function by reflection. This is the point of compiling in the emitter: a generated
	 * graph is verified by *running* it, not by inspecting its nodes — node inspection would pass for a
	 * graph the compiler rejects.
	 */
	inline bool CallStringFunction(UBlueprint* Library, const FString& FunctionName, FString& OutValue,
		FString* OutSignature = nullptr)
	{
		OutValue.Reset();
		if (!Library || !Library->GeneratedClass)
		{
			return false;
		}
		UFunction* Function = Library->GeneratedClass->FindFunctionByName(FName(*FunctionName));
		if (!Function)
		{
			return false;
		}

		// The parameter frame is built from the function's own properties rather than a hand-declared
		// struct: a mismatched struct would read the wrong memory and look exactly like a wrong return
		// value, which is not a diagnosis anyone should have to make twice.
		uint8* Frame = static_cast<uint8*>(FMemory::Malloc(FMath::Max<int32>(Function->ParmsSize, 1)));
		FMemory::Memzero(Frame, FMath::Max<int32>(Function->ParmsSize, 1));
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(Frame);
			if (OutSignature)
			{
				OutSignature->Append(FString::Printf(TEXT("%s %s (flags 0x%llx); "),
					*It->GetClass()->GetName(), *It->GetName(), static_cast<uint64>(It->PropertyFlags)));
			}
		}

		Library->GeneratedClass->GetDefaultObject()->ProcessEvent(Function, Frame);

		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm))
			{
				if (const FStrProperty* AsString = CastField<FStrProperty>(*It))
				{
					OutValue = AsString->GetPropertyValue_InContainer(Frame);
				}
			}
		}

		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Frame);
		}
		FMemory::Free(Frame);
		return true;
	}
}

using namespace FlockFunctionLibraryEmitterTestHelpers;

// ── The generated library compiles and its functions return the baked ids ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLibraryCompilesTest, "Flock.Editor.FunctionLibrary.CompilesAndReturnsBakedIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLibraryCompilesTest::RunTest(const FString& Parameters)
{
	const FFlockFunctionLibraryEmitter::FEmitResult Result =
		FFlockFunctionLibraryEmitter::BuildLibrary(Snapshot(), FreshOuter());

	if (!TestTrue(TEXT("library built"), Result.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("three functions"), Result.FunctionCount, 3);
	TestEqual(TEXT("no warnings"), Result.Warnings.Num(), 0);

	// A graph that does not compile has no generated class, so nothing below could run.
	TestTrue(TEXT("compiled without errors"), Result.Library->Status != BS_Error);
	TestNotNull(TEXT("generated class exists"), Result.Library->GeneratedClass.Get());

	// Running the function is the real assertion: it proves the graph, the pure flag, the result pin, and
	// its default value all came out right.
	FString Value;
	FString Signature;
	TestTrue(TEXT("template id function exists"),
		CallStringFunction(Result.Library, TEXT("WalletTemplateId"), Value, &Signature));
	AddInfo(FString::Printf(TEXT("WalletTemplateId params: [%s] returned '%s'"), *Signature, *Value));
	TestEqual(TEXT("returns the baked template id"), Value, FString(TEXT("tmpl-1")));

	TestTrue(TEXT("config id function exists"), CallStringFunction(Result.Library, TEXT("GameplayConfigId"), Value));
	TestEqual(TEXT("returns the baked config id"), Value, FString(TEXT("cfg-1")));

	// A name with a separator becomes one PascalCase identifier.
	TestTrue(TEXT("spaced name became an identifier"),
		CallStringFunction(Result.Library, TEXT("PlayerProgressTemplateId"), Value));
	TestEqual(TEXT("returns its id"), Value, FString(TEXT("tmpl-2")));

	// Every generated function is categorised beside the SDK's own nodes. Uncategorised, they land in
	// Unreal's default bucket and are findable only by name — which is most of what generating them was
	// meant to avoid, and is invisible from the code.
	for (const UEdGraph* Graph : Result.Library->FunctionGraphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				// Sub-categorised by kind: a baked constant and an enum lookup are different things and
				// should not sort together, and neither should interleave with the conversions.
				const FString Category = Entry->MetaData.Category.ToString();
				TestTrue(FString::Printf(TEXT("'%s' is categorised under Flock|Generated (got '%s')"),
					*Graph->GetName(), *Category), Category.StartsWith(TEXT("Flock|Generated|")));
			}
		}
	}

	return true;
}

// ── Function names are legal identifiers and never collide ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLibraryNamingTest, "Flock.Editor.FunctionLibrary.NamesFunctionsSafely",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLibraryNamingTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("spaces collapse"),
		FFlockFunctionLibraryEmitter::MakeTemplateIdFunctionName(TEXT("player progress")),
		FString(TEXT("PlayerProgressTemplateId")));
	TestEqual(TEXT("snake_case collapses"),
		FFlockFunctionLibraryEmitter::MakeConfigIdFunctionName(TEXT("game_play")), FString(TEXT("GamePlayConfigId")));
	TestEqual(TEXT("leading digit is prefixed"),
		FFlockFunctionLibraryEmitter::MakeTemplateIdFunctionName(TEXT("2ndWind")), FString(TEXT("_2ndWindTemplateId")));

	// A template and a config are one function namespace, so names that collapse together must still
	// produce two callable functions rather than one overwriting the other.
	FFlockSchemaSnapshot Snap;
	Snap.PlayerTemplates.Add(Template(TEXT("tmpl-1"), TEXT("Wallet")));
	Snap.PlayerTemplates.Add(Template(TEXT("tmpl-2"), TEXT("wallet")));

	const FFlockFunctionLibraryEmitter::FEmitResult Result =
		FFlockFunctionLibraryEmitter::BuildLibrary(Snap, FreshOuter());
	if (!TestTrue(TEXT("library built"), Result.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("both functions emitted"), Result.FunctionCount, 2);

	FString First;
	FString Second;
	TestTrue(TEXT("first callable"), CallStringFunction(Result.Library, TEXT("WalletTemplateId"), First));
	TestTrue(TEXT("disambiguated second callable"),
		CallStringFunction(Result.Library, TEXT("WalletTemplateId_2"), Second));
	TestEqual(TEXT("first id"), First, FString(TEXT("tmpl-1")));
	TestEqual(TEXT("second id"), Second, FString(TEXT("tmpl-2")));

	return true;
}

// ── An empty game produces a valid, empty library rather than nothing or a broken asset ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLibraryEmptyTest, "Flock.Editor.FunctionLibrary.EmptyGameStillCompiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLibraryEmptyTest::RunTest(const FString& Parameters)
{
	FFlockSchemaSnapshot Bare;
	Bare.GameVersionId = TEXT("ver-1");

	const FFlockFunctionLibraryEmitter::FEmitResult Result =
		FFlockFunctionLibraryEmitter::BuildLibrary(Bare, FreshOuter());

	// The asset exists and compiles: a later sync adds functions to it, and a half-created library would
	// be worse than an empty one.
	TestTrue(TEXT("library built"), Result.IsValid());
	TestEqual(TEXT("no functions"), Result.FunctionCount, 0);
	TestEqual(TEXT("no warnings"), Result.Warnings.Num(), 0);
	if (Result.IsValid())
	{
		TestTrue(TEXT("still compiles"), Result.Library->Status != BS_Error);
	}

	// An entity with no id contributes nothing — there would be no constant to bake.
	FFlockSchemaSnapshot NoId;
	NoId.PlayerTemplates.Add(Template(FString(), TEXT("Nameless")));
	TestEqual(TEXT("id-less entity is skipped"),
		FFlockFunctionLibraryEmitter::BuildLibrary(NoId, FreshOuter()).FunctionCount, 0);

	return true;
}

// ── Re-syncing into the same package replaces the functions instead of crashing or accumulating ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLibraryReSyncTest, "Flock.Editor.FunctionLibrary.ReSyncReplacesFunctions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLibraryReSyncTest::RunTest(const FString& Parameters)
{
	// The same outer twice, which is exactly what a second Sync Schemas does. CreateBlueprint *asserts*
	// when the asset already exists, so getting this wrong takes the editor down rather than failing a
	// test — hence pinning it.
	UPackage* Outer = FreshOuter();

	const FFlockFunctionLibraryEmitter::FEmitResult First =
		FFlockFunctionLibraryEmitter::BuildLibrary(Snapshot(), Outer);
	if (!TestTrue(TEXT("first sync built"), First.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("three functions"), First.FunctionCount, 3);

	// A second sync where the backend has dropped a template and renumbered another.
	FFlockSchemaSnapshot Changed;
	Changed.GameVersionId = TEXT("ver-1");
	Changed.PlayerTemplates.Add(Template(TEXT("tmpl-9"), TEXT("Wallet")));

	const FFlockFunctionLibraryEmitter::FEmitResult Second =
		FFlockFunctionLibraryEmitter::BuildLibrary(Changed, Outer);
	if (!TestTrue(TEXT("second sync built"), Second.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("same asset reused"), Second.Library, First.Library);
	TestEqual(TEXT("only the surviving function"), Second.FunctionCount, 1);

	FString Value;
	TestTrue(TEXT("surviving function callable"), CallStringFunction(Second.Library, TEXT("WalletTemplateId"), Value));
	TestEqual(TEXT("returns the new id"), Value, FString(TEXT("tmpl-9")));

	// A template deleted on the backend must lose its function here too, or the library grows forever and
	// offers ids that no longer exist.
	TestFalse(TEXT("dropped template's function is gone"),
		CallStringFunction(Second.Library, TEXT("GameplayConfigId"), Value));

	return true;
}

// ── The conversion functions round-trip a row through its generated struct ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLibraryConversionTest, "Flock.Editor.FunctionLibrary.EmitsStructConversions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLibraryConversionTest::RunTest(const FString& Parameters)
{
	UPackage* Outer = FreshOuter();

	// A template with a real schema, so the struct emitter has something to build from.
	FFlockSchemaSnapshot Snap;
	Snap.GameVersionId = TEXT("ver-1");
	FFlockPlayerTemplateSchema Wallet = Template(TEXT("tmpl-1"), TEXT("Wallet"));
	Wallet.SchemaJson = TEXT("[{\"type\":\"int\",\"field_name\":\"game_currencies\",\"type_name\":\"int\"}]");
	Snap.PlayerTemplates.Add(Wallet);

	const FFlockStructEmitter::FEmitResult Structs = FFlockStructEmitter::BuildAll(Snap, Outer);
	if (!TestEqual(TEXT("struct built"), Structs.StructCount, 1))
	{
		return false;
	}

	const FFlockFunctionLibraryEmitter::FEmitResult Result =
		FFlockFunctionLibraryEmitter::BuildLibrary(Snap, Outer, Structs.StructById);
	if (!TestTrue(TEXT("library built"), Result.IsValid()))
	{
		return false;
	}
	for (const FString& Warning : Result.Warnings)
	{
		AddInfo(Warning);
	}

	// Id + the read + the update builder.
	TestEqual(TEXT("three functions"), Result.FunctionCount, 3);
	TestEqual(TEXT("no warnings"), Result.Warnings.Num(), 0);
	TestTrue(TEXT("compiled"), Result.Library->Status != BS_Error);

	UClass* Generated = Result.Library->GeneratedClass;
	if (!TestNotNull(TEXT("generated class"), Generated))
	{
		return false;
	}
	UFunction* ToWallet = Generated->FindFunctionByName(TEXT("ReadWalletTemplate"));
	UFunction* FromWallet = Generated->FindFunctionByName(TEXT("MakeWalletTemplateUpdate"));
	if (!TestNotNull(TEXT("ReadWalletTemplate exists"), ToWallet) || !TestNotNull(TEXT("MakeWalletTemplateUpdate exists"), FromWallet))
	{
		return false;
	}

	// The signatures are the real assertion: the wildcard has to have become the *generated* struct, not
	// the int32 it is declared as in C++.
	const FStructProperty* ToOut = nullptr;
	for (TFieldIterator<FProperty> It(ToWallet); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm))
		{
			ToOut = CastField<FStructProperty>(*It);
		}
	}
	if (TestNotNull(TEXT("ReadWalletTemplate returns a struct"), ToOut))
	{
		TestEqual(TEXT("and it is the generated one"),
			ToOut->Struct.Get(), static_cast<UScriptStruct*>(Structs.StructById[TEXT("tmpl-1")]));
	}

	// FromWallet takes the generated struct and gives back a command body.
	bool bTakesGeneratedStruct = false;
	bool bReturnsCommandData = false;
	for (TFieldIterator<FProperty> It(FromWallet); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		const FStructProperty* AsStruct = CastField<FStructProperty>(*It);
		if (!AsStruct)
		{
			continue;
		}
		if (It->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm))
		{
			bReturnsCommandData |= AsStruct->Struct == FFlockCommandData::StaticStruct();
		}
		else
		{
			bTakesGeneratedStruct |= AsStruct->Struct.Get() == static_cast<UScriptStruct*>(Structs.StructById[TEXT("tmpl-1")]);
		}
	}
	TestTrue(TEXT("the update builder takes the generated struct"), bTakesGeneratedStruct);
	TestTrue(TEXT("the update builder returns a command body"), bReturnsCommandData);

	// A game config gets a read but no update builder: configs are game-wide and admin-only, so offering
	// one would be offering a call the server always rejects.
	FFlockSchemaSnapshot ConfigOnly;
	ConfigOnly.GameVersionId = TEXT("ver-1");
	FFlockGameConfigSchema Gameplay;
	Gameplay.Id = TEXT("cfg-1");
	Gameplay.Name = TEXT("Gameplay");
	Gameplay.SchemaJson = TEXT("[{\"type\":\"int\",\"field_name\":\"move_speed\"}]");
	ConfigOnly.GameConfigs.Add(Gameplay);

	UPackage* ConfigOuter = FreshOuter();
	const FFlockStructEmitter::FEmitResult ConfigStructs = FFlockStructEmitter::BuildAll(ConfigOnly, ConfigOuter);
	const FFlockFunctionLibraryEmitter::FEmitResult ConfigResult =
		FFlockFunctionLibraryEmitter::BuildLibrary(ConfigOnly, ConfigOuter, ConfigStructs.StructById);
	if (TestTrue(TEXT("config library built"), ConfigResult.IsValid()))
	{
		UClass* ConfigClass = ConfigResult.Library->GeneratedClass;
		TestEqual(TEXT("id + read only"), ConfigResult.FunctionCount, 2);
		TestNotNull(TEXT("config gets a read"), ConfigClass->FindFunctionByName(TEXT("ReadGameplayConfig")));
		TestNull(TEXT("config gets no update builder"),
			ConfigClass->FindFunctionByName(TEXT("MakeGameplayConfigUpdate")));
	}

	return true;
}

// ── Without a generated struct there is nothing to convert, and only the id is emitted ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLibraryNoStructTest, "Flock.Editor.FunctionLibrary.SkipsConversionsWithoutAStruct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLibraryNoStructTest::RunTest(const FString& Parameters)
{
	// No struct map passed: an entity whose schema produced nothing still gets its id constant, which is
	// useful on its own for feeding the SDK's own nodes.
	const FFlockFunctionLibraryEmitter::FEmitResult Result =
		FFlockFunctionLibraryEmitter::BuildLibrary(Snapshot(), FreshOuter());

	TestTrue(TEXT("library built"), Result.IsValid());
	TestEqual(TEXT("ids only"), Result.FunctionCount, 3);
	TestEqual(TEXT("no warnings"), Result.Warnings.Num(), 0);

	FString Value;
	TestTrue(TEXT("id still callable"), CallStringFunction(Result.Library, TEXT("WalletTemplateId"), Value));
	TestEqual(TEXT("id correct"), Value, FString(TEXT("tmpl-1")));

	return true;
}

// ── The generated conversion actually runs: the wildcard thunk fills a real struct instance ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLibraryConversionRunsTest, "Flock.Editor.FunctionLibrary.ConversionThunkFillsAStruct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLibraryConversionRunsTest::RunTest(const FString& Parameters)
{
	UPackage* Outer = FreshOuter();

	FFlockSchemaSnapshot Snap;
	Snap.GameVersionId = TEXT("ver-1");
	FFlockPlayerTemplateSchema Wallet = Template(TEXT("tmpl-1"), TEXT("Wallet"));
	Wallet.SchemaJson = TEXT("[{\"type\":\"int\",\"field_name\":\"game_currencies\",\"type_name\":\"int\"}]");
	Snap.PlayerTemplates.Add(Wallet);

	const FFlockStructEmitter::FEmitResult Structs = FFlockStructEmitter::BuildAll(Snap, Outer);
	const FFlockFunctionLibraryEmitter::FEmitResult Result =
		FFlockFunctionLibraryEmitter::BuildLibrary(Snap, Outer, Structs.StructById);
	UFunction* ToWallet = Result.IsValid() && Result.Library->GeneratedClass
		? Result.Library->GeneratedClass->FindFunctionByName(TEXT("ReadWalletTemplate"))
		: nullptr;
	if (!TestNotNull(TEXT("ReadWalletTemplate exists"), ToWallet))
	{
		return false;
	}

	// Executing it is the only way to exercise `execDataToStruct` — the CustomThunk that steps the stack to
	// recover the connected struct's type and address. Signatures and metadata can all be right while that
	// body is wrong, and nothing else in the suite runs it.
	uint8* Frame = static_cast<uint8*>(FMemory::Malloc(FMath::Max<int32>(ToWallet->ParmsSize, 1)));
	FMemory::Memzero(Frame, FMath::Max<int32>(ToWallet->ParmsSize, 1));
	for (TFieldIterator<FProperty> It(ToWallet); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->InitializeValue_InContainer(Frame);
	}

	// Seed the input with a row as a read would hand it back — Pascal-cased, unlike the declared name the
	// struct member carries, so this also proves the spelling bridge runs inside the generated graph.
	FStructProperty* OutStructProp = nullptr;
	for (TFieldIterator<FProperty> It(ToWallet); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		FStructProperty* AsStruct = CastField<FStructProperty>(*It);
		if (!AsStruct)
		{
			continue;
		}
		if (AsStruct->Struct == FFlockStructuredData::StaticStruct())
		{
			FFlockStructuredData Row;
			Row.FlatJson = TEXT("{\"GameCurrencies\":250}");
			*AsStruct->ContainerPtrToValuePtr<FFlockStructuredData>(Frame) = Row;
		}
		else if (It->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm))
		{
			OutStructProp = AsStruct;
		}
	}
	if (!TestNotNull(TEXT("out struct parameter"), OutStructProp))
	{
		FMemory::Free(Frame);
		return false;
	}

	Result.Library->GeneratedClass->GetDefaultObject()->ProcessEvent(ToWallet, Frame);

	// Read the filled member back off the generated struct instance.
	int32 Filled = -1;
	const void* StructMemory = OutStructProp->ContainerPtrToValuePtr<void>(Frame);
	for (TFieldIterator<FProperty> It(OutStructProp->Struct); It; ++It)
	{
		if (FFlockStructBinder::GetMemberName(OutStructProp->Struct, *It) == TEXT("game_currencies"))
		{
			if (const FIntProperty* AsInt = CastField<FIntProperty>(*It))
			{
				Filled = AsInt->GetPropertyValue_InContainer(StructMemory);
			}
		}
	}
	TestEqual(TEXT("the thunk filled the generated struct"), Filled, 250);

	for (TFieldIterator<FProperty> It(ToWallet); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->DestroyValue_InContainer(Frame);
	}
	FMemory::Free(Frame);
	return true;
}

// ── An enum lookup turns a picked member into the string the SDK sends ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLibraryEnumLookupTest, "Flock.Editor.FunctionLibrary.EmitsEnumLookups",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLibraryEnumLookupTest::RunTest(const FString& Parameters)
{
	UPackage* Outer = FreshOuter();

	// Two shop items, so the lookup has to pick between members rather than always returning the first.
	FFlockSchemaSnapshot Snap;
	Snap.GameVersionId = TEXT("ver-1");
	FFlockShop Shop;
	Shop.Id = TEXT("shop-1");
	Shop.Name = TEXT("Starter");
	FFlockShopItem Gem;
	Gem.Id = TEXT("item-1");
	Gem.Name = TEXT("Gem Pack");
	Gem.Currency = TEXT("Gold");
	FFlockShopItem Shield;
	Shield.Id = TEXT("item-2");
	Shield.Name = TEXT("Shield");
	Shield.Currency = TEXT("Gold");
	Shop.ShopItems = { Gem, Shield };
	Snap.Shops = { Shop };

	const FFlockEnumEmitter::FEmitResult Enums = FFlockEnumEmitter::BuildAll(Snap, Outer);
	if (!TestTrue(TEXT("shop item enum built"), Enums.ShopItems.IsValid()))
	{
		return false;
	}

	FFlockEnumLookupSpec Lookup;
	Lookup.FunctionName = TEXT("ShopItemId");
	Lookup.Enum = Enums.ShopItems.Enum;
	Lookup.Members = Enums.ShopItems.WireValueByDisplayName;

	const FFlockFunctionLibraryEmitter::FEmitResult Result = FFlockFunctionLibraryEmitter::BuildLibrary(
		Snap, Outer, TMap<FString, UUserDefinedStruct*>(), { Lookup });
	for (const FString& Warning : Result.Warnings)
	{
		AddInfo(Warning);
	}
	if (!TestTrue(TEXT("library built"), Result.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("no warnings"), Result.Warnings.Num(), 0);
	TestTrue(TEXT("compiled"), Result.Library->Status != BS_Error);

	UFunction* Function = Result.Library->GeneratedClass
		? Result.Library->GeneratedClass->FindFunctionByName(TEXT("ShopItemId"))
		: nullptr;
	if (!TestNotNull(TEXT("lookup function exists"), Function))
	{
		return false;
	}

	// Running it for each member is the assertion that matters: the Select node's option pins have to line
	// up with the enum's members in order, and nothing about the graph shows that until it executes.
	auto LookupFor = [&](uint8 MemberIndex)
	{
		uint8* Frame = static_cast<uint8*>(FMemory::Malloc(FMath::Max<int32>(Function->ParmsSize, 1)));
		FMemory::Memzero(Frame, FMath::Max<int32>(Function->ParmsSize, 1));
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(Frame);
			if (const FByteProperty* AsByte = CastField<FByteProperty>(*It))
			{
				AsByte->SetPropertyValue_InContainer(Frame, MemberIndex);
			}
		}
		Result.Library->GeneratedClass->GetDefaultObject()->ProcessEvent(Function, Frame);

		FString Value;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm))
			{
				if (const FStrProperty* AsString = CastField<FStrProperty>(*It))
				{
					Value = AsString->GetPropertyValue_InContainer(Frame);
				}
			}
		}
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Frame);
		}
		FMemory::Free(Frame);
		return Value;
	};

	// Members are ordered by item id, so index 0 is item-1 and index 1 is item-2.
	TestEqual(TEXT("first member maps to its id"), LookupFor(0), FString(TEXT("item-1")));
	TestEqual(TEXT("second member maps to its id"), LookupFor(1), FString(TEXT("item-2")));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
