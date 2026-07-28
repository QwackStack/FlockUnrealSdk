// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Codegen/FlockFunctionLibraryEmitter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/FlockStructLibrary.h"
#include "Codegen/FlockStructEmitter.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "FileHelpers.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Select.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockStructuredData.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

const TCHAR* const FFlockFunctionLibraryEmitter::LibraryAssetName = TEXT("FlockGeneratedLibrary");

namespace
{
	/** The pin name the generated id functions return through. */
	const TCHAR* const ReturnPinName = TEXT("Id");

	/**
	 * Where generated functions appear in the Blueprint action menu — beside the SDK's own nodes rather
	 * than in Unreal's catch-all category, and split by what they are *for*.
	 *
	 * The split earns its keep: flat, a baked constant (`Test Template Id`) and an enum lookup
	 * (`Shop Item Id`) read as the same kind of thing when they are not — one takes no input and answers
	 * with a fixed id, the other maps a picked enum member. Sorted together alphabetically they interleave
	 * with the conversions too, so a list of thirty functions has no shape at all.
	 */
	const TCHAR* const IdCategory = TEXT("Flock|Generated|Ids");
	const TCHAR* const StructCategory = TEXT("Flock|Generated|Structs");
	const TCHAR* const LookupCategory = TEXT("Flock|Generated|Lookups");

	/**
	 * Adds a pure, input-less function returning a baked string.
	 *
	 * The value rides on the result node's input-pin default rather than through a literal node: a pure
	 * function with nothing driving its result returns that default, which is the smallest graph that can
	 * express a constant and the least there is to get wrong.
	 */
	bool AddConstantFunction(UBlueprint* Blueprint, const FString& FunctionName, const FString& Value,
		TArray<FString>& OutWarnings)
	{
		if (FindObject<UEdGraph>(Blueprint, *FunctionName))
		{
			OutWarnings.Add(FString::Printf(TEXT("Function '%s' already exists; skipped."), *FunctionName));
			return false;
		}

		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint, FName(*FunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!Graph)
		{
			OutWarnings.Add(FString::Printf(TEXT("Could not create a graph for '%s'."), *FunctionName));
			return false;
		}
		// Creates the entry node via the schema's default-node pass.
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, /*bIsUserCreated*/ true, nullptr);

		UK2Node_FunctionEntry* Entry = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* AsEntry = Cast<UK2Node_FunctionEntry>(Node))
			{
				Entry = AsEntry;
				break;
			}
		}
		if (!Entry)
		{
			OutWarnings.Add(FString::Printf(TEXT("'%s': no function entry node was created."), *FunctionName));
			return false;
		}
		// Without this a generated function lands under Unreal's default category, away from the rest of
		// the SDK — findable only by knowing its name, which defeats the point of generating it.
		Entry->MetaData.Category = FText::FromString(IdCategory);
		FGraphNodeCreator<UK2Node_FunctionResult> Creator(*Graph);
		UK2Node_FunctionResult* Result = Creator.CreateNode();
		Result->NodePosX = Entry->NodePosX + 400;
		Result->NodePosY = Entry->NodePosY;
		Creator.Finalize();

		FEdGraphPinType StringType;
		StringType.PinCategory = UEdGraphSchema_K2::PC_String;
		UEdGraphPin* ReturnPin = Result->CreateUserDefinedPin(FName(ReturnPinName), StringType, EGPD_Input);
		if (!ReturnPin)
		{
			OutWarnings.Add(FString::Printf(TEXT("'%s': could not create the return pin."), *FunctionName));
			return false;
		}
		ReturnPin->DefaultValue = Value;

		// The default has to be written to the *pin description* as well, not just the live pin. Compiling
		// reconstructs an editable node's pins from UserDefinedPins, so a default set only on the live pin
		// is discarded and the function silently returns an empty string — everything else about the graph
		// looks correct, which is what makes this worth stating.
		if (Result->UserDefinedPins.Num() > 0 && Result->UserDefinedPins.Last().IsValid())
		{
			Result->UserDefinedPins.Last()->PinDefaultValue = Value;
		}

		// Execution is wired from entry to result, which is what makes the compiler emit "return this
		// literal". A *pure* function would read better at the call site, but with nothing driving its
		// result pin there is no data flow to compile and the function returns an empty string — the graph
		// still compiles cleanly, so the only symptom is a wrong value at runtime.
		UEdGraphPin* EntryThen = Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* ResultExec = Result->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		if (!EntryThen || !ResultExec || !Graph->GetSchema()->TryCreateConnection(EntryThen, ResultExec))
		{
			OutWarnings.Add(FString::Printf(
				TEXT("'%s': could not connect execution from entry to result; the function would return nothing."),
				*FunctionName));
			return false;
		}

		return true;
	}

	/** Adds a parameter to an entry/result node, writing the default into the pin description as well. */
	UEdGraphPin* AddUserPin(UK2Node_EditablePinBase* Node, const FName& PinName, const FEdGraphPinType& PinType,
		EEdGraphPinDirection Direction)
	{
		UEdGraphPin* Pin = Node->CreateUserDefinedPin(PinName, PinType, Direction);
		return Pin;
	}

	/**
	 * Adds a function that converts one way between a row's data handle and its generated struct.
	 *
	 * Both directions are one call to the hand-written wildcard node with the struct type baked into its
	 * pin, which is the whole reason those nodes exist: the SDK cannot name a generated type, but a
	 * generated graph can.
	 */
	bool AddConversionFunction(UBlueprint* Blueprint, const FString& FunctionName, UUserDefinedStruct* Struct,
		bool bDataToStruct, TArray<FString>& OutWarnings)
	{
		if (!Struct)
		{
			return false;
		}
		if (FindObject<UEdGraph>(Blueprint, *FunctionName))
		{
			OutWarnings.Add(FString::Printf(TEXT("Function '%s' already exists; skipped."), *FunctionName));
			return false;
		}

		UFunction* Target = bDataToStruct
			? UFlockStructLibrary::StaticClass()->FindFunctionByName(TEXT("DataToStruct"))
			: UFlockStructLibrary::StaticClass()->FindFunctionByName(TEXT("StructToCommandData"));
		if (!Target)
		{
			OutWarnings.Add(FString::Printf(TEXT("'%s': the SDK conversion node is missing."), *FunctionName));
			return false;
		}

		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint, FName(*FunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!Graph)
		{
			OutWarnings.Add(FString::Printf(TEXT("Could not create a graph for '%s'."), *FunctionName));
			return false;
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, /*bIsUserCreated*/ true, nullptr);

		UK2Node_FunctionEntry* Entry = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* AsEntry = Cast<UK2Node_FunctionEntry>(Node))
			{
				Entry = AsEntry;
				break;
			}
		}
		if (!Entry)
		{
			OutWarnings.Add(FString::Printf(TEXT("'%s': no function entry node was created."), *FunctionName));
			return false;
		}
		// Without this a generated function lands under Unreal's default category, away from the rest of
		// the SDK — findable only by knowing its name, which defeats the point of generating it.
		Entry->MetaData.Category = FText::FromString(StructCategory);

		FEdGraphPinType StructType;
		StructType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		StructType.PinSubCategoryObject = Struct;

		FEdGraphPinType DataType;
		DataType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		DataType.PinSubCategoryObject = FFlockStructuredData::StaticStruct();

		FEdGraphPinType CommandType;
		CommandType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		CommandType.PinSubCategoryObject = FFlockCommandData::StaticStruct();

		// A function's inputs are output pins on its entry node, and vice versa on the result node.
		UEdGraphPin* EntryValue = AddUserPin(Entry, bDataToStruct ? TEXT("Data") : TEXT("Struct"),
			bDataToStruct ? DataType : StructType, EGPD_Output);

		FGraphNodeCreator<UK2Node_CallFunction> CallCreator(*Graph);
		UK2Node_CallFunction* Call = CallCreator.CreateNode();
		Call->SetFromFunction(Target);
		Call->NodePosX = Entry->NodePosX + 320;
		Call->NodePosY = Entry->NodePosY;
		CallCreator.Finalize();

		FGraphNodeCreator<UK2Node_FunctionResult> ResultCreator(*Graph);
		UK2Node_FunctionResult* Result = ResultCreator.CreateNode();
		Result->NodePosX = Call->NodePosX + 380;
		Result->NodePosY = Entry->NodePosY;
		ResultCreator.Finalize();

		UEdGraphPin* ResultValue = AddUserPin(Result, bDataToStruct ? TEXT("Struct") : TEXT("Data"),
			bDataToStruct ? StructType : CommandType, EGPD_Input);

		const UEdGraphSchema* Schema = Graph->GetSchema();
		UEdGraphPin* WildcardPin = Call->FindPin(bDataToStruct ? TEXT("OutStruct") : TEXT("Struct"));
		if (!EntryValue || !ResultValue || !WildcardPin)
		{
			OutWarnings.Add(FString::Printf(TEXT("'%s': could not build the conversion pins."), *FunctionName));
			return false;
		}

		// The wildcard is typed explicitly rather than left to resolve from a connection: it is declared
		// int32 in C++ and only becomes the struct because something says so.
		WildcardPin->PinType = StructType;
		Call->NotifyPinConnectionListChanged(WildcardPin);

		bool bWired = true;
		if (bDataToStruct)
		{
			// Impure node: execution runs entry -> call -> result.
			bWired &= Schema->TryCreateConnection(Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output),
				Call->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
			bWired &= Schema->TryCreateConnection(Call->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output),
				Result->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
			bWired &= Schema->TryCreateConnection(EntryValue, Call->FindPin(TEXT("Data")));
			bWired &= Schema->TryCreateConnection(WildcardPin, ResultValue);
		}
		else
		{
			// StructToCommandData is pure, so it has no exec pins and execution goes straight through.
			bWired &= Schema->TryCreateConnection(Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output),
				Result->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
			bWired &= Schema->TryCreateConnection(EntryValue, WildcardPin);
			bWired &= Schema->TryCreateConnection(Call->GetReturnValuePin(), ResultValue);
		}

		if (!bWired)
		{
			OutWarnings.Add(FString::Printf(
				TEXT("'%s': could not wire the conversion graph; the function would return nothing."), *FunctionName));
			return false;
		}
		return true;
	}

	/**
	 * Adds a function mapping an enum member to the string the SDK sends for it.
	 *
	 * This is the piece that makes the generated enums worth having: a graph picks `GemPack` from a
	 * dropdown, and the opaque id it purchases by never appears anywhere a person can mistype it.
	 *
	 * A Select node keyed on the enum carries the table. Its option pins are created one per member, in
	 * declaration order, which is why the emitter's member order and the mapping order have to be the same
	 * list — and are.
	 */
	bool AddEnumLookupFunction(UBlueprint* Blueprint, const FString& FunctionName, UEnum* Enum,
		const TArray<TPair<FString, FString>>& Members, TArray<FString>& OutWarnings)
	{
		if (!Enum || Members.IsEmpty())
		{
			return false;
		}
		if (FindObject<UEdGraph>(Blueprint, *FunctionName))
		{
			OutWarnings.Add(FString::Printf(TEXT("Function '%s' already exists; skipped."), *FunctionName));
			return false;
		}

		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint, FName(*FunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!Graph)
		{
			OutWarnings.Add(FString::Printf(TEXT("Could not create a graph for '%s'."), *FunctionName));
			return false;
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, /*bIsUserCreated*/ true, nullptr);

		UK2Node_FunctionEntry* Entry = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* AsEntry = Cast<UK2Node_FunctionEntry>(Node))
			{
				Entry = AsEntry;
				break;
			}
		}
		if (!Entry)
		{
			OutWarnings.Add(FString::Printf(TEXT("'%s': no function entry node was created."), *FunctionName));
			return false;
		}
		// Without this a generated function lands under Unreal's default category, away from the rest of
		// the SDK — findable only by knowing its name, which defeats the point of generating it.
		Entry->MetaData.Category = FText::FromString(LookupCategory);

		FEdGraphPinType EnumType;
		EnumType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		EnumType.PinSubCategoryObject = Enum;

		FEdGraphPinType StringType;
		StringType.PinCategory = UEdGraphSchema_K2::PC_String;

		UEdGraphPin* EntryValue = AddUserPin(Entry, TEXT("Value"), EnumType, EGPD_Output);

		FGraphNodeCreator<UK2Node_Select> SelectCreator(*Graph);
		UK2Node_Select* Select = SelectCreator.CreateNode();
		Select->NodePosX = Entry->NodePosX + 320;
		Select->NodePosY = Entry->NodePosY;
		SelectCreator.Finalize();
		// Creates one option pin per member and types the index pin; must happen before the options are
		// filled in, since it is what brings them into existence.
		Select->SetEnum(Enum, /*bForceRegenerate*/ true);

		FGraphNodeCreator<UK2Node_FunctionResult> ResultCreator(*Graph);
		UK2Node_FunctionResult* Result = ResultCreator.CreateNode();
		Result->NodePosX = Select->NodePosX + 380;
		Result->NodePosY = Entry->NodePosY;
		ResultCreator.Finalize();

		UEdGraphPin* ResultValue = AddUserPin(Result, TEXT("Id"), StringType, EGPD_Input);

		if (!EntryValue || !ResultValue || !Select->GetIndexPin())
		{
			OutWarnings.Add(FString::Printf(TEXT("'%s': could not build the lookup pins."), *FunctionName));
			return false;
		}

		// Connect everything first, then reconstruct, then fill the options in. That order is dictated by
		// how the node resolves itself, which is worth stating because guessing at it does not converge:
		//
		//   - the option pins do not exist until the *index* is connected to an enum;
		//   - `PostReconstructNode` is what types the return pin, and it takes that type from the pin the
		//     return is *linked to* — not from anything assigned to it directly;
		//   - the same pass then copies the return's type onto every option pin still marked wildcard.
		//
		// So assigning `PinType` by hand is not merely redundant, it is actively wrong: it makes the pin
		// non-wildcard, which is the exact condition that stops the fill from happening.
		const UEdGraphSchema* Schema = Graph->GetSchema();
		bool bWired = true;
		bWired &= Schema->TryCreateConnection(EntryValue, Select->GetIndexPin());
		bWired &= Schema->TryCreateConnection(Select->GetReturnValuePin(), ResultValue);
		bWired &= Schema->TryCreateConnection(Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output),
			Result->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
		if (!bWired)
		{
			OutWarnings.Add(FString::Printf(
				TEXT("'%s': could not wire the lookup graph; the function would return nothing."), *FunctionName));
			return false;
		}

		Select->ReconstructNode();

		// Every pin pointer taken before that call is dead — reconstruction replaces them.
		TArray<UEdGraphPin*> OptionPins;
		Select->GetOptionPins(OptionPins);
		if (OptionPins.Num() < Members.Num())
		{
			OutWarnings.Add(FString::Printf(
				TEXT("'%s': the enum offered %d options for %d members; the lookup would be incomplete."),
				*FunctionName, OptionPins.Num(), Members.Num()));
			return false;
		}
		for (int32 Index = 0; Index < Members.Num(); ++Index)
		{
			OptionPins[Index]->DefaultValue = Members[Index].Value;
		}
		return true;
	}

	FString PascalCase(const FString& Source)
	{
		FString Pascal;
		bool bUpperNext = true;
		for (const TCHAR Character : Source)
		{
			if (FChar::IsAlnum(Character))
			{
				Pascal.AppendChar(bUpperNext ? FChar::ToUpper(Character) : Character);
				bUpperNext = false;
			}
			else
			{
				bUpperNext = true;
			}
		}
		if (Pascal.IsEmpty())
		{
			return TEXT("Unnamed");
		}
		return FChar::IsDigit(Pascal[0]) ? TEXT("_") + Pascal : Pascal;
	}
}

FString FFlockFunctionLibraryEmitter::MakeTemplateIdFunctionName(const FString& TemplateName)
{
	return PascalCase(TemplateName) + TEXT("TemplateId");
}

FString FFlockFunctionLibraryEmitter::MakeConfigIdFunctionName(const FString& ConfigName)
{
	return PascalCase(ConfigName) + TEXT("ConfigId");
}

FString FFlockFunctionLibraryEmitter::MakeToStructFunctionName(const FString& EntityName)
{
	return TEXT("Read") + PascalCase(EntityName);
}

FString FFlockFunctionLibraryEmitter::MakeFromStructFunctionName(const FString& EntityName)
{
	// "Make … Update" rather than "Write …": this builds the body an update *would* send, it does not send
	// anything. Naming it Write would imply the row had already changed on the server.
	return TEXT("Make") + PascalCase(EntityName) + TEXT("Update");
}

FFlockFunctionLibraryEmitter::FEmitResult FFlockFunctionLibraryEmitter::BuildLibrary(
	const FFlockSchemaSnapshot& Snapshot, UObject* Outer, const TMap<FString, UUserDefinedStruct*>& StructsById,
	const TArray<FFlockEnumLookupSpec>& EnumLookups)
{
	FEmitResult Result;

	// Reused when it already exists, never recreated. Two reasons, and the first is not optional:
	// FKismetEditorUtilities::CreateBlueprint *asserts* when an asset of that name is already in the
	// Outer, so a second sync would take the editor down. The second is that graphs calling these
	// functions must keep working across a re-sync, which only holds if the asset keeps its identity.
	UBlueprint* Library = FindObject<UBlueprint>(Outer, LibraryAssetName);
	if (Library)
	{
		// Generated functions are wholly owned, so the previous set goes rather than accumulating; a
		// template deleted on the backend must lose its function here too.
		TArray<UEdGraph*> Existing = Library->FunctionGraphs;
		for (UEdGraph* Graph : Existing)
		{
			FBlueprintEditorUtils::RemoveGraph(Library, Graph);
		}
	}
	else
	{
		Library = FKismetEditorUtilities::CreateBlueprint(
			UBlueprintFunctionLibrary::StaticClass(), Outer, FName(LibraryAssetName),
			BPTYPE_FunctionLibrary, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}
	if (!Library)
	{
		Result.Warnings.Add(TEXT("Could not create the generated function library."));
		return Result;
	}

	// Names are de-duplicated across templates and configs together: they share one function namespace, so
	// a template and a config with the same name would otherwise collide.
	TSet<FString> UsedNames;
	auto AddUnique = [&](const FString& BaseName, const FString& Value)
	{
		FString Candidate = BaseName;
		int32 Suffix = 2;
		while (UsedNames.Contains(Candidate))
		{
			Candidate = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++);
		}
		UsedNames.Add(Candidate);
		if (AddConstantFunction(Library, Candidate, Value, Result.Warnings))
		{
			++Result.FunctionCount;
		}
	};

	// Conversions are added per entity alongside its id, so everything for one template sits together and
	// a name clash between an id and a conversion is impossible by construction.
	auto AddConversions = [&](const FString& EntityId, bool bWritable)
	{
		UUserDefinedStruct* const* Struct = StructsById.Find(EntityId);
		if (!Struct || !*Struct)
		{
			return;
		}
		// Named after the *struct*, not the entity. A template and a config can share a name ("Currencies"),
		// and naming from the entity produced `To Currencies` and `To Currencies 2` — a disambiguation that
		// says nothing about which is which. The struct name already carries the Template/Config suffix that
		// distinguishes them, so borrowing it removes the collision instead of papering over it.
		const FString EntityName = (*Struct)->GetName();
		FString ToName = MakeToStructFunctionName(EntityName);
		int32 Suffix = 2;
		while (UsedNames.Contains(ToName))
		{
			ToName = FString::Printf(TEXT("%s_%d"), *MakeToStructFunctionName(EntityName), Suffix++);
		}
		UsedNames.Add(ToName);
		if (AddConversionFunction(Library, ToName, *Struct, /*bDataToStruct*/ true, Result.Warnings))
		{
			Result.ReadFunctionByEntityId.Add(EntityId, ToName);
			++Result.FunctionCount;
		}

		// Only a player row can be written back. Game configs are game-wide and admin-only, so emitting a
		// write-side conversion for one would offer a call whose result the server always rejects.
		if (!bWritable)
		{
			return;
		}
		FString FromName = MakeFromStructFunctionName(EntityName);
		Suffix = 2;
		while (UsedNames.Contains(FromName))
		{
			FromName = FString::Printf(TEXT("%s_%d"), *MakeFromStructFunctionName(EntityName), Suffix++);
		}
		UsedNames.Add(FromName);
		if (AddConversionFunction(Library, FromName, *Struct, /*bDataToStruct*/ false, Result.Warnings))
		{
			Result.WriteFunctionByEntityId.Add(EntityId, FromName);
			++Result.FunctionCount;
		}
	};

	for (const FFlockPlayerTemplateSchema& Template : Snapshot.PlayerTemplates)
	{
		if (!Template.Id.IsEmpty())
		{
			AddUnique(MakeTemplateIdFunctionName(Template.Name), Template.Id);
		}
		AddConversions(Template.Id, /*bWritable*/ true);
	}
	for (const FFlockGameConfigSchema& Config : Snapshot.GameConfigs)
	{
		if (!Config.Id.IsEmpty())
		{
			AddUnique(MakeConfigIdFunctionName(Config.Name), Config.Id);
		}
		AddConversions(Config.Id, /*bWritable*/ false);
	}

	// The enum lookups last: they are game-wide rather than per entity, so they read best grouped at the
	// end of the library rather than interleaved with the per-template functions.
	for (const FFlockEnumLookupSpec& Lookup : EnumLookups)
	{
		if (!Lookup.Enum || Lookup.Members.IsEmpty() || UsedNames.Contains(Lookup.FunctionName))
		{
			continue;
		}
		UsedNames.Add(Lookup.FunctionName);
		if (AddEnumLookupFunction(Library, Lookup.FunctionName, Lookup.Enum, Lookup.Members, Result.Warnings))
		{
			Result.LookupEnumByFunctionName.Add(Lookup.FunctionName, Lookup.Enum);
			++Result.FunctionCount;
		}
	}

	// Compiled here rather than by the caller: an uncompiled library has no generated class, so nothing
	// could call its functions — and a graph that does not compile is a bug worth surfacing at emit time.
	FKismetEditorUtilities::CompileBlueprint(Library);
	if (Library->Status == BS_Error)
	{
		Result.Warnings.Add(TEXT("The generated function library did not compile cleanly."));
	}

	Result.Library = Library;
	return Result;
}

FFlockFunctionLibraryEmitter::FEmitResult FFlockFunctionLibraryEmitter::Emit(
	const FFlockSchemaSnapshot& Snapshot, const FString& ContentPath,
	const TMap<FString, UUserDefinedStruct*>& StructsById, const TArray<FFlockEnumLookupSpec>& EnumLookups,
	FString& OutError)
{
	FEmitResult Result;

	FString Root = ContentPath.TrimStartAndEnd();
	Root.RemoveFromEnd(TEXT("/"));
	if (Root.IsEmpty() || !Root.StartsWith(TEXT("/")))
	{
		OutError = FString::Printf(
			TEXT("Generated Content Path must be a package path such as /Game/Flock/Generated. Got '%s'."), *ContentPath);
		return Result;
	}

	const FString PackageName = FString::Printf(TEXT("%s/%s"), *Root, LibraryAssetName);
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Could not create package '%s'."), *PackageName);
		return Result;
	}
	Package->FullyLoad();

	Result = BuildLibrary(Snapshot, Package, StructsById, EnumLookups);
	if (!Result.IsValid())
	{
		OutError = TEXT("Could not build the generated function library.");
		return Result;
	}

	FAssetRegistryModule::AssetCreated(Result.Library);
	Result.Library->MarkPackageDirty();
	if (!UEditorLoadingAndSavingUtils::SavePackages({ Package }, /*bOnlyDirty*/ false))
	{
		OutError = FString::Printf(TEXT("Could not save '%s'."), *PackageName);
		return Result;
	}

	OutError.Reset();
	return Result;
}
