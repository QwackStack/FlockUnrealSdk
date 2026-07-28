// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockInitConfig.h"
#include "Config/FlockConfig.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockInitConfigFromSettingsTest, "Flock.Runtime.InitConfig.FromSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockInitConfigFromSettingsTest::RunTest(const FString& Parameters)
{
	UFlockConfig* Settings = NewObject<UFlockConfig>(GetTransientPackage());
	Settings->ApiUrl = TEXT("https://api-flock.qwacks.com");
	Settings->ApiKey = TEXT("secret");
	Settings->GameId = TEXT("my-game");
	Settings->GameVersion = TEXT("1.2.3");
	Settings->GameVersionId = TEXT("ver-abc");
	Settings->bEnableDebugLogs = true;

	const FFlockInitConfig Config = FFlockInitConfig::FromSettings(*Settings);
	TestEqual(TEXT("ApiUrl copied"), Config.ApiUrl, Settings->ApiUrl);
	TestEqual(TEXT("ApiKey copied"), Config.ApiKey, Settings->ApiKey);
	TestEqual(TEXT("GameId copied"), Config.GameId, Settings->GameId);
	TestEqual(TEXT("GameVersion copied"), Config.GameVersion, Settings->GameVersion);
	TestEqual(TEXT("GameVersionId copied"), Config.GameVersionId, Settings->GameVersionId);
	TestTrue(TEXT("bEnableDebugLogs copied"), Config.bEnableDebugLogs);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
