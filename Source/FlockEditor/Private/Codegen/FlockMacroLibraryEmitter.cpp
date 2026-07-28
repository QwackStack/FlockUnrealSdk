// Copyright 2022, Qwacks. All Rights Reserved.

#include "Codegen/FlockMacroLibraryEmitter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/FlockCommandAsyncActions.h"
#include "Blueprint/FlockConfigAsyncActions.h"
#include "Blueprint/FlockPlayerAsyncActions.h"
#include "Blueprint/FlockShopAsyncActions.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Http/FlockError.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Tunnel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Models/FlockPlayerModels.h"
#include "Models/FlockShopModels.h"
#include "Models/FlockStructuredData.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h"

const TCHAR* const FFlockMacroLibraryEmitter::LibraryAssetName = TEXT("FlockGeneratedMacros");
const TCHAR* const FFlockMacroLibraryEmitter::PurchaseMacroName = TEXT("Purchase");
const TCHAR* const FFlockMacroLibraryEmitter::UnlockAchievementMacroName = TEXT("UnlockAchievement");
const TCHAR* const FFlockMacroLibraryEmitter::AddFundsMacroName = TEXT("AddFunds");

namespace
{
	const TCHAR* const MacroCategory = TEXT("Flock|Generated");

	/** Pin names on the SDK's async action nodes, from their BlueprintAssignable delegates. */
	const TCHAR* const OnSuccessPin = TEXT("OnSuccess");
	const TCHAR* const OnFailurePin = TEXT("OnFailure");

	/** The macros' own tunnel pins. */
	const TCHAR* const InPin = TEXT("In");
	const TCHAR* const CompletedPin = TEXT("Completed");
	const TCHAR* const FailedPin = TEXT("Failed");
	const TCHAR* const StructPin = TEXT("Struct");
	const TCHAR* const RowIdPin = TEXT("RowId");
	const TCHAR* const ErrorPin = TEXT("Error");

	/** The template tag whose row holds the player's wallet — see FindCurrencyTemplateId. */
	const TCHAR* const CurrencyTag = TEXT("currency");

	/** The generated lookup functions, by the names the function library emitter gave them. */
	const TCHAR* const ShopItemLookup = TEXT("ShopItemId");
	const TCHAR* const CurrencyLookup = TEXT("CurrencyName");
	const TCHAR* const AchievementLookup = TEXT("AchievementName");

	FEdGraphPinType ExecType()
	{
		FEdGraphPinType Pin;
		Pin.PinCategory = UEdGraphSchema_K2::PC_Exec;
		return Pin;
	}

	FEdGraphPinType StringType()
	{
		FEdGraphPinType Pin;
		Pin.PinCategory = UEdGraphSchema_K2::PC_String;
		return Pin;
	}

	FEdGraphPinType IntType()
	{
		FEdGraphPinType Pin;
		Pin.PinCategory = UEdGraphSchema_K2::PC_Int;
		return Pin;
	}

	FEdGraphPinType StructType(UScriptStruct* Struct)
	{
		FEdGraphPinType Pin;
		Pin.PinCategory = UEdGraphSchema_K2::PC_Struct;
		Pin.PinSubCategoryObject = Struct;
		return Pin;
	}

	FEdGraphPinType EnumType(UEnum* Enum)
	{
		FEdGraphPinType Pin;
		Pin.PinCategory = UEdGraphSchema_K2::PC_Byte;
		Pin.PinSubCategoryObject = Enum;
		return Pin;
	}

	/**
	 * Points an async-action node at its factory.
	 *
	 * Set reflectively because `ProxyFactoryClass` and friends are **protected** on
	 * `UK2Node_BaseAsyncTask` — the engine only ever writes them from inside the node's own menu-action
	 * lambda, so there is no public way in from an emitter. They are UPROPERTYs, so reflection is a
	 * supported route rather than a trick; it just has to be checked rather than assumed, hence the bool.
	 */
	bool ConfigureAsyncNode(UK2Node_AsyncAction* Node, UClass* ProxyClass, const TCHAR* FactoryFunctionName)
	{
		if (!Node || !ProxyClass)
		{
			return false;
		}
		UClass* NodeClass = Node->GetClass();

		FNameProperty* FactoryName = CastField<FNameProperty>(
			NodeClass->FindPropertyByName(TEXT("ProxyFactoryFunctionName")));
		FObjectPropertyBase* FactoryClass = CastField<FObjectPropertyBase>(
			NodeClass->FindPropertyByName(TEXT("ProxyFactoryClass")));
		FObjectPropertyBase* Proxy = CastField<FObjectPropertyBase>(
			NodeClass->FindPropertyByName(TEXT("ProxyClass")));
		if (!FactoryName || !FactoryClass || !Proxy)
		{
			return false;
		}

		FactoryName->SetPropertyValue_InContainer(Node, FName(FactoryFunctionName));
		FactoryClass->SetObjectPropertyValue_InContainer(Node, ProxyClass);
		Proxy->SetObjectPropertyValue_InContainer(Node, ProxyClass);
		return true;
	}

	/** The entry/exit tunnels a macro graph is created with. */
	void FindTunnels(UEdGraph* Graph, UK2Node_Tunnel*& OutEntry, UK2Node_Tunnel*& OutExit)
	{
		OutEntry = nullptr;
		OutExit = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
			if (!Tunnel)
			{
				continue;
			}
			// The entry tunnel exposes the macro's *inputs* as its outputs, and vice versa.
			if (Tunnel->bCanHaveOutputs)
			{
				OutEntry = Tunnel;
			}
			else if (Tunnel->bCanHaveInputs)
			{
				OutExit = Tunnel;
			}
		}
	}

	/**
	 * One macro graph under construction.
	 *
	 * Exists because six macros repeat the same five-step dance — create the graph, find the tunnels,
	 * declare pins, place nodes, connect — and the failure handling around each step is the part that
	 * matters. Every connection is null-checked: `TryCreateConnection` dereferences its arguments, so a
	 * pin whose name has moved on an engine or SDK node crashes the editor rather than failing the sync.
	 */
	struct FMacroGraph
	{
		FMacroGraph(UBlueprint* InBlueprint, const FString& InName, TArray<FString>& InWarnings)
			: Name(InName)
			, Warnings(&InWarnings)
		{
			if (FindObject<UEdGraph>(InBlueprint, *InName))
			{
				Warn(TEXT("a macro of that name already exists; skipped"));
				return;
			}
			Graph = FBlueprintEditorUtils::CreateNewGraph(
				InBlueprint, FName(*InName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (!Graph)
			{
				Warn(TEXT("could not create the graph"));
				return;
			}
			FBlueprintEditorUtils::AddMacroGraph(InBlueprint, Graph, /*bIsUserCreated*/ true, nullptr);

			FindTunnels(Graph, Entry, Exit);
			if (!Entry || !Exit)
			{
				Warn(TEXT("the macro graph has no tunnels"));
				return;
			}
			// Without this the macro lands in Unreal's default category, away from the rest of the SDK.
			Entry->MetaData.Category = FText::FromString(MacroCategory);
			bOk = true;
		}

		bool IsOk() const { return bOk; }

		void Warn(const TCHAR* What)
		{
			Warnings->Add(FString::Printf(TEXT("'%s': %s."), *Name, What));
			bOk = false;
		}

		/**
		 * Declares a macro input. These are *output* pins on the entry tunnel — a tunnel is seen from
		 * inside the graph, where the macro's inputs are things flowing out of the entry.
		 *
		 * A macro tunnel starts with no pins at all, unlike a function entry which arrives with `then`
		 * already on it, so even the exec input has to be made here.
		 */
		UEdGraphPin* In(const TCHAR* PinName, const FEdGraphPinType& Type)
		{
			UEdGraphPin* Pin = Entry ? Entry->CreateUserDefinedPin(FName(PinName), Type, EGPD_Output) : nullptr;
			if (!Pin)
			{
				Warn(TEXT("could not create an input pin"));
			}
			return Pin;
		}

		/** Declares a macro output — an input pin on the exit tunnel. */
		UEdGraphPin* Out(const TCHAR* PinName, const FEdGraphPinType& Type)
		{
			UEdGraphPin* Pin = Exit ? Exit->CreateUserDefinedPin(FName(PinName), Type, EGPD_Input) : nullptr;
			if (!Pin)
			{
				Warn(TEXT("could not create an output pin"));
			}
			return Pin;
		}

		/** Places an async fetch/command node, wired to its factory. */
		UK2Node_AsyncAction* AsyncNode(UClass* ProxyClass, const TCHAR* FactoryFunctionName, int32 Column)
		{
			if (!bOk)
			{
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_AsyncAction> Creator(*Graph);
			UK2Node_AsyncAction* Node = Creator.CreateNode();
			if (!ConfigureAsyncNode(Node, ProxyClass, FactoryFunctionName))
			{
				Warn(TEXT("could not configure the async node"));
				Creator.Finalize();
				return nullptr;
			}
			Place(Node, Column);
			Creator.Finalize();
			return Node;
		}

		/** Places a call to a generated function-library function. */
		UK2Node_CallFunction* CallNode(UFunction* Function, int32 Column)
		{
			if (!bOk || !Function)
			{
				Warn(TEXT("a generated function it calls is missing"));
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
			UK2Node_CallFunction* Node = Creator.CreateNode();
			Node->SetFromFunction(Function);
			Place(Node, Column);
			Creator.Finalize();
			return Node;
		}

		/** Places a Break node over a struct, so a macro can reach one member of a fetched row. */
		UK2Node_BreakStruct* BreakNode(UScriptStruct* Struct, int32 Column)
		{
			if (!bOk)
			{
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_BreakStruct> Creator(*Graph);
			UK2Node_BreakStruct* Node = Creator.CreateNode();
			Node->StructType = Struct;
			// Opts out of the legacy "break every member" fixup; without it the node is treated as
			// pre-4.24 content and logs a deprecation on every compile.
			Node->bMadeAfterOverridePinRemoval = true;
			Place(Node, Column);
			Creator.Finalize();
			return Node;
		}

		/**
		 * The struct input on a Break node.
		 *
		 * Found by category rather than by name: `UK2Node_BreakStruct` names that pin after the struct
		 * itself, which makes a by-name lookup a silent dependency on a runtime type's spelling.
		 */
		UEdGraphPin* BreakInput(UK2Node_BreakStruct* Node)
		{
			if (!Node)
			{
				return nullptr;
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
				{
					return Pin;
				}
			}
			return nullptr;
		}

		void Connect(UEdGraphPin* From, UEdGraphPin* To, const TCHAR* What)
		{
			if (!bOk)
			{
				return;
			}
			if (!From || !To)
			{
				Warnings->Add(FString::Printf(TEXT("'%s': missing pin for %s."), *Name, What));
				bOk = false;
				return;
			}
			if (!Graph->GetSchema()->TryCreateConnection(From, To))
			{
				Warnings->Add(FString::Printf(TEXT("'%s': could not connect %s."), *Name, What));
				bOk = false;
			}
		}

		/** Bakes a literal onto an input pin of a node inside the graph. */
		void Bake(UEdGraphPin* Pin, const FString& Value, const TCHAR* What)
		{
			if (!bOk)
			{
				return;
			}
			if (!Pin)
			{
				Warnings->Add(FString::Printf(TEXT("'%s': missing pin for %s."), *Name, What));
				bOk = false;
				return;
			}
			Pin->DefaultValue = Value;
		}

		/** Reports whether the graph is wired; the caller counts only what came back true. */
		bool Finish()
		{
			if (!bOk)
			{
				Warnings->Add(FString::Printf(
					TEXT("'%s': the macro graph could not be wired, so it would do nothing."), *Name));
			}
			return bOk;
		}

		FString Name;
		TArray<FString>* Warnings = nullptr;
		UEdGraph* Graph = nullptr;
		UK2Node_Tunnel* Entry = nullptr;
		UK2Node_Tunnel* Exit = nullptr;

	private:
		void Place(UEdGraphNode* Node, int32 Column) const
		{
			Node->NodePosX = Entry->NodePosX + 320 + (Column * 400);
			Node->NodePosY = Entry->NodePosY;
		}

		bool bOk = false;
	};

	/** The struct a generated read/write function takes or returns, read off the function's own signature. */
	UUserDefinedStruct* FindStructParam(UFunction* Function)
	{
		if (!Function)
		{
			return nullptr;
		}
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (const FStructProperty* AsStruct = CastField<FStructProperty>(*It))
			{
				if (UUserDefinedStruct* Generated = Cast<UUserDefinedStruct>(AsStruct->Struct))
				{
					return Generated;
				}
			}
		}
		return nullptr;
	}

	/**
	 * Emits `Get<Config>`: fetch this config by its baked id, convert the result into its generated struct,
	 * and hand both exec paths back out.
	 */
	bool AddConfigGetMacro(UBlueprint* Blueprint, const FString& MacroName, const FString& ConfigId,
		UFunction* ReadFunction, UUserDefinedStruct* Struct, TArray<FString>& OutWarnings)
	{
		if (!ReadFunction || !Struct)
		{
			return false;
		}
		FMacroGraph M(Blueprint, MacroName, OutWarnings);
		if (!M.IsOk())
		{
			return false;
		}

		UK2Node_AsyncAction* Fetch = M.AsyncNode(
			UFlockResolveConfigDataAction::StaticClass(), TEXT("ResolveConfigData"), 0);
		UK2Node_CallFunction* Read = M.CallNode(ReadFunction, 1);

		UEdGraphPin* ExecIn = M.In(InPin, ExecType());
		UEdGraphPin* CompletedOut = M.Out(CompletedPin, ExecType());
		UEdGraphPin* FailedOut = M.Out(FailedPin, ExecType());
		UEdGraphPin* StructOut = M.Out(StructPin, StructType(Struct));
		UEdGraphPin* ErrorOut = M.Out(ErrorPin, StructType(FFlockError::StaticStruct()));
		if (!M.IsOk())
		{
			return M.Finish();
		}

		// The id is baked here — this is precisely the thing a caller should never have to supply.
		M.Bake(Fetch->FindPin(TEXT("ConfigId")), ConfigId, TEXT("the config id"));

		M.Connect(ExecIn, Fetch->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input), TEXT("entry -> fetch"));
		M.Connect(Fetch->FindPin(OnSuccessPin, EGPD_Output),
			Read->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input), TEXT("fetch success -> read"));
		M.Connect(Fetch->FindPin(TEXT("Data"), EGPD_Output), Read->FindPin(TEXT("Data"), EGPD_Input),
			TEXT("fetched data -> read"));
		M.Connect(Read->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), CompletedOut, TEXT("read -> Completed"));
		M.Connect(Read->FindPin(StructPin, EGPD_Output), StructOut, TEXT("read struct -> Struct"));
		M.Connect(Fetch->FindPin(OnFailurePin, EGPD_Output), FailedOut, TEXT("fetch failure -> Failed"));
		M.Connect(Fetch->FindPin(ErrorPin, EGPD_Output), ErrorOut, TEXT("fetch error -> Error"));
		return M.Finish();
	}

	/**
	 * Emits `Get<Template>`: resolve the signed-in player's row for this template, convert it, and hand
	 * back the typed struct **and the row id**.
	 *
	 * The row id is an output because `Save<Template>` needs it and nothing else can supply it — it is
	 * per-player, and only this fetch knows it.
	 */
	bool AddTemplateGetMacro(UBlueprint* Blueprint, const FString& MacroName, const FString& TemplateId,
		UFunction* ReadFunction, UUserDefinedStruct* Struct, TArray<FString>& OutWarnings)
	{
		if (!ReadFunction || !Struct)
		{
			return false;
		}
		FMacroGraph M(Blueprint, MacroName, OutWarnings);
		if (!M.IsOk())
		{
			return false;
		}

		UK2Node_AsyncAction* Fetch = M.AsyncNode(
			UFlockGetMyPlayerDataAction::StaticClass(), TEXT("GetMyDataByTemplate"), 0);
		// The fetch answers with a whole row; the conversion takes only its data handle, and the caller
		// needs its id. Breaking it is what separates the two.
		UK2Node_BreakStruct* Break = M.BreakNode(FFlockPlayerData::StaticStruct(), 1);
		UK2Node_CallFunction* Read = M.CallNode(ReadFunction, 2);

		UEdGraphPin* ExecIn = M.In(InPin, ExecType());
		UEdGraphPin* CompletedOut = M.Out(CompletedPin, ExecType());
		UEdGraphPin* FailedOut = M.Out(FailedPin, ExecType());
		UEdGraphPin* StructOut = M.Out(StructPin, StructType(Struct));
		UEdGraphPin* RowIdOut = M.Out(RowIdPin, StringType());
		UEdGraphPin* ErrorOut = M.Out(ErrorPin, StructType(FFlockError::StaticStruct()));
		if (!M.IsOk())
		{
			return M.Finish();
		}

		M.Bake(Fetch->FindPin(TEXT("PlayerTemplateId")), TemplateId, TEXT("the template id"));

		M.Connect(ExecIn, Fetch->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input), TEXT("entry -> fetch"));
		M.Connect(Fetch->FindPin(TEXT("Data"), EGPD_Output), M.BreakInput(Break), TEXT("fetched row -> break"));
		M.Connect(Fetch->FindPin(OnSuccessPin, EGPD_Output),
			Read->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input), TEXT("fetch success -> read"));
		M.Connect(Break->FindPin(TEXT("Data"), EGPD_Output), Read->FindPin(TEXT("Data"), EGPD_Input),
			TEXT("row data -> read"));
		M.Connect(Break->FindPin(TEXT("Id"), EGPD_Output), RowIdOut, TEXT("row id -> RowId"));
		M.Connect(Read->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), CompletedOut, TEXT("read -> Completed"));
		M.Connect(Read->FindPin(StructPin, EGPD_Output), StructOut, TEXT("read struct -> Struct"));
		M.Connect(Fetch->FindPin(OnFailurePin, EGPD_Output), FailedOut, TEXT("fetch failure -> Failed"));
		M.Connect(Fetch->FindPin(ErrorPin, EGPD_Output), ErrorOut, TEXT("fetch error -> Error"));
		return M.Finish();
	}

	/**
	 * Emits `Save<Template>`: turn the typed struct back into a command body and write it to the row.
	 *
	 * Hands back no struct. The update's response row is authoritative, so re-emitting it would look like
	 * a free refresh — but it is one the caller did not ask for, and a graph that trusts it starts using
	 * write responses to observe changes made elsewhere.
	 */
	bool AddTemplateSaveMacro(UBlueprint* Blueprint, const FString& MacroName, UFunction* WriteFunction,
		UUserDefinedStruct* Struct, TArray<FString>& OutWarnings)
	{
		if (!WriteFunction || !Struct)
		{
			return false;
		}
		FMacroGraph M(Blueprint, MacroName, OutWarnings);
		if (!M.IsOk())
		{
			return false;
		}

		UK2Node_CallFunction* Write = M.CallNode(WriteFunction, 0);
		UK2Node_AsyncAction* Update = M.AsyncNode(
			UFlockUpdatePlayerDataAction::StaticClass(), TEXT("UpdatePlayerData"), 1);

		UEdGraphPin* ExecIn = M.In(InPin, ExecType());
		UEdGraphPin* StructIn = M.In(StructPin, StructType(Struct));
		UEdGraphPin* RowIdIn = M.In(RowIdPin, StringType());
		UEdGraphPin* CompletedOut = M.Out(CompletedPin, ExecType());
		UEdGraphPin* FailedOut = M.Out(FailedPin, ExecType());
		UEdGraphPin* ErrorOut = M.Out(ErrorPin, StructType(FFlockError::StaticStruct()));
		if (!M.IsOk())
		{
			return M.Finish();
		}

		// `Make…Update` is pure, so execution runs entry -> update and the conversion happens on the data
		// edge. The update's own `Data` input is the command body, which is why both ends are named Data.
		M.Connect(ExecIn, Write->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input), TEXT("entry -> convert"));
		M.Connect(Write->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output),
			Update->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input), TEXT("convert -> update"));
		M.Connect(StructIn, Write->FindPin(StructPin, EGPD_Input), TEXT("Struct -> convert"));
		M.Connect(Write->FindPin(TEXT("Data"), EGPD_Output), Update->FindPin(TEXT("Data"), EGPD_Input),
			TEXT("command body -> update"));
		M.Connect(RowIdIn, Update->FindPin(TEXT("PlayerDataId")), TEXT("RowId -> update"));
		M.Connect(Update->FindPin(OnSuccessPin, EGPD_Output), CompletedOut, TEXT("update success -> Completed"));
		M.Connect(Update->FindPin(OnFailurePin, EGPD_Output), FailedOut, TEXT("update failure -> Failed"));
		M.Connect(Update->FindPin(ErrorPin, EGPD_Output), ErrorOut, TEXT("update error -> Error"));
		return M.Finish();
	}

	/**
	 * Emits a command macro: a generated enum in, its wire value looked up, the SDK's command node run.
	 *
	 * One macro per family rather than one per shop item. The enum pin is what makes that work — the
	 * dropdown is still typed and still un-mistypeable, but the node count does not track the catalog.
	 *
	 * `ExtraInputPin`/`ExtraInputType` carry the one command that takes more than an identifier (funds
	 * take an amount); `BakedPinName`/`BakedValue` carry the one that has something to bake beyond it.
	 */
	bool AddCommandMacro(UBlueprint* Blueprint, const FString& MacroName, UFunction* LookupFunction, UEnum* Enum,
		const TCHAR* EnumPinName, UClass* ProxyClass, const TCHAR* FactoryFunctionName, const TCHAR* IdPinName,
		const TCHAR* ResultPinName, UScriptStruct* ResultStruct, const TCHAR* ExtraInputPin,
		const FEdGraphPinType* ExtraInputType, const TCHAR* BakedPinName, const FString& BakedValue,
		TArray<FString>& OutWarnings)
	{
		if (!LookupFunction || !Enum)
		{
			return false;
		}
		FMacroGraph M(Blueprint, MacroName, OutWarnings);
		if (!M.IsOk())
		{
			return false;
		}

		UK2Node_CallFunction* Lookup = M.CallNode(LookupFunction, 0);
		UK2Node_AsyncAction* Command = M.AsyncNode(ProxyClass, FactoryFunctionName, 1);

		UEdGraphPin* ExecIn = M.In(InPin, ExecType());
		UEdGraphPin* EnumIn = M.In(EnumPinName, EnumType(Enum));
		UEdGraphPin* ExtraIn = ExtraInputPin && ExtraInputType ? M.In(ExtraInputPin, *ExtraInputType) : nullptr;
		UEdGraphPin* CompletedOut = M.Out(CompletedPin, ExecType());
		UEdGraphPin* FailedOut = M.Out(FailedPin, ExecType());
		UEdGraphPin* ResultOut = M.Out(ResultPinName, StructType(ResultStruct));
		UEdGraphPin* ErrorOut = M.Out(ErrorPin, StructType(FFlockError::StaticStruct()));
		if (!M.IsOk())
		{
			return M.Finish();
		}

		if (BakedPinName && !BakedValue.IsEmpty())
		{
			M.Bake(Command->FindPin(BakedPinName), BakedValue, TEXT("a baked id"));
		}

		M.Connect(ExecIn, Lookup->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input), TEXT("entry -> lookup"));
		M.Connect(EnumIn, Lookup->FindPin(TEXT("Value"), EGPD_Input), TEXT("picked member -> lookup"));
		M.Connect(Lookup->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output),
			Command->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input), TEXT("lookup -> command"));
		M.Connect(Lookup->FindPin(TEXT("Id"), EGPD_Output), Command->FindPin(IdPinName, EGPD_Input),
			TEXT("looked-up id -> command"));
		if (ExtraIn)
		{
			M.Connect(ExtraIn, Command->FindPin(ExtraInputPin, EGPD_Input), TEXT("the extra argument"));
		}
		M.Connect(Command->FindPin(OnSuccessPin, EGPD_Output), CompletedOut, TEXT("command success -> Completed"));
		M.Connect(Command->FindPin(OnFailurePin, EGPD_Output), FailedOut, TEXT("command failure -> Failed"));
		M.Connect(Command->FindPin(ResultPinName, EGPD_Output), ResultOut, TEXT("command result -> Result"));
		M.Connect(Command->FindPin(ErrorPin, EGPD_Output), ErrorOut, TEXT("command error -> Error"));
		return M.Finish();
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

	/**
	 * Reserves a macro name, disambiguating by *kind* before falling back to a number.
	 *
	 * A template and a config can share a name, and `Get Currencies` / `Get Currencies_2` says nothing
	 * about which is which. `Get Currencies` / `Get CurrenciesTemplate` does.
	 */
	FString ReserveName(const FString& Base, const TCHAR* KindSuffix, TSet<FString>& UsedNames)
	{
		FString Candidate = Base;
		if (UsedNames.Contains(Candidate))
		{
			Candidate = Base + KindSuffix;
		}
		int32 Suffix = 2;
		while (UsedNames.Contains(Candidate))
		{
			Candidate = FString::Printf(TEXT("%s%s_%d"), *Base, KindSuffix, Suffix++);
		}
		UsedNames.Add(Candidate);
		return Candidate;
	}

	/**
	 * The template whose row holds the player's wallet.
	 *
	 * Baking its id lets `Add Funds` skip the runtime tag scan. Absent, the macro leaves the pin empty,
	 * which is the SDK's own "resolve it by tag" path — a slower call, not a broken one.
	 */
	FString FindCurrencyTemplateId(const FFlockSchemaSnapshot& Snapshot)
	{
		for (const FFlockPlayerTemplateSchema& Template : Snapshot.PlayerTemplates)
		{
			if (Template.Tag.Equals(CurrencyTag, ESearchCase::IgnoreCase))
			{
				return Template.Id;
			}
		}
		return FString();
	}
}

FString FFlockMacroLibraryEmitter::MakeGetMacroName(const FString& EntityName)
{
	return TEXT("Get") + PascalCase(EntityName);
}

FString FFlockMacroLibraryEmitter::MakeSaveMacroName(const FString& EntityName)
{
	return TEXT("Save") + PascalCase(EntityName);
}

FFlockMacroLibraryEmitter::FEmitResult FFlockMacroLibraryEmitter::BuildLibrary(
	const FFlockSchemaSnapshot& Snapshot, UObject* Outer,
	const FFlockFunctionLibraryEmitter::FEmitResult& Functions)
{
	FEmitResult Result;
	UBlueprint* FunctionLibrary = Functions.Library;
	if (!FunctionLibrary || !FunctionLibrary->GeneratedClass)
	{
		Result.Warnings.Add(TEXT("The generated function library is unavailable, so no macros were emitted."));
		return Result;
	}
	UClass* const Generated = FunctionLibrary->GeneratedClass;

	// Reused rather than recreated, for the same two reasons as the function library: CreateBlueprint
	// asserts on an existing asset, and graphs calling these macros must survive a re-sync.
	UBlueprint* Library = FindObject<UBlueprint>(Outer, LibraryAssetName);
	if (Library)
	{
		TArray<UEdGraph*> Existing = Library->MacroGraphs;
		for (UEdGraph* Graph : Existing)
		{
			FBlueprintEditorUtils::RemoveGraph(Library, Graph);
		}
	}
	else
	{
		// AActor, not UObject: the SDK's async nodes take a world context filled from `self`, which only
		// resolves in an actor context. See the header.
		Library = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Outer, FName(LibraryAssetName),
			BPTYPE_MacroLibrary, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}
	if (!Library)
	{
		Result.Warnings.Add(TEXT("Could not create the generated macro library."));
		return Result;
	}

	TSet<FString> UsedNames;

	// Templates first: they are the read-modify-write half, and their Get/Save pair is what the whole
	// tier is for. Configs are read-only and follow.
	for (const FFlockPlayerTemplateSchema& Template : Snapshot.PlayerTemplates)
	{
		const FString* ReadName = Functions.ReadFunctionByEntityId.Find(Template.Id);
		if (Template.Id.IsEmpty() || !ReadName)
		{
			continue;
		}
		UFunction* ReadFunction = Generated->FindFunctionByName(FName(**ReadName));
		if (!ReadFunction)
		{
			Result.Warnings.Add(FString::Printf(
				TEXT("'%s': the generated read function is missing, so no macro was emitted."), *Template.Name));
			continue;
		}
		// The struct comes off the generated function's own signature, so a macro cannot disagree with it
		// about the type.
		UUserDefinedStruct* Struct = FindStructParam(ReadFunction);

		const FString GetName = ReserveName(MakeGetMacroName(Template.Name), TEXT("Template"), UsedNames);
		if (AddTemplateGetMacro(Library, GetName, Template.Id, ReadFunction, Struct, Result.Warnings))
		{
			++Result.MacroCount;
		}

		// Save is emitted only where the write-side conversion exists. A template whose fields were all
		// skipped has no update to build, and a Save that sends nothing is worse than no Save at all.
		const FString* WriteName = Functions.WriteFunctionByEntityId.Find(Template.Id);
		if (!WriteName)
		{
			continue;
		}
		UFunction* WriteFunction = Generated->FindFunctionByName(FName(**WriteName));
		const FString SaveName = ReserveName(MakeSaveMacroName(Template.Name), TEXT("Template"), UsedNames);
		if (AddTemplateSaveMacro(Library, SaveName, WriteFunction, Struct, Result.Warnings))
		{
			++Result.MacroCount;
		}
	}

	for (const FFlockGameConfigSchema& Config : Snapshot.GameConfigs)
	{
		const FString* ReadName = Functions.ReadFunctionByEntityId.Find(Config.Id);
		if (Config.Id.IsEmpty() || !ReadName)
		{
			continue;
		}
		UFunction* ReadFunction = Generated->FindFunctionByName(FName(**ReadName));
		if (!ReadFunction)
		{
			Result.Warnings.Add(FString::Printf(
				TEXT("'%s': the generated read function is missing, so no macro was emitted."), *Config.Name));
			continue;
		}
		UUserDefinedStruct* Struct = FindStructParam(ReadFunction);

		const FString MacroName = ReserveName(MakeGetMacroName(Config.Name), TEXT("Config"), UsedNames);
		if (AddConfigGetMacro(Library, MacroName, Config.Id, ReadFunction, Struct, Result.Warnings))
		{
			++Result.MacroCount;
		}
	}

	// The command macros: one per family, each keyed by the enum the enum emitter produced. Emitted only
	// where that enum exists — a game with no shop gets no Purchase node rather than one with an empty
	// dropdown.
	auto FindLookup = [&](const TCHAR* LookupName) -> TPair<UFunction*, UEnum*>
	{
		UEnum* const* Enum = Functions.LookupEnumByFunctionName.Find(LookupName);
		if (!Enum || !*Enum)
		{
			return TPair<UFunction*, UEnum*>(nullptr, nullptr);
		}
		return TPair<UFunction*, UEnum*>(Generated->FindFunctionByName(FName(LookupName)), *Enum);
	};

	const TPair<UFunction*, UEnum*> ShopItems = FindLookup(ShopItemLookup);
	if (ShopItems.Key && !UsedNames.Contains(PurchaseMacroName))
	{
		UsedNames.Add(PurchaseMacroName);
		if (AddCommandMacro(Library, PurchaseMacroName, ShopItems.Key, ShopItems.Value, TEXT("Item"),
			UFlockPurchaseAction::StaticClass(), TEXT("Purchase"), TEXT("ShopItemId"),
			TEXT("Entry"), FFlockPlayerInventory::StaticStruct(),
			/*ExtraInputPin*/ nullptr, /*ExtraInputType*/ nullptr,
			/*BakedPinName*/ nullptr, FString(), Result.Warnings))
		{
			++Result.MacroCount;
		}
	}

	const TPair<UFunction*, UEnum*> Achievements = FindLookup(AchievementLookup);
	if (Achievements.Key && !UsedNames.Contains(UnlockAchievementMacroName))
	{
		UsedNames.Add(UnlockAchievementMacroName);
		if (AddCommandMacro(Library, UnlockAchievementMacroName, Achievements.Key, Achievements.Value,
			TEXT("Achievement"), UFlockUnlockAchievementAction::StaticClass(), TEXT("UnlockAchievement"),
			TEXT("AchievementName"), TEXT("Data"), FFlockPlayerData::StaticStruct(),
			/*ExtraInputPin*/ nullptr, /*ExtraInputType*/ nullptr,
			/*BakedPinName*/ nullptr, FString(), Result.Warnings))
		{
			++Result.MacroCount;
		}
	}

	const TPair<UFunction*, UEnum*> Currencies = FindLookup(CurrencyLookup);
	if (Currencies.Key && !UsedNames.Contains(AddFundsMacroName))
	{
		UsedNames.Add(AddFundsMacroName);
		const FEdGraphPinType AmountType = IntType();
		if (AddCommandMacro(Library, AddFundsMacroName, Currencies.Key, Currencies.Value, TEXT("Currency"),
			UFlockAddGameFundsAction::StaticClass(), TEXT("AddGameFunds"), TEXT("Currency"),
			TEXT("Data"), FFlockPlayerData::StaticStruct(),
			TEXT("Amount"), &AmountType,
			TEXT("CurrencyTemplateId"), FindCurrencyTemplateId(Snapshot), Result.Warnings))
		{
			++Result.MacroCount;
		}
	}

	FKismetEditorUtilities::CompileBlueprint(Library);
	if (Library->Status == BS_Error)
	{
		Result.Warnings.Add(TEXT("The generated macro library did not compile cleanly."));
	}

	Result.Library = Library;
	return Result;
}

FFlockMacroLibraryEmitter::FEmitResult FFlockMacroLibraryEmitter::Emit(const FFlockSchemaSnapshot& Snapshot,
	const FString& ContentPath, const FFlockFunctionLibraryEmitter::FEmitResult& Functions, FString& OutError)
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

	Result = BuildLibrary(Snapshot, Package, Functions);
	if (!Result.IsValid())
	{
		OutError = TEXT("Could not build the generated macro library.");
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
