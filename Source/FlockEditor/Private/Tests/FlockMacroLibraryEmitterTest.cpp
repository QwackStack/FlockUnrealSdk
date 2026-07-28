// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Codegen/FlockEnumEmitter.h"
#include "Codegen/FlockFunctionLibraryEmitter.h"
#include "Codegen/FlockMacroLibraryEmitter.h"
#include "Codegen/FlockStructEmitter.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/UserDefinedEnum.h"
#include "GameFramework/Actor.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h"

/**
 * A macro has no generated UFunction — it is expanded into whatever calls it — so unlike the function
 * library there is nothing to invoke by reflection.
 *
 * Two things carry the weight instead. The macro library's own compile catches a graph that cannot
 * resolve, and `CompilesInsideAConsumerGraph` compiles an Actor Blueprint that actually *calls* each
 * macro, which is what puts the expansion under test. Structural assertions sit on top, because a graph
 * can compile while being wired to the wrong thing — four of the engine traps this tier hit did exactly
 * that.
 */
namespace FlockMacroLibraryEmitterTestHelpers
{
	inline UPackage* FreshOuter()
	{
		return CreatePackage(*FString::Printf(TEXT("/Temp/FlockMacroTest_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

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

	inline FFlockShopItem Item(const FString& Id, const FString& Name, const FString& Currency)
	{
		FFlockShopItem Result;
		Result.Id = Id;
		Result.Name = Name;
		Result.Currency = Currency;
		return Result;
	}

	/** Just a config, for the tests that only care about the read-only path. */
	inline FFlockSchemaSnapshot ConfigOnlySnapshot()
	{
		FFlockSchemaSnapshot Result;
		Result.GameVersionId = TEXT("ver-1");
		FFlockGameConfigSchema Config;
		Config.Id = TEXT("cfg-1");
		Config.Name = TEXT("Gameplay");
		Config.SchemaJson = TEXT("[{\"type\":\"int\",\"field_name\":\"move_speed\",\"type_name\":\"int\"}]");
		Result.GameConfigs.Add(Config);
		return Result;
	}

	/**
	 * A game with everything the emitter can produce a macro from: a config, a writable template, the
	 * `currency`-tagged wallet whose id `Add Funds` bakes, the `achievement`-tagged row the achievement
	 * enum is built from, and a shop to give the item and currency enums members.
	 */
	inline FFlockSchemaSnapshot FullSnapshot()
	{
		FFlockSchemaSnapshot Result = ConfigOnlySnapshot();

		Result.PlayerTemplates.Add(Template(TEXT("tmpl-wallet"), TEXT("Wallet"), TEXT("currency"),
			TEXT("[{\"type\":\"int\",\"field_name\":\"gold\"},{\"type\":\"int\",\"field_name\":\"shards\"}]")));
		Result.PlayerTemplates.Add(Template(TEXT("tmpl-trophies"), TEXT("Trophies"), TEXT("achievement"),
			TEXT("[{\"type\":\"bool\",\"field_name\":\"first_win\"},{\"type\":\"bool\",\"field_name\":\"flawless_run\"}]")));

		FFlockShop Shop;
		Shop.Id = TEXT("shop-1");
		Shop.Name = TEXT("Starter");
		Shop.ShopItems.Add(Item(TEXT("item-1"), TEXT("Gem Pack"), TEXT("Gold")));
		Result.Shops.Add(Shop);

		return Result;
	}

	/** Runs the emitters in the order the sync does: structs, enums, functions, then macros. */
	struct FBuilt
	{
		UPackage* Outer = nullptr;
		FFlockFunctionLibraryEmitter::FEmitResult Functions;
		FFlockMacroLibraryEmitter::FEmitResult Macros;
	};

	inline FBuilt BuildAll(const FFlockSchemaSnapshot& Snapshot, UPackage* Outer = nullptr)
	{
		FBuilt Built;
		Built.Outer = Outer ? Outer : FreshOuter();
		const FFlockStructEmitter::FEmitResult Structs = FFlockStructEmitter::BuildAll(Snapshot, Built.Outer);
		const FFlockEnumEmitter::FEmitResult Enums = FFlockEnumEmitter::BuildAll(Snapshot, Built.Outer);

		// Mirrors the runner: an emitted enum travels to the function library as a lookup, and the macro
		// library reads the enum back off that result to type its command pins.
		TArray<FFlockEnumLookupSpec> Lookups;
		auto AddLookup = [&Lookups](const TCHAR* FunctionName, const FFlockEnumEmitter::FEnumResult& Emitted)
		{
			if (Emitted.IsValid())
			{
				FFlockEnumLookupSpec Spec;
				Spec.FunctionName = FunctionName;
				Spec.Enum = Emitted.Enum;
				Spec.Members = Emitted.WireValueByDisplayName;
				Lookups.Add(MoveTemp(Spec));
			}
		};
		AddLookup(TEXT("ShopItemId"), Enums.ShopItems);
		AddLookup(TEXT("CurrencyName"), Enums.Currencies);
		AddLookup(TEXT("AchievementName"), Enums.Achievements);

		Built.Functions = FFlockFunctionLibraryEmitter::BuildLibrary(
			Snapshot, Built.Outer, Structs.StructById, Lookups);
		Built.Macros = FFlockMacroLibraryEmitter::BuildLibrary(Snapshot, Built.Outer, Built.Functions);
		return Built;
	}

	inline UEdGraph* FindMacro(UBlueprint* Library, const TCHAR* Name)
	{
		for (UEdGraph* Graph : Library->MacroGraphs)
		{
			if (Graph->GetName() == Name)
			{
				return Graph;
			}
		}
		return nullptr;
	}

	template <typename TNode>
	TNode* FindNode(UEdGraph* Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (TNode* Typed = Cast<TNode>(Node))
			{
				return Typed;
			}
		}
		return nullptr;
	}

	/** A tunnel pin as the macro's caller sees it: macro inputs are entry *outputs*, and vice versa. */
	inline UEdGraphPin* FindTunnelPin(UEdGraph* Macro, const TCHAR* PinName, EEdGraphPinDirection TunnelSide)
	{
		for (UEdGraphNode* Node : Macro->Nodes)
		{
			UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
			if (!Tunnel)
			{
				continue;
			}
			for (UEdGraphPin* Pin : Tunnel->Pins)
			{
				if (Pin->PinName == PinName && Pin->Direction == TunnelSide)
				{
					return Pin;
				}
			}
		}
		return nullptr;
	}

	inline UEdGraphPin* MacroInput(UEdGraph* Macro, const TCHAR* PinName)
	{
		return FindTunnelPin(Macro, PinName, EGPD_Output);
	}

	inline UEdGraphPin* MacroOutput(UEdGraph* Macro, const TCHAR* PinName)
	{
		return FindTunnelPin(Macro, PinName, EGPD_Input);
	}
}

using namespace FlockMacroLibraryEmitterTestHelpers;

// ── One node per config: the macro holds the fetch, so a caller supplies no id ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockMacroEmitTest, "Flock.Editor.MacroLibrary.EmitsOneNodePerConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockMacroEmitTest::RunTest(const FString& Parameters)
{
	const FBuilt Built = BuildAll(ConfigOnlySnapshot());
	for (const FString& Warning : Built.Macros.Warnings)
	{
		AddInfo(Warning);
	}
	if (!TestTrue(TEXT("macro library built"), Built.Macros.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("one macro"), Built.Macros.MacroCount, 1);
	TestEqual(TEXT("no warnings"), Built.Macros.Warnings.Num(), 0);
	// A macro graph with a bad connection fails the library's compilation, so this is the load-bearing
	// assertion — there is no way to invoke a macro to check it.
	TestTrue(TEXT("compiled"), Built.Macros.Library->Status != BS_Error);

	UEdGraph* Macro = FindMacro(Built.Macros.Library, TEXT("GetGameplay"));
	if (!TestNotNull(TEXT("GetGameplay exists"), Macro))
	{
		return false;
	}

	// The fetch lives inside the macro — that is the whole point, and the only reason a macro was needed
	// rather than a function.
	UK2Node_AsyncAction* Fetch = FindNode<UK2Node_AsyncAction>(Macro);
	if (TestNotNull(TEXT("the macro contains the async fetch"), Fetch))
	{
		// The id is baked, so nothing is asked of the caller.
		UEdGraphPin* ConfigIdPin = Fetch->FindPin(TEXT("ConfigId"));
		if (TestNotNull(TEXT("fetch has a config id pin"), ConfigIdPin))
		{
			TestEqual(TEXT("the config id is baked in"), ConfigIdPin->DefaultValue, FString(TEXT("cfg-1")));
			TestEqual(TEXT("and is not left for a caller to wire"), ConfigIdPin->LinkedTo.Num(), 0);
		}
	}

	// And the conversion, so the macro answers with a typed struct rather than a data handle.
	TestNotNull(TEXT("the macro calls the generated read"), FindNode<UK2Node_CallFunction>(Macro));

	return true;
}

// ── The macro's own pins: one exec in, and Completed / Failed / Struct out ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockMacroPinsTest, "Flock.Editor.MacroLibrary.ExposesTypedTunnelPins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockMacroPinsTest::RunTest(const FString& Parameters)
{
	const FBuilt Built = BuildAll(ConfigOnlySnapshot());
	UEdGraph* Macro = Built.Macros.IsValid() ? FindMacro(Built.Macros.Library, TEXT("GetGameplay")) : nullptr;
	if (!TestNotNull(TEXT("GetGameplay exists"), Macro))
	{
		return false;
	}

	// A macro tunnel arrives with no pins at all, unlike a function entry — so every pin below only exists
	// because the emitter made it, and a regression would silently produce a macro with no exec input.
	TestNotNull(TEXT("has an exec input"), MacroInput(Macro, TEXT("In")));
	TestNotNull(TEXT("has a Completed exec output"), MacroOutput(Macro, TEXT("Completed")));
	// Both exec paths are exposed: a fetch that failed must be distinguishable from one that returned.
	TestNotNull(TEXT("has a Failed exec output"), MacroOutput(Macro, TEXT("Failed")));
	// And the reason it failed, or a graph can report nothing to the player.
	TestNotNull(TEXT("has an Error output"), MacroOutput(Macro, TEXT("Error")));

	UEdGraphPin* StructOut = MacroOutput(Macro, TEXT("Struct"));
	if (TestNotNull(TEXT("hands back a struct"), StructOut))
	{
		// Typed to the generated struct, not left wildcard — otherwise Break Struct is useless.
		TestEqual(TEXT("as a struct pin"), StructOut->PinType.PinCategory, UEdGraphSchema_K2::PC_Struct);
		TestNotNull(TEXT("of the generated type"),
			Cast<UUserDefinedStruct>(StructOut->PinType.PinSubCategoryObject.Get()));
	}

	return true;
}

// ── A template gets Get + Save, and the Get hands out the row id the Save needs ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockMacroTemplateTest, "Flock.Editor.MacroLibrary.TemplateGetOutputsRowId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockMacroTemplateTest::RunTest(const FString& Parameters)
{
	const FBuilt Built = BuildAll(FullSnapshot());
	for (const FString& Warning : Built.Macros.Warnings)
	{
		AddInfo(Warning);
	}
	if (!TestTrue(TEXT("macro library built"), Built.Macros.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("compiled"), Built.Macros.Library->Status != BS_Error);

	UEdGraph* Get = FindMacro(Built.Macros.Library, TEXT("GetWallet"));
	if (!TestNotNull(TEXT("GetWallet exists"), Get))
	{
		return false;
	}

	// The row id is the reason this differs from a config read: it is per-player, only the fetch knows it,
	// and Save cannot be called without it. If it stops coming out, Save becomes unusable.
	UEdGraphPin* RowId = MacroOutput(Get, TEXT("RowId"));
	if (TestNotNull(TEXT("Get hands back the row id"), RowId))
	{
		TestEqual(TEXT("as a string"), RowId->PinType.PinCategory, UEdGraphSchema_K2::PC_String);
	}
	TestNotNull(TEXT("and the typed struct"), MacroOutput(Get, TEXT("Struct")));

	UK2Node_AsyncAction* Fetch = FindNode<UK2Node_AsyncAction>(Get);
	if (TestNotNull(TEXT("Get contains the fetch"), Fetch))
	{
		UEdGraphPin* TemplateIdPin = Fetch->FindPin(TEXT("PlayerTemplateId"));
		if (TestNotNull(TEXT("fetch has a template id pin"), TemplateIdPin))
		{
			TestEqual(TEXT("the template id is baked in"), TemplateIdPin->DefaultValue, FString(TEXT("tmpl-wallet")));
		}
	}

	UEdGraph* Save = FindMacro(Built.Macros.Library, TEXT("SaveWallet"));
	if (!TestNotNull(TEXT("SaveWallet exists"), Save))
	{
		return false;
	}
	// Save takes back exactly what Get produced, so the pair composes without anything in between.
	UEdGraphPin* StructIn = MacroInput(Save, TEXT("Struct"));
	if (TestNotNull(TEXT("Save takes the typed struct"), StructIn))
	{
		TestNotNull(TEXT("of the generated type"),
			Cast<UUserDefinedStruct>(StructIn->PinType.PinSubCategoryObject.Get()));
	}
	TestNotNull(TEXT("Save takes the row id"), MacroInput(Save, TEXT("RowId")));
	TestNotNull(TEXT("Save reports completion"), MacroOutput(Save, TEXT("Completed")));
	TestNotNull(TEXT("Save reports failure"), MacroOutput(Save, TEXT("Failed")));
	// Deliberately absent: the update's response row is authoritative, but handing it back would invite a
	// graph to use write responses to observe changes made elsewhere.
	TestNull(TEXT("Save hands back no struct"), MacroOutput(Save, TEXT("Struct")));

	return true;
}

// ── The command macros: one per family, keyed by a generated enum ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockMacroCommandTest, "Flock.Editor.MacroLibrary.EmitsEnumKeyedCommands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockMacroCommandTest::RunTest(const FString& Parameters)
{
	const FBuilt Built = BuildAll(FullSnapshot());
	if (!TestTrue(TEXT("macro library built"), Built.Macros.IsValid()))
	{
		return false;
	}

	// One macro per family, not one per shop item: the node count must not track the catalog.
	UEdGraph* Purchase = FindMacro(Built.Macros.Library, TEXT("Purchase"));
	if (TestNotNull(TEXT("Purchase exists"), Purchase))
	{
		UEdGraphPin* ItemPin = MacroInput(Purchase, TEXT("Item"));
		if (TestNotNull(TEXT("it takes an item"), ItemPin))
		{
			// A byte pin carrying the generated enum is what makes the dropdown typed. A plain string here
			// would compile and would accept any typo.
			TestEqual(TEXT("as an enum pin"), ItemPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Byte);
			TestNotNull(TEXT("of the generated enum type"),
				Cast<UUserDefinedEnum>(ItemPin->PinType.PinSubCategoryObject.Get()));
		}
		TestNotNull(TEXT("and hands back what was bought"), MacroOutput(Purchase, TEXT("Entry")));
	}

	TestNotNull(TEXT("Unlock Achievement exists"), FindMacro(Built.Macros.Library, TEXT("UnlockAchievement")));

	UEdGraph* AddFunds = FindMacro(Built.Macros.Library, TEXT("AddFunds"));
	if (TestNotNull(TEXT("Add Funds exists"), AddFunds))
	{
		TestNotNull(TEXT("it takes a currency"), MacroInput(AddFunds, TEXT("Currency")));
		UEdGraphPin* AmountPin = MacroInput(AddFunds, TEXT("Amount"));
		if (TestNotNull(TEXT("and an amount"), AmountPin))
		{
			TestEqual(TEXT("as an int"), AmountPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Int);
		}

		// The `currency`-tagged template's id is baked so the call skips the runtime tag scan. When no
		// template carries the tag the pin is left empty, which is the SDK's own resolve-by-tag path.
		for (UEdGraphNode* Node : AddFunds->Nodes)
		{
			if (UK2Node_AsyncAction* Command = Cast<UK2Node_AsyncAction>(Node))
			{
				UEdGraphPin* TemplatePin = Command->FindPin(TEXT("CurrencyTemplateId"));
				if (TestNotNull(TEXT("the command takes a currency template id"), TemplatePin))
				{
					TestEqual(TEXT("baked from the currency-tagged template"), TemplatePin->DefaultValue,
						FString(TEXT("tmpl-wallet")));
				}
			}
		}
	}

	return true;
}

// ── A game with no shop gets no Purchase node, rather than one with an empty dropdown ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockMacroNoCatalogTest, "Flock.Editor.MacroLibrary.SkipsCommandsWithoutEnums",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockMacroNoCatalogTest::RunTest(const FString& Parameters)
{
	// A config and nothing else: no shop, no tagged templates, so no enum has any members.
	const FBuilt Built = BuildAll(ConfigOnlySnapshot());
	if (!TestTrue(TEXT("macro library built"), Built.Macros.IsValid()))
	{
		return false;
	}

	TestNull(TEXT("no Purchase macro"), FindMacro(Built.Macros.Library, TEXT("Purchase")));
	TestNull(TEXT("no Unlock Achievement macro"), FindMacro(Built.Macros.Library, TEXT("UnlockAchievement")));
	TestNull(TEXT("no Add Funds macro"), FindMacro(Built.Macros.Library, TEXT("AddFunds")));
	TestTrue(TEXT("and the library still compiles"), Built.Macros.Library->Status != BS_Error);

	return true;
}

// ── The expansion, not the emitted nodes: a graph that calls every macro must compile ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockMacroConsumerTest, "Flock.Editor.MacroLibrary.CompilesInsideAConsumerGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockMacroConsumerTest::RunTest(const FString& Parameters)
{
	const FBuilt Built = BuildAll(FullSnapshot());
	if (!TestTrue(TEXT("macro library built"), Built.Macros.IsValid()))
	{
		return false;
	}

	// An Actor, because that is what the macros are parented to — the SDK's async nodes resolve their
	// world context from `self`, which only works in an actor graph.
	UBlueprint* Consumer = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Built.Outer, TEXT("FlockMacroConsumer"), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("consumer blueprint created"), Consumer)
		|| !TestTrue(TEXT("and has an event graph"), Consumer->UbergraphPages.Num() > 0))
	{
		return false;
	}
	UEdGraph* EventGraph = Consumer->UbergraphPages[0];

	// Each macro is driven from its own custom event. Driving them matters: an orphaned node is pruned
	// before expansion, so an unconnected macro instance would prove nothing at all.
	int32 Placed = 0;
	int32 Row = 0;
	for (UEdGraph* Macro : Built.Macros.Library->MacroGraphs)
	{
		FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*EventGraph);
		UK2Node_CustomEvent* Event = EventCreator.CreateNode();
		Event->CustomFunctionName = FName(*FString::Printf(TEXT("Run_%s"), *Macro->GetName()));
		Event->NodePosX = 0;
		Event->NodePosY = Row * 300;
		EventCreator.Finalize();

		FGraphNodeCreator<UK2Node_MacroInstance> MacroCreator(*EventGraph);
		UK2Node_MacroInstance* Instance = MacroCreator.CreateNode();
		Instance->SetMacroGraph(Macro);
		Instance->NodePosX = 400;
		Instance->NodePosY = Row * 300;
		MacroCreator.Finalize();
		++Row;

		UEdGraphPin* EventThen = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* MacroExec = Instance->FindPin(TEXT("In"), EGPD_Input);
		if (!TestNotNull(*FString::Printf(TEXT("'%s' instance exposes its exec input"), *Macro->GetName()), MacroExec)
			|| !EventThen)
		{
			continue;
		}
		if (TestTrue(*FString::Printf(TEXT("'%s' can be driven from an event"), *Macro->GetName()),
			EventGraph->GetSchema()->TryCreateConnection(EventThen, MacroExec)))
		{
			++Placed;
		}
	}

	TestEqual(TEXT("every macro was placed and driven"), Placed, Built.Macros.MacroCount);

	// The real assertion. Expansion inlines each macro into this graph, so a mis-wired tunnel, an
	// unresolved pin type, or a dangling connection surfaces here and nowhere earlier.
	FKismetEditorUtilities::CompileBlueprint(Consumer);
	TestTrue(TEXT("a graph calling every generated macro compiles"), Consumer->Status != BS_Error);

	return true;
}

// ── Re-syncing replaces the macros rather than crashing or accumulating ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockMacroReSyncTest, "Flock.Editor.MacroLibrary.ReSyncReplacesMacros",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockMacroReSyncTest::RunTest(const FString& Parameters)
{
	UPackage* Outer = FreshOuter();
	const FBuilt First = BuildAll(ConfigOnlySnapshot(), Outer);
	if (!TestTrue(TEXT("first sync built"), First.Macros.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("one macro"), First.Macros.MacroCount, 1);

	// A second sync where the backend dropped that config and added another.
	FFlockSchemaSnapshot Changed;
	Changed.GameVersionId = TEXT("ver-1");
	FFlockGameConfigSchema Combat;
	Combat.Id = TEXT("cfg-2");
	Combat.Name = TEXT("Combat");
	Combat.SchemaJson = TEXT("[{\"type\":\"int\",\"field_name\":\"damage\"}]");
	Changed.GameConfigs.Add(Combat);

	const FBuilt Second = BuildAll(Changed, Outer);
	if (!TestTrue(TEXT("second sync built"), Second.Macros.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("same asset reused"), Second.Macros.Library, First.Macros.Library);
	TestEqual(TEXT("one macro again, not two"), Second.Macros.MacroCount, 1);
	TestNotNull(TEXT("the new config's macro exists"), FindMacro(Second.Macros.Library, TEXT("GetCombat")));
	// A config deleted on the backend must lose its macro, or the library offers a fetch for content that
	// no longer exists.
	TestNull(TEXT("the dropped config's macro is gone"), FindMacro(Second.Macros.Library, TEXT("GetGameplay")));

	return true;
}

// ── Without a function library there is nothing to convert with, so nothing is emitted ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockMacroNoFunctionsTest, "Flock.Editor.MacroLibrary.NeedsTheFunctionLibrary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockMacroNoFunctionsTest::RunTest(const FString& Parameters)
{
	// The macro calls a generated read function; without one it would emit a fetch that hands back a raw
	// data handle, which is what the caller already had.
	const FFlockMacroLibraryEmitter::FEmitResult NoLibrary = FFlockMacroLibraryEmitter::BuildLibrary(
		ConfigOnlySnapshot(), FreshOuter(), FFlockFunctionLibraryEmitter::FEmitResult());
	TestFalse(TEXT("nothing built without a function library"), NoLibrary.IsValid());
	TestTrue(TEXT("and it says why"), NoLibrary.Warnings.Num() > 0);

	// An empty game emits a valid, empty library rather than nothing.
	FFlockSchemaSnapshot Bare;
	Bare.GameVersionId = TEXT("ver-1");
	const FBuilt Built = BuildAll(Bare);
	TestTrue(TEXT("empty library still built"), Built.Macros.IsValid());
	TestEqual(TEXT("no macros"), Built.Macros.MacroCount, 0);
	if (Built.Macros.IsValid())
	{
		TestTrue(TEXT("and still compiles"), Built.Macros.Library->Status != BS_Error);
	}

	return true;
}

#endif // WITH_AUTOMATION_TESTS
