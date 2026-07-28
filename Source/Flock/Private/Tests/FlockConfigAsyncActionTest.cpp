// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Blueprint/FlockConfigAsyncActions.h"
#include "Tests/Support/FlockConfigNodeTestListener.h"

/**
 * A graph can reach these nodes before the SDK is up. Each must fire exactly one pin — the failure pin,
 * carrying a Validation error — never zero (the graph would hang) and never a crash. A null world context
 * resolves to no subsystem, which is the uninitialized case. The success path is covered by the provider
 * tests, which drive the same provider methods directly.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigNodeUninitializedTest, "Flock.Config.Node.Uninitialized",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigNodeUninitializedTest::RunTest(const FString& Parameters)
{
	UFlockConfigNodeTestListener* Listener = NewObject<UFlockConfigNodeTestListener>();
	UObject* BadContext = nullptr;

	auto ExpectValidation = [this, Listener](const TCHAR* What)
	{
		TestEqual(FString::Printf(TEXT("%s: validation error"), What),
			static_cast<int32>(Listener->LastError.Type), static_cast<int32>(EFlockErrorType::Validation));
	};

	{
		UFlockGetConfigAction* Action = UFlockGetConfigAction::GetConfigByName(BadContext, TEXT("Balance"));
		Action->OnSuccess.AddDynamic(Listener, &UFlockConfigNodeTestListener::HandleConfigPin);
		Action->OnFailure.AddDynamic(Listener, &UFlockConfigNodeTestListener::HandleConfigPin);
		Action->Activate();
		TestEqual(TEXT("get config by name fired one pin"), Listener->ConfigPinCount, 1);
		ExpectValidation(TEXT("config by name"));
	}
	{
		UFlockGetConfigAction* Action = UFlockGetConfigAction::GetConfigById(BadContext, TEXT("cfg-1"));
		Action->OnSuccess.AddDynamic(Listener, &UFlockConfigNodeTestListener::HandleConfigPin);
		Action->OnFailure.AddDynamic(Listener, &UFlockConfigNodeTestListener::HandleConfigPin);
		Action->Activate();
		TestEqual(TEXT("get config by id fired one more pin"), Listener->ConfigPinCount, 2);
	}
	{
		UFlockGetConfigsByTagAction* Action = UFlockGetConfigsByTagAction::GetConfigsByTag(BadContext, EFlockConfigTag::Gameplay);
		Action->OnSuccess.AddDynamic(Listener, &UFlockConfigNodeTestListener::HandleConfigListPin);
		Action->OnFailure.AddDynamic(Listener, &UFlockConfigNodeTestListener::HandleConfigListPin);
		Action->Activate();
		TestEqual(TEXT("get configs by tag fired one pin"), Listener->ConfigListPinCount, 1);
		ExpectValidation(TEXT("configs by tag"));
	}
	{
		UFlockResolveConfigDataAction* Action = UFlockResolveConfigDataAction::ResolveConfigData(BadContext, TEXT("cfg-1"));
		Action->OnSuccess.AddDynamic(Listener, &UFlockConfigNodeTestListener::HandleConfigDataPin);
		Action->OnFailure.AddDynamic(Listener, &UFlockConfigNodeTestListener::HandleConfigDataPin);
		Action->Activate();
		TestEqual(TEXT("resolve config data fired one pin"), Listener->ConfigDataPinCount, 1);
		ExpectValidation(TEXT("resolve config data"));
	}
	{
		UFlockGetGameAction* Action = UFlockGetGameAction::GetGame(BadContext);
		Action->OnSuccess.AddDynamic(Listener, &UFlockConfigNodeTestListener::HandleGamePin);
		Action->OnFailure.AddDynamic(Listener, &UFlockConfigNodeTestListener::HandleGamePin);
		Action->Activate();
		TestEqual(TEXT("get game fired one pin"), Listener->GamePinCount, 1);
		ExpectValidation(TEXT("get game"));
	}
	{
		UFlockGetGameVersionAction* Action = UFlockGetGameVersionAction::GetGameVersion(BadContext);
		Action->OnSuccess.AddDynamic(Listener, &UFlockConfigNodeTestListener::HandleGameVersionPin);
		Action->OnFailure.AddDynamic(Listener, &UFlockConfigNodeTestListener::HandleGameVersionPin);
		Action->Activate();
		TestEqual(TEXT("get game version fired one pin"), Listener->GameVersionPinCount, 1);
		ExpectValidation(TEXT("get game version"));
	}
	{
		UFlockGetGameVersionAction* Action = UFlockGetGameVersionAction::GetGameVersionByName(BadContext, TEXT("release"));
		Action->OnSuccess.AddDynamic(Listener, &UFlockConfigNodeTestListener::HandleGameVersionPin);
		Action->OnFailure.AddDynamic(Listener, &UFlockConfigNodeTestListener::HandleGameVersionPin);
		Action->Activate();
		TestEqual(TEXT("get game version by name fired one more pin"), Listener->GameVersionPinCount, 2);
	}

	return true;
}

#endif // WITH_AUTOMATION_TESTS
