// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Blueprint/BlueprintExceptionInfo.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"
#include "UObject/Script.h"
#include "UObject/Stack.h"

namespace
{
	const FName AccessedNoneTestEventName(TEXT("RaiseAccessedNone"));
	const FName AccessedNoneTestVariableName(TEXT("EmptyTarget"));

	/**
	 * A Blueprint whose one event destroys the actor an empty variable points at: a read through an empty reference,
	 * which is what the Blueprint VM reports as Accessed None. Built from nodes, as a studio's graph is, so the fault
	 * comes from the VM itself rather than from a call standing in for it.
	 */
	UBlueprint* BuildAccessedNoneBlueprint(FAutomationTestBase& Test)
	{
		UPackage* Outer = CreatePackage(*FString::Printf(TEXT("/Temp/FlockBlueprintExceptionTest_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(UObject::StaticClass(), Outer,
			TEXT("BP_FlockAccessedNoneProbe"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (!Test.TestNotNull(TEXT("Blueprint created"), Blueprint)
			|| !Test.TestTrue(TEXT("with an event graph"), Blueprint->UbergraphPages.Num() > 0))
		{
			return nullptr;
		}

		FEdGraphPinType ActorReference;
		ActorReference.PinCategory = UEdGraphSchema_K2::PC_Object;
		ActorReference.PinSubCategoryObject = AActor::StaticClass();
		if (!Test.TestTrue(TEXT("an actor variable added"),
			FBlueprintEditorUtils::AddMemberVariable(Blueprint, AccessedNoneTestVariableName, ActorReference)))
		{
			return nullptr;
		}

		UEdGraph* EventGraph = Blueprint->UbergraphPages[0];

		FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*EventGraph);
		UK2Node_CustomEvent* Event = EventCreator.CreateNode();
		Event->CustomFunctionName = AccessedNoneTestEventName;
		EventCreator.Finalize();

		FGraphNodeCreator<UK2Node_VariableGet> GetCreator(*EventGraph);
		UK2Node_VariableGet* Get = GetCreator.CreateNode();
		Get->VariableReference.SetSelfMember(AccessedNoneTestVariableName);
		GetCreator.Finalize();

		FGraphNodeCreator<UK2Node_CallFunction> CallCreator(*EventGraph);
		UK2Node_CallFunction* Destroy = CallCreator.CreateNode();
		Destroy->SetFromFunction(AActor::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(AActor, K2_DestroyActor)));
		CallCreator.Finalize();

		const UEdGraphSchema* Schema = EventGraph->GetSchema();
		UEdGraphPin* EventThen = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* DestroyExec = Destroy->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		UEdGraphPin* DestroyTarget = Destroy->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
		UEdGraphPin* VariableValue = Get->FindPin(AccessedNoneTestVariableName, EGPD_Output);
		if (!Test.TestTrue(TEXT("every pin found"), EventThen && DestroyExec && DestroyTarget && VariableValue)
			|| !Test.TestTrue(TEXT("the event drives the call"), Schema->TryCreateConnection(EventThen, DestroyExec))
			|| !Test.TestTrue(TEXT("on the empty variable"), Schema->TryCreateConnection(VariableValue, DestroyTarget)))
		{
			return nullptr;
		}

		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		if (!Test.TestTrue(TEXT("the Blueprint compiles"), Blueprint->Status != BS_Error && Blueprint->GeneratedClass != nullptr))
		{
			return nullptr;
		}
		return Blueprint;
	}

	/** One script exception, as the engine hands it to anything listening. */
	struct FBroadcastScriptException
	{
		EBlueprintExceptionType::Type Type = EBlueprintExceptionType::Breakpoint;
		FString Description;
		FString StackTrace;
		bool bOnGameThread = false;
	};
}

/**
 * The Flock SDK reports a Blueprint fault from what the engine broadcasts on FBlueprintCoreDelegates::OnScriptException:
 * the exception's type, its description and the frame's script call stack. Its own tests hand those in directly, so
 * this pins the other half against the real Blueprint VM: a real Accessed None arrives as an access violation, on the
 * game thread, described by the variable that was empty, with the Blueprint in its stack -- and a hundred of
 * them arrive as a hundred identical faults, which is what lets them be counted into one repeat report.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockBlueprintAccessedNoneTest, "Flock.Editor.BlueprintExceptions.AccessedNoneIsBroadcast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockBlueprintAccessedNoneTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = BuildAccessedNoneBlueprint(*this);
	if (Blueprint == nullptr)
	{
		return false;
	}
	UObject* Instance = NewObject<UObject>(GetTransientPackage(), Blueprint->GeneratedClass);
	UFunction* Event = Instance != nullptr ? Instance->FindFunction(AccessedNoneTestEventName) : nullptr;
	if (!TestNotNull(TEXT("the event is callable"), Event))
	{
		return false;
	}

	TArray<FBroadcastScriptException> Broadcasts;
	const FDelegateHandle Listening = FBlueprintCoreDelegates::OnScriptException.AddLambda(
		[&Broadcasts](const UObject*, const FFrame& Stack, const FBlueprintExceptionInfo& Info)
		{
			FBroadcastScriptException Broadcast;
			Broadcast.Type = Info.GetType();
			Broadcast.Description = Info.GetDescription().ToString();
			Broadcast.StackTrace = Stack.GetStackTrace();
			Broadcast.bOnGameThread = IsInGameThread();
			Broadcasts.Add(Broadcast);
		});
	ON_SCOPE_EXIT
	{
		FBlueprintCoreDelegates::OnScriptException.Remove(Listening);
	};

	// In the editor the VM also sends its debugger's tracepoints down the same delegate -- eight for this one event --
	// which is why the SDK reports only the fault kinds and skips breakpoints and tracepoints.
	const auto IsDebuggerSignal = [](EBlueprintExceptionType::Type Type)
	{
		return Type == EBlueprintExceptionType::Breakpoint || Type == EBlueprintExceptionType::Tracepoint
			|| Type == EBlueprintExceptionType::WireTracepoint;
	};
	const auto Faults = [&Broadcasts, &IsDebuggerSignal]()
	{
		return Broadcasts.FilterByPredicate([&IsDebuggerSignal](const FBroadcastScriptException& Broadcast)
		{
			return !IsDebuggerSignal(Broadcast.Type);
		});
	};

	Instance->ProcessEvent(Event, nullptr);

	const TArray<FBroadcastScriptException> FirstRun = Faults();
	if (!TestEqual(TEXT("one Accessed None, one fault broadcast"), FirstRun.Num(), 1))
	{
		return false;
	}
	const FBroadcastScriptException& First = FirstRun[0];
	TestEqual(TEXT("as an access violation"), static_cast<int32>(First.Type), static_cast<int32>(EBlueprintExceptionType::AccessViolation));
	TestTrue(TEXT("described as Accessed None"), First.Description.Contains(TEXT("Accessed None"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("naming the empty variable"), First.Description.Contains(AccessedNoneTestVariableName.ToString(), ESearchCase::CaseSensitive));
	TestTrue(TEXT("on the game thread"), First.bOnGameThread);
	TestTrue(TEXT("with the Blueprint in its stack"), First.StackTrace.Contains(TEXT("BP_FlockAccessedNoneProbe"), ESearchCase::CaseSensitive));

	for (int32 Index = 1; Index < 100; ++Index)
	{
		Instance->ProcessEvent(Event, nullptr);
	}
	const TArray<FBroadcastScriptException> AllRuns = Faults();
	TestEqual(TEXT("a hundred runs, a hundred fault broadcasts"), AllRuns.Num(), 100);
	int32 Identical = 0;
	for (const FBroadcastScriptException& Broadcast : AllRuns)
	{
		if (Broadcast.Type == First.Type && Broadcast.Description.Equals(First.Description, ESearchCase::CaseSensitive)
			&& Broadcast.StackTrace.Equals(First.StackTrace, ESearchCase::CaseSensitive))
		{
			++Identical;
		}
	}
	TestEqual(TEXT("every one the same fault"), Identical, 100);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
