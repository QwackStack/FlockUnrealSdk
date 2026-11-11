// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/MemberReference.h"
#include "Models/FlockShopModels.h"
#include "Store/FlockStore.h"

namespace FlockShopSampleTest
{
	const TCHAR* const ScreenPath = TEXT("/Flock/Samples/Shop/WBP_FlockShopSample.WBP_FlockShopSample");
	const TCHAR* const RowPath = TEXT("/Flock/Samples/Shop/WBP_FlockShopSampleItem.WBP_FlockShopSampleItem");

	bool HoldsStruct(FAutomationTestBase& Test, UClass* Class, const TCHAR* Name, const UScriptStruct* Struct)
	{
		const FStructProperty* Property = FindFProperty<FStructProperty>(Class, Name);
		return Test.TestTrue(*FString::Printf(TEXT("it holds %s (%s)"), Name, *Struct->GetName()), Property != nullptr && Property->Struct == Struct);
	}
}

/**
 * The plugin's Blueprint-only shop sample (Content/Samples/Shop) loads in this engine: its screen has every event its buttons and
 * Flock's store events call and what it holds (the items, the wallet, what the player owns, the store), its row has Show and the
 * clicks and holds its item and its screen, and its level loads. Saved with the oldest engine the SDK supports; every newer one
 * runs this.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopSampleLoadsTest, "Flock.Samples.Shop.TheSampleLoads_WithEveryEventItsButtonsCall_AndWhatItHolds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FFlockShopSampleLoadsTest::RunTest(const FString& Parameters)
{
	using namespace FlockShopSampleTest;
	UClass* Screen = LoadObject<UClass>(nullptr, TEXT("/Flock/Samples/Shop/WBP_FlockShopSample.WBP_FlockShopSample_C"));
	UClass* Row = LoadObject<UClass>(nullptr, TEXT("/Flock/Samples/Shop/WBP_FlockShopSampleItem.WBP_FlockShopSampleItem_C"));
	// The SDK does not depend on UMG, so the widget class is found by its path.
	const UClass* UserWidget = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
	if (!TestTrue(TEXT("the screen and its row load, as widgets"), Screen != nullptr && Row != nullptr && UserWidget != nullptr
		&& Screen->IsChildOf(UserWidget) && Row->IsChildOf(UserWidget)))
	{
		return false;
	}
	for (const TCHAR* Event : { TEXT("Say"), TEXT("Buy"), TEXT("Use"), TEXT("Refresh"), TEXT("ShowItems"), TEXT("SignInClicked"), TEXT("OpenClicked"),
		TEXT("RestoreClicked"), TEXT("CheckSetupClicked"), TEXT("StorePurchaseCompleted"), TEXT("StorePurchaseNotDelivered") })
	{
		TestNotNull(*FString::Printf(TEXT("the screen's event %s"), Event), Screen->FindFunctionByName(Event));
	}
	const FArrayProperty* Items = FindFProperty<FArrayProperty>(Screen, TEXT("Items"));
	const FStructProperty* Item = Items != nullptr ? CastField<FStructProperty>(Items->Inner) : nullptr;
	TestTrue(TEXT("it holds the items it read"), Item != nullptr && Item->Struct == FFlockShopItem::StaticStruct());
	HoldsStruct(*this, Screen, TEXT("Wallet"), FFlockWallet::StaticStruct());
	HoldsStruct(*this, Screen, TEXT("Owned"), FFlockOwnedItemCounts::StaticStruct());
	const FObjectProperty* Store = FindFProperty<FObjectProperty>(Screen, TEXT("Store"));
	TestTrue(TEXT("and the store it connected"), Store != nullptr && Store->PropertyClass == UFlockStore::StaticClass());
	TestNotNull(TEXT("and the lines it said"), FindFProperty<FStrProperty>(Screen, TEXT("Lines")));

	for (const TCHAR* Event : { TEXT("Show"), TEXT("BuyClicked"), TEXT("UseClicked") })
	{
		TestNotNull(*FString::Printf(TEXT("the row's event %s"), Event), Row->FindFunctionByName(Event));
	}
	HoldsStruct(*this, Row, TEXT("Item"), FFlockShopItem::StaticStruct());
	const FObjectProperty* RowScreen = FindFProperty<FObjectProperty>(Row, TEXT("Screen"));
	TestTrue(TEXT("the row holds its screen"), RowScreen != nullptr && RowScreen->PropertyClass == Screen);

	TestNotNull(TEXT("its level loads"), LoadObject<UWorld>(nullptr, TEXT("/Flock/Samples/Shop/FlockShopSample.FlockShopSample")));
	return true;
}

#if WITH_EDITORONLY_DATA
/**
 * The sample's screen connects the store with Flock Connect Store's Options pin left unwired (a game's own way to pay) and with it
 * wired (a payment provider typed), and the Blueprint compiles clean as saved: an optional struct pin left unwired is a graph a
 * game will make, so the sample is the one that proves it. The graphs exist in the editor only.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopSampleConnectStoreTest, "Flock.Samples.Shop.ConnectStore_OptionsLeftUnwired_CompilesClean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFlockShopSampleConnectStoreTest::RunTest(const FString& Parameters)
{
	using namespace FlockShopSampleTest;
	UBlueprint* Screen = LoadObject<UBlueprint>(nullptr, ScreenPath);
	UBlueprint* Row = LoadObject<UBlueprint>(nullptr, RowPath);
	if (!TestTrue(TEXT("the screen's and the row's Blueprints load"), Screen != nullptr && Row != nullptr))
	{
		return false;
	}
	TestTrue(TEXT("the screen compiles clean"), Screen->Status == BS_UpToDate);
	TestTrue(TEXT("the row compiles clean"), Row->Status == BS_UpToDate);
	int32 Unwired = 0;
	int32 Wired = 0;
	for (const UEdGraph* Graph : Screen->UbergraphPages)
	{
		for (const UEdGraphNode* Node : Graph != nullptr ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			for (const UEdGraphPin* Pin : Node != nullptr ? Node->Pins : TArray<UEdGraphPin*>())
			{
				if (Pin->Direction == EGPD_Input && Pin->PinName == TEXT("Options") && Pin->PinType.PinSubCategoryObject == FFlockStoreOptions::StaticStruct())
				{
					++(Pin->LinkedTo.IsEmpty() ? Unwired : Wired);
				}
			}
		}
	}
	TestEqual(TEXT("one Connect Store with its Options pin unwired"), Unwired, 1);
	TestEqual(TEXT("one Connect Store with its Options pin wired (a payment provider typed)"), Wired, 1);
	return true;
}

namespace FlockShopSampleTest
{
	/** The function a call node calls, read by reflection (the SDK does not depend on the editor's graph module). */
	FName CalledFunction(const UEdGraphNode* Node)
	{
		const FStructProperty* Reference = Node != nullptr ? FindFProperty<FStructProperty>(Node->GetClass(), TEXT("FunctionReference")) : nullptr;
		return Reference != nullptr && Reference->Struct == FMemberReference::StaticStruct()
			? Reference->ContainerPtrToValuePtr<FMemberReference>(Node)->GetMemberName() : NAME_None;
	}

	/** The node an execution output named Then leads to, or null. */
	const UEdGraphNode* Next(const UEdGraphNode* Node)
	{
		const UEdGraphPin* Then = Node != nullptr ? Node->FindPin(TEXT("then"), EGPD_Output) : nullptr;
		return Then != nullptr && Then->LinkedTo.Num() == 1 ? Then->LinkedTo[0]->GetOwningNode() : nullptr;
	}
}

/**
 * Flock's server can answer a purchase, a use or a restore before it has saved it, so a read of what the player owns made at once can
 * miss it. The sample's Refresh After Change reads now, waits a second, and reads again; Buy, Use, Restore and the store's purchase
 * event each call it, so the row reads Owned once the server has saved the purchase.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopSampleReadsAgainTest, "Flock.Samples.Shop.AfterAPurchaseUseOrRestore_ReadsAgainASecondLater",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFlockShopSampleReadsAgainTest::RunTest(const FString& Parameters)
{
	using namespace FlockShopSampleTest;
	UBlueprint* Screen = LoadObject<UBlueprint>(nullptr, ScreenPath);
	if (!TestNotNull(TEXT("the screen's Blueprint loads"), Screen))
	{
		return false;
	}
	const UEdGraphNode* Event = nullptr;
	int32 Callers = 0;
	for (const UEdGraph* Graph : Screen->UbergraphPages)
	{
		for (const UEdGraphNode* Node : Graph != nullptr ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			const FNameProperty* Custom = Node != nullptr ? FindFProperty<FNameProperty>(Node->GetClass(), TEXT("CustomFunctionName")) : nullptr;
			if (Custom != nullptr && Custom->GetPropertyValue_InContainer(Node) == TEXT("RefreshAfterChange"))
			{
				Event = Node;
			}
			Callers += CalledFunction(Node) == TEXT("RefreshAfterChange") ? 1 : 0;
		}
	}
	if (!TestNotNull(TEXT("the screen has Refresh After Change"), Event))
	{
		return false;
	}
	const UEdGraphNode* Now = Next(Event);
	const UEdGraphNode* Wait = Next(Now);
	const UEdGraphNode* Again = Next(Wait);
	TestEqual(TEXT("it reads at once"), CalledFunction(Now), FName(TEXT("Refresh")));
	TestEqual(TEXT("then waits"), CalledFunction(Wait), FName(TEXT("Delay")));
	const UEdGraphPin* Duration = Wait != nullptr ? Wait->FindPin(TEXT("Duration"), EGPD_Input) : nullptr;
	TestTrue(TEXT("a second"), Duration != nullptr && FCString::Atof(*Duration->DefaultValue) >= 1.0f);
	TestEqual(TEXT("then reads again"), CalledFunction(Again), FName(TEXT("Refresh")));
	TestEqual(TEXT("Buy, Buy's restore after no answer, Use, Restore and the store's purchase event call it"), Callers, 5);
	return true;
}

namespace FlockShopSampleTest
{
	/** The factory an async node calls (Flock Purchase is Purchase), read by reflection, or None. */
	FName AsyncFactory(const UEdGraphNode* Node)
	{
		const FNameProperty* Factory = Node != nullptr ? FindFProperty<FNameProperty>(Node->GetClass(), TEXT("ProxyFactoryFunctionName")) : nullptr;
		return Factory != nullptr ? Factory->GetPropertyValue_InContainer(Node) : NAME_None;
	}
}

/**
 * Flock's answer to a purchase never came (Outcome Unknown): a game-currency item may have been bought, and the wallet and
 * inventory read again show it, but a real-money one may have been paid for (a phone's store took the payment; a web checkout's
 * page may still be paid), so the screen remembers it as waiting for its delivery and runs Flock Restore Purchases. Every node
 * On Failure leads to is followed: a Branch on Outcome Unknown, a Branch on the item's Is Real Money, the item added to Delivery
 * Unknown and Restore Purchases are among them; and the row reads Delivery Unknown, so Buy stays off while it waits.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopSampleUnknownRealMoneyTest, "Flock.Samples.Shop.NoAnswerToARealMoneyPurchase_RestoresBeforeOfferingItAgain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFlockShopSampleUnknownRealMoneyTest::RunTest(const FString& Parameters)
{
	using namespace FlockShopSampleTest;
	UBlueprint* Screen = LoadObject<UBlueprint>(nullptr, ScreenPath);
	if (!TestNotNull(TEXT("the screen's Blueprint loads"), Screen))
	{
		return false;
	}
	const UEdGraphNode* Purchase = nullptr;
	for (const UEdGraph* Graph : Screen->UbergraphPages)
	{
		for (const UEdGraphNode* Node : Graph != nullptr ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			if (AsyncFactory(Node) == TEXT("Purchase"))
			{
				Purchase = Node;
			}
		}
	}
	const UEdGraphPin* Failed = Purchase != nullptr ? Purchase->FindPin(TEXT("OnFailure"), EGPD_Output) : nullptr;
	if (!TestNotNull(TEXT("the screen buys with Flock Purchase, and handles On Failure"), Failed))
	{
		return false;
	}
	TArray<const UEdGraphNode*> Reached;
	TArray<const UEdGraphPin*> ToFollow = { Failed };
	while (!ToFollow.IsEmpty())
	{
		const UEdGraphPin* Out = ToFollow.Pop();
		for (const UEdGraphPin* In : Out->LinkedTo)
		{
			const UEdGraphNode* Node = In->GetOwningNode();
			if (Node == nullptr || Reached.Contains(Node))
			{
				continue;
			}
			Reached.Add(Node);
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == TEXT("exec"))
				{
					ToFollow.Add(Pin);
				}
			}
		}
	}
	const auto BranchesOn = [&Reached](const TCHAR* PinName)
	{
		return Reached.ContainsByPredicate([PinName](const UEdGraphNode* Node)
		{
			const UEdGraphPin* Condition = Node->FindPin(TEXT("Condition"), EGPD_Input);
			return Condition != nullptr && Condition->LinkedTo.ContainsByPredicate([PinName](const UEdGraphPin* From)
			{
				return From->PinName.ToString().Equals(PinName, ESearchCase::CaseSensitive);
			});
		});
	};
	TestTrue(TEXT("a Branch on Outcome Unknown"), BranchesOn(TEXT("bOutcomeUnknown")));
	TestTrue(TEXT("a Branch on the item's Is Real Money"), BranchesOn(TEXT("IsRealMoney")));
	TestTrue(TEXT("and Flock Restore Purchases"), Reached.ContainsByPredicate([](const UEdGraphNode* Node) { return AsyncFactory(Node) == TEXT("RestorePurchases"); }));
	TestTrue(TEXT("the item is remembered as waiting for its delivery"), Reached.ContainsByPredicate([](const UEdGraphNode* Node)
	{
		const UEdGraphPin* Into = Node->FindPin(TEXT("TargetArray"), EGPD_Input);
		return CalledFunction(Node) == TEXT("Array_AddUnique") && Into != nullptr && Into->LinkedTo.ContainsByPredicate([](const UEdGraphPin* From)
		{
			return From->PinName == TEXT("DeliveryUnknown");
		});
	}));

	UBlueprint* Row = LoadObject<UBlueprint>(nullptr, RowPath);
	bool bRowReadsIt = false;
	for (const UEdGraph* Graph : Row != nullptr ? Row->UbergraphPages : TArray<TObjectPtr<UEdGraph>>())
	{
		for (const UEdGraphNode* Node : Graph != nullptr ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			const UEdGraphPin* Into = Node != nullptr ? Node->FindPin(TEXT("TargetArray"), EGPD_Input) : nullptr;
			bRowReadsIt |= CalledFunction(Node) == TEXT("Array_Contains") && Into != nullptr && Into->LinkedTo.ContainsByPredicate([](const UEdGraphPin* From)
			{
				return From->PinName == TEXT("DeliveryUnknown");
			});
		}
	}
	TestTrue(TEXT("and the row looks its item up in Delivery Unknown"), bRowReadsIt);
	return true;
}

/**
 * Log In signs in once: a second sign-in would end the first, and a purchase under way with it. Its click branches on Flock Is
 * Authenticated first, and only its False leads on to Flock Login With Device.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockShopSampleSignInOnceTest, "Flock.Samples.Shop.LogIn_WhenSignedInAlready_DoesNotSignInAgain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFlockShopSampleSignInOnceTest::RunTest(const FString& Parameters)
{
	using namespace FlockShopSampleTest;
	UBlueprint* Screen = LoadObject<UBlueprint>(nullptr, ScreenPath);
	if (!TestNotNull(TEXT("the screen's Blueprint loads"), Screen))
	{
		return false;
	}
	const UEdGraphNode* Clicked = nullptr;
	for (const UEdGraph* Graph : Screen->UbergraphPages)
	{
		for (const UEdGraphNode* Node : Graph != nullptr ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			const FNameProperty* Custom = Node != nullptr ? FindFProperty<FNameProperty>(Node->GetClass(), TEXT("CustomFunctionName")) : nullptr;
			if (Custom != nullptr && Custom->GetPropertyValue_InContainer(Node) == TEXT("SignInClicked"))
			{
				Clicked = Node;
			}
		}
	}
	const UEdGraphNode* First = Next(Clicked);
	const UEdGraphPin* Condition = First != nullptr ? First->FindPin(TEXT("Condition"), EGPD_Input) : nullptr;
	if (!TestTrue(TEXT("Log In's click goes first to a Branch"), Condition != nullptr && Condition->LinkedTo.Num() == 1))
	{
		return false;
	}
	TestEqual(TEXT("on Flock Is Authenticated"), CalledFunction(Condition->LinkedTo[0]->GetOwningNode()), FName(TEXT("IsAuthenticated")));
	const UEdGraphPin* NotSignedIn = First->FindPin(TEXT("else"), EGPD_Output);
	const UEdGraphPin* SignedIn = First->FindPin(TEXT("then"), EGPD_Output);
	const auto LeadsToLogin = [](const UEdGraphPin* Pin)
	{
		return Pin != nullptr && Pin->LinkedTo.ContainsByPredicate([](const UEdGraphPin* To) { return AsyncFactory(To->GetOwningNode()) == TEXT("LoginWithDevice"); });
	};
	TestTrue(TEXT("its False logs the device in"), LeadsToLogin(NotSignedIn));
	TestFalse(TEXT("its True does not"), LeadsToLogin(SignedIn));
	return true;
}
#endif

#endif
