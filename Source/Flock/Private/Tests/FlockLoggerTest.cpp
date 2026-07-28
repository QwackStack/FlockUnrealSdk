// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockSubsystem.h"
#include "FlockInitConfig.h"
#include "FlockLogger.h"
#include "Engine/GameInstance.h"
#include "UObject/Package.h"

namespace
{
	/** Captures messages instead of routing to UE_LOG — stands in for a game's own logger/debugger. */
	class FCapturingLogger : public IFlockLogger
	{
	public:
		TArray<FString> Debugs;
		TArray<FString> Infos;
		TArray<FString> Warnings;
		TArray<FString> Errors;

		virtual void LogDebug(const FString& Message) override { Debugs.Add(Message); }
		virtual void LogInfo(const FString& Message) override { Infos.Add(Message); }
		virtual void LogWarning(const FString& Message) override { Warnings.Add(Message); }
		virtual void LogError(const FString& Message) override { Errors.Add(Message); }
	};

	FFlockInitConfig MakeValidConfig()
	{
		FFlockInitConfig Config;
		Config.ApiUrl = TEXT("https://api-flock.qwacks.com");
		Config.ApiKey = TEXT("secret");
		Config.GameId = TEXT("my-game");
		Config.GameVersion = TEXT("1.2.3");
		Config.GameVersionId = TEXT("ver-abc");
		return Config;
	}

	// UFlockSubsystem is a UGameInstanceSubsystem (ClassWithin=UGameInstance), so its Outer must be a
	// UGameInstance. Creating it under the transient package trips a "created in invalid Outer" ensure.
	UFlockSubsystem* NewTransientSubsystem()
	{
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		return NewObject<UFlockSubsystem>(GameInstance);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLoggerRoutingTest, "Flock.Runtime.Logger.Routing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLoggerRoutingTest::RunTest(const FString& Parameters)
{
	// An injected logger captures, so no error reaches the automation framework (no AddExpectedError).
	UFlockSubsystem* Sdk = NewTransientSubsystem();
	const TSharedRef<FCapturingLogger> Capture = MakeShared<FCapturingLogger>();
	Sdk->SetLogger(Capture);

	Sdk->InitializeWithConfig(MakeValidConfig());
	TestTrue(TEXT("Init breadcrumb routed to the injected logger"), Capture->Infos.Num() > 0);

	// Double-init routes a warning.
	Sdk->InitializeWithConfig(MakeValidConfig());
	TestTrue(TEXT("Double-init warning routed to the injected logger"), Capture->Warnings.Num() > 0);

	// Gate failure routes an error (no UE_LOG, so the test framework doesn't flag it).
	Sdk->ShutdownSdk();
	FFlockInitConfig Unresolved = MakeValidConfig();
	Unresolved.GameVersionId = TEXT("");
	Sdk->InitializeWithConfig(Unresolved);
	TestTrue(TEXT("Gate-failure error routed to the injected logger"), Capture->Errors.Num() > 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNullLoggerTest, "Flock.Runtime.Logger.Null",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockNullLoggerTest::RunTest(const FString& Parameters)
{
	// The null logger swallows everything without touching UE_LOG — safe to drive a failure through it.
	UFlockSubsystem* Sdk = NewTransientSubsystem();
	Sdk->SetLogger(MakeShared<FFlockNullLogger>());

	FFlockInitConfig Unresolved = MakeValidConfig();
	Unresolved.GameVersionId = TEXT("");
	Sdk->InitializeWithConfig(Unresolved);

	TestFalse(TEXT("Init still fails cleanly with a silent logger"), Sdk->IsInitialized());
	TestFalse(TEXT("Error is still recorded programmatically"), Sdk->GetInitializationError().IsEmpty());

	return true;
}

#endif // WITH_AUTOMATION_TESTS
