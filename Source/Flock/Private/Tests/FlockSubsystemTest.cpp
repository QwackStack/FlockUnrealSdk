// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockSubsystem.h"
#include "FlockInitConfig.h"
#include "UObject/Package.h"

namespace
{
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSubsystemInitGateTest, "Flock.Runtime.Subsystem.InitGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSubsystemInitGateTest::RunTest(const FString& Parameters)
{
	// The clean-failure path logs an Error on purpose; tell the framework to expect it.
	AddExpectedError(TEXT("Initialize failed"), EAutomationExpectedErrorFlags::Contains, 1);

	UFlockSubsystem* Sdk = NewObject<UFlockSubsystem>(GetTransientPackage());

	// Missing baked version ID -> clean failure, stays uninitialized.
	FFlockInitConfig NoVersion = MakeValidConfig();
	NoVersion.GameVersionId = TEXT("");
	Sdk->InitializeWithConfig(NoVersion);
	TestFalse(TEXT("Not initialized without a baked version ID"), Sdk->IsInitialized());
	TestFalse(TEXT("Error is recorded on failed init"), Sdk->GetInitializationError().IsEmpty());

	// Valid config -> initialized, error cleared, getters populated.
	Sdk->InitializeWithConfig(MakeValidConfig());
	TestTrue(TEXT("Initialized with a baked version ID"), Sdk->IsInitialized());
	TestTrue(TEXT("Error is cleared on success"), Sdk->GetInitializationError().IsEmpty());
	TestEqual(TEXT("GameVersionId is exposed"), Sdk->GetGameVersionId(), FString(TEXT("ver-abc")));
	TestEqual(TEXT("Versioned URL appends /v1"), Sdk->GetVersionedApiUrl(),
		FString(TEXT("https://api-flock.qwacks.com/v1")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSubsystemReinitTest, "Flock.Runtime.Subsystem.ReinitAndShutdown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSubsystemReinitTest::RunTest(const FString& Parameters)
{
	UFlockSubsystem* Sdk = NewObject<UFlockSubsystem>(GetTransientPackage());
	Sdk->InitializeWithConfig(MakeValidConfig());
	TestTrue(TEXT("Initialized"), Sdk->IsInitialized());

	// Double-init is a no-op guard; the first config is kept.
	FFlockInitConfig Other = MakeValidConfig();
	Other.GameVersionId = TEXT("ver-xyz");
	Sdk->InitializeWithConfig(Other);
	TestEqual(TEXT("Double-init is ignored (keeps first config)"), Sdk->GetGameVersionId(),
		FString(TEXT("ver-abc")));

	// Shutdown allows re-init with the new config.
	Sdk->ShutdownSdk();
	TestFalse(TEXT("Not initialized after shutdown"), Sdk->IsInitialized());
	Sdk->InitializeWithConfig(Other);
	TestTrue(TEXT("Re-initialized after shutdown"), Sdk->IsInitialized());
	TestEqual(TEXT("New config adopted after shutdown"), Sdk->GetGameVersionId(),
		FString(TEXT("ver-xyz")));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
