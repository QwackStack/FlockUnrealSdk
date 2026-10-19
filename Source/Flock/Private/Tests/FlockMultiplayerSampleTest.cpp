// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Models/FlockMultiplayerModels.h"
#include "Multiplayer/FlockGameModes.h"
#include "Multiplayer/FlockMultiplayerSession.h"

/**
 * The plugin's Blueprint-only multiplayer sample (Content/Samples/Multiplayer) loads in this engine, its Game Instance has every
 * event its buttons call and what it holds, and its level runs Flock's game mode, so a host built from it lets in only the
 * session's players. Saved with the oldest engine the SDK supports; every newer one runs this.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockMultiplayerSampleTest, "Flock.Samples.Multiplayer.TheSampleLoads_WithEveryEventItsButtonsCall_AndFlocksGameMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FFlockMultiplayerSampleTest::RunTest(const FString& Parameters)
{
	UClass* GameInstance = LoadObject<UClass>(nullptr, TEXT("/Flock/Samples/Multiplayer/BP_FlockSampleGameInstance.BP_FlockSampleGameInstance_C"));
	if (!TestNotNull(TEXT("the sample's Game Instance loads"), GameInstance))
	{
		return false;
	}
	TestTrue(TEXT("a Game Instance"), GameInstance->IsChildOf(UGameInstance::StaticClass()));
	for (const TCHAR* Event : { TEXT("SignIn"), TEXT("Host"), TEXT("Join"), TEXT("QuickMatch"), TEXT("Leave"), TEXT("Play"), TEXT("TravelAgain"),
		TEXT("HostChanged"), TEXT("RelayLost"), TEXT("SessionEnded") })
	{
		TestNotNull(*FString::Printf(TEXT("its event %s"), Event), GameInstance->FindFunctionByName(Event));
	}
	const FObjectProperty* Session = FindFProperty<FObjectProperty>(GameInstance, TEXT("Session"));
	TestTrue(TEXT("it holds the session"), Session != nullptr && Session->PropertyClass == UFlockMultiplayerSession::StaticClass());
	TestNotNull(TEXT("and a status line"), FindFProperty<FStrProperty>(GameInstance, TEXT("Status")));
	const FStructProperty* Options = FindFProperty<FStructProperty>(GameInstance, TEXT("TravelOptions"));
	TestTrue(TEXT("and Travel To Session's options"), Options != nullptr && Options->Struct == FFlockTravelToSessionOptions::StaticStruct());

	UClass* Widget = LoadObject<UClass>(nullptr, TEXT("/Flock/Samples/Multiplayer/WBP_FlockMultiplayerSample.WBP_FlockMultiplayerSample_C"));
	// The SDK does not depend on UMG, so the widget class is found by its path.
	const UClass* UserWidget = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
	TestTrue(TEXT("its widget loads"), Widget != nullptr && UserWidget != nullptr && Widget->IsChildOf(UserWidget));

	UWorld* Map = LoadObject<UWorld>(nullptr, TEXT("/Flock/Samples/Multiplayer/FlockMultiplayerSample.FlockMultiplayerSample"));
	if (!TestNotNull(TEXT("its level loads"), Map))
	{
		return false;
	}
	const AWorldSettings* Settings = Map->PersistentLevel != nullptr ? Map->PersistentLevel->GetWorldSettings(false) : nullptr;
	TestTrue(TEXT("its level runs Flock's game mode"), Settings != nullptr && Settings->DefaultGameMode == AFlockGameModeBase::StaticClass());
	return true;
}

#endif
