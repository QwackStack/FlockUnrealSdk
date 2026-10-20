// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"
#include "Misc/FlockEngineCompat.h"

#if WITH_AUTOMATION_TESTS

#include "Blueprint/FlockMultiplayerAsyncActions.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Multiplayer/FlockGameModes.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
	UPackage* TemporaryPackage(const TCHAR* Name)
	{
		return CreatePackage(*FString::Printf(TEXT("/Temp/%s_%s"), Name, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	const TArray<UClass*>& NodeClasses()
	{
		static const TArray<UClass*> Classes = {
			UFlockPartyAction::StaticClass(), UFlockGetMyPartyAction::StaticClass(), UFlockSessionAction::StaticClass(),
			UFlockGetMySessionAction::StaticClass(), UFlockFindMatchAction::StaticClass(), UFlockRequestJoinTokenAction::StaticClass(),
			UFlockVerifyJoinTokenAction::StaticClass(), UFlockFindDirectAddressAction::StaticClass(),
			UFlockWaitForConnectionAction::StaticClass(), UFlockTravelToSessionAction::StaticClass(),
		};
		return Classes;
	}

	/** Every multiplayer node: the static functions the editor offers as async nodes. */
	TArray<UFunction*> NodeFactories()
	{
		TArray<UFunction*> Found;
		for (UClass* Class : NodeClasses())
		{
			for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				if (It->HasAnyFunctionFlags(FUNC_Static) && It->HasMetaData(TEXT("BlueprintInternalUseOnly")))
				{
					Found.Add(*It);
				}
			}
		}
		return Found;
	}

	/** The spawner the editor's context menu places an async node with, for this factory: filed under the node's class. */
	UBlueprintNodeSpawner* AsyncNodeSpawnerFor(UFunction* Factory)
	{
		const FBlueprintActionDatabase::FActionList* Actions = FBlueprintActionDatabase::Get().GetAllActions().Find(FObjectKey(Factory->GetOwnerClass()));
		if (Actions != nullptr)
		{
			for (UBlueprintNodeSpawner* Spawner : *Actions)
			{
				const UBlueprintFunctionNodeSpawner* ForFunction = Cast<UBlueprintFunctionNodeSpawner>(Spawner);
				if (ForFunction != nullptr && ForFunction->GetFunction() == Factory && Spawner->NodeClass != nullptr
					&& Spawner->NodeClass->IsChildOf(UK2Node_AsyncAction::StaticClass()))
				{
					return Spawner;
				}
			}
		}
		return nullptr;
	}
}

/**
 * Every multiplayer node, placed in a Blueprint graph the way the editor's context menu places it and driven by an event so the
 * compiler compiles it: a pin a node declares wrongly (pins of different signatures, a parameter Blueprint cannot pass) fails
 * here, which no C++ test of the nodes can show. Each node has one exec pin per pin it fires, and only the four cancellable nodes
 * hand out the node itself, for Cancel.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockMultiplayerNodesInABlueprintTest, "Flock.Editor.MultiplayerBlueprint.EveryNodeIsOfferedAndCompilesInAGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFlockMultiplayerNodesInABlueprintTest::RunTest(const FString& Parameters)
{
	const TArray<UFunction*> Factories = NodeFactories();
	TestEqual(TEXT("every multiplayer node"), Factories.Num(), 24);

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), TemporaryPackage(TEXT("FlockMultiplayerNodes")),
		TEXT("BP_FlockMultiplayerNodes"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("precondition: a Blueprint"), Blueprint) || !TestTrue(TEXT("with an event graph"), Blueprint->UbergraphPages.Num() > 0))
	{
		return false;
	}
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	const UEdGraphSchema* Schema = Graph->GetSchema();
	int32 Placed = 0;
	for (UFunction* Factory : Factories)
	{
		const FString Name = Factory->GetName();
		UBlueprintNodeSpawner* Spawner = AsyncNodeSpawnerFor(Factory);
		if (!TestNotNull(*FString::Printf(TEXT("%s: offered as an async node"), *Name), Spawner))
		{
			continue;
		}
		UEdGraphNode* Node = Spawner->Invoke(Graph, IBlueprintNodeBinder::FBindingSet(), FVector2D(400.f, 300.f * Placed));
		if (!TestNotNull(*FString::Printf(TEXT("%s: placed"), *Name), Node))
		{
			continue;
		}
		FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*Graph);
		UK2Node_CustomEvent* Event = EventCreator.CreateNode();
		Event->CustomFunctionName = *FString::Printf(TEXT("Run%s"), *Name);
		EventCreator.Finalize();
		TestTrue(*FString::Printf(TEXT("%s: driven by an event"), *Name),
			Schema->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), Node->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)));
		const FString WorldContextInput = Factory->GetMetaData(FBlueprintMetadata::MD_WorldContext);
		const UEdGraphPin* WorldContextPin = WorldContextInput.IsEmpty() ? nullptr : Node->FindPin(*WorldContextInput, EGPD_Input);
		TestTrue(*FString::Printf(TEXT("%s: World Context filled with Self in an Actor, not a pin the game must wire"), *Name),
			WorldContextPin != nullptr && WorldContextPin->bHidden);

		UClass* NodeClass = CastField<FObjectProperty>(Factory->GetReturnProperty())->PropertyClass;
		for (TFieldIterator<FMulticastDelegateProperty> It(NodeClass); It; ++It)
		{
			UEdGraphPin* Pin = Node->FindPin(It->GetFName(), EGPD_Output);
			TestTrue(*FString::Printf(TEXT("%s: an exec pin for %s"), *Name, *It->GetName()),
				Pin != nullptr && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
		}
		const bool bHandsOutTheNode = Node->Pins.ContainsByPredicate([NodeClass](const UEdGraphPin* Pin)
		{
			return Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object
				&& Pin->PinType.PinSubCategoryObject == NodeClass;
		});
		TestEqual(*FString::Printf(TEXT("%s: hands out the node for Cancel only when it can be cancelled"), *Name), bHandsOutTheNode,
			NodeClass->IsChildOf(UCancellableAsyncAction::StaticClass()));
		++Placed;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestTrue(TEXT("the Blueprint compiles"), Blueprint->Status != BS_Error && Blueprint->GeneratedClass != nullptr);
	TestEqual(TEXT("every node placed"), Placed, Factories.Num());
	return true;
}

/**
 * A node run by the Blueprint VM, not called from C++: a Blueprint's event runs Flock Join Party, and each of the node's pins sets
 * a variable of its own. The Blueprint is an Actor, as a game places the node, so the editor fills World Context with Self; the
 * event runs on the class default object, which needs no world. With no Flock running in the editor the node answers at once on
 * its failure pin, so the graph after that pin runs and the graph after the success pin does not.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockMultiplayerNodeRunsInAGraphTest, "Flock.Editor.MultiplayerBlueprint.AGraphRunsANodeAndTheGraphAfterItsPin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFlockMultiplayerNodeRunsInAGraphTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), TemporaryPackage(TEXT("FlockMultiplayerRun")),
		TEXT("BP_FlockMultiplayerRun"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("precondition: a Blueprint"), Blueprint) || !TestTrue(TEXT("with an event graph"), Blueprint->UbergraphPages.Num() > 0))
	{
		return false;
	}
	FEdGraphPinType Flag;
	Flag.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	const FName FailureRan(TEXT("bFailurePinRan"));
	const FName SuccessRan(TEXT("bSuccessPinRan"));
	TestTrue(TEXT("variables added"), FBlueprintEditorUtils::AddMemberVariable(Blueprint, FailureRan, Flag)
		&& FBlueprintEditorUtils::AddMemberVariable(Blueprint, SuccessRan, Flag));

	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	const UEdGraphSchema* Schema = Graph->GetSchema();
	UFunction* Factory = UFlockPartyAction::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UFlockPartyAction, JoinParty));
	UBlueprintNodeSpawner* Spawner = Factory != nullptr ? AsyncNodeSpawnerFor(Factory) : nullptr;
	UEdGraphNode* Node = Spawner != nullptr ? Spawner->Invoke(Graph, IBlueprintNodeBinder::FBindingSet(), FVector2D(400.f, 0.f)) : nullptr;
	if (!TestNotNull(TEXT("Flock Join Party placed"), Node))
	{
		return false;
	}
	FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*Graph);
	UK2Node_CustomEvent* Event = EventCreator.CreateNode();
	Event->CustomFunctionName = TEXT("RunJoinParty");
	EventCreator.Finalize();
	TestTrue(TEXT("the event runs the node"),
		Schema->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), Node->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)));

	auto SetOnPin = [&](const TCHAR* PinName, const FName Variable, float Y)
	{
		FGraphNodeCreator<UK2Node_VariableSet> SetCreator(*Graph);
		UK2Node_VariableSet* Set = SetCreator.CreateNode();
		Set->VariableReference.SetSelfMember(Variable);
		Set->NodePosY = Y;
		SetCreator.Finalize();
		UEdGraphPin* Value = Set->FindPin(Variable, EGPD_Input);
		if (Value != nullptr)
		{
			Value->DefaultValue = TEXT("true");
		}
		return Value != nullptr && Schema->TryCreateConnection(Node->FindPin(PinName, EGPD_Output), Set->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
	};
	TestTrue(TEXT("the failure pin sets its variable"), SetOnPin(TEXT("OnFailure"), FailureRan, 200.f));
	TestTrue(TEXT("the success pin sets its variable"), SetOnPin(TEXT("OnSuccess"), SuccessRan, 400.f));

	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("the Blueprint compiles"), Blueprint->Status != BS_Error && Blueprint->GeneratedClass != nullptr))
	{
		return false;
	}
	UObject* Instance = Blueprint->GeneratedClass->GetDefaultObject();
	UFunction* Run = Instance->FindFunction(TEXT("RunJoinParty"));
	if (!TestNotNull(TEXT("the event"), Run))
	{
		return false;
	}
	Instance->ProcessEvent(Run, nullptr);
	const FBoolProperty* Failed = CastField<FBoolProperty>(Blueprint->GeneratedClass->FindPropertyByName(FailureRan));
	const FBoolProperty* Succeeded = CastField<FBoolProperty>(Blueprint->GeneratedClass->FindPropertyByName(SuccessRan));
	TestTrue(TEXT("the graph after the failure pin ran"), Failed != nullptr && Failed->GetPropertyValue_InContainer(Instance));
	TestTrue(TEXT("the graph after the success pin did not"), Succeeded != nullptr && !Succeeded->GetPropertyValue_InContainer(Instance));
	return true;
}

/**
 * Approve Joining Player overridden in a Blueprint based on Flock Game Mode Base, as the editor's Override menu makes it: the
 * game mode's own call of it (from C++, as Flock's check makes it) runs the Blueprint's graph, its return and Refused Reason
 * included. The class's own answer lets everyone in, so a refusal can only come from the Blueprint.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameModeBlueprintOverrideTest, "Flock.Editor.MultiplayerBlueprint.ABlueprintGameModeOverridesApproveJoiningPlayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFlockGameModeBlueprintOverrideTest::RunTest(const FString& Parameters)
{
	for (UClass* Parent : TArray<UClass*>{ AFlockGameModeBase::StaticClass(), AFlockGameMode::StaticClass() })
	{
		const FString Label = Parent->GetName();
		UFunction* Approve = Parent->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(AFlockGameModeBase, ApproveJoiningPlayer));
		if (!TestNotNull(*FString::Printf(TEXT("%s: has Approve Joining Player"), *Label), Approve))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s: offered for overriding"), *Label), UEdGraphSchema_K2::CanKismetOverrideFunction(Approve));

		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(Parent, TemporaryPackage(TEXT("FlockGameModeOverride")),
			*FString::Printf(TEXT("BP_%sOverride"), *Label), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, Approve->GetFName(), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph(Blueprint, Graph, /*bIsUserCreated*/ false, Parent);

		TArray<UK2Node_FunctionEntry*> Entries;
		TArray<UK2Node_FunctionResult*> Results;
		Graph->GetNodesOfClass(Entries);
		Graph->GetNodesOfClass(Results);
		if (!TestEqual(*FString::Printf(TEXT("%s: an entry"), *Label), Entries.Num(), 1))
		{
			continue;
		}
		UK2Node_FunctionResult* Result = Results.Num() > 0 ? Results[0] : nullptr;
		if (Result == nullptr)
		{
			FGraphNodeCreator<UK2Node_FunctionResult> ResultCreator(*Graph);
			Result = ResultCreator.CreateNode();
			ResultCreator.Finalize();
		}
		UEdGraphPin* EntryThen = Entries[0]->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* ResultExec = Result->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		if (EntryThen != nullptr && ResultExec != nullptr && !EntryThen->LinkedTo.Contains(ResultExec))
		{
			Graph->GetSchema()->TryCreateConnection(EntryThen, ResultExec);
		}
		UEdGraphPin* Returned = Result->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Input);
		UEdGraphPin* Reason = Result->FindPin(TEXT("RefusedReason"), EGPD_Input);
		if (!TestTrue(*FString::Printf(TEXT("%s: the result's pins"), *Label), Returned != nullptr && Reason != nullptr))
		{
			continue;
		}
		Returned->DefaultValue = TEXT("false");
		Reason->DefaultValue = TEXT("banned in the Blueprint");

		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		if (!TestTrue(*FString::Printf(TEXT("%s: the Blueprint compiles"), *Label), Blueprint->Status != BS_Error && Blueprint->GeneratedClass != nullptr))
		{
			continue;
		}
		FFlockJoiningPlayer Player;
		Player.PlayerId = TEXT("player-b");
		FString Refused;
		bool bApproved = true;
		if (AFlockGameModeBase* AsBase = Cast<AFlockGameModeBase>(Blueprint->GeneratedClass->GetDefaultObject()))
		{
			bApproved = AsBase->ApproveJoiningPlayer(Player, Refused);
		}
		else if (AFlockGameMode* AsMode = Cast<AFlockGameMode>(Blueprint->GeneratedClass->GetDefaultObject()))
		{
			bApproved = AsMode->ApproveJoiningPlayer(Player, Refused);
		}
		TestFalse(*FString::Printf(TEXT("%s: the Blueprint's answer, not the class's"), *Label), bApproved);
		TestEqualSensitive(*FString::Printf(TEXT("%s: with the Blueprint's reason"), *Label), Refused, FString(TEXT("banned in the Blueprint")));
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
