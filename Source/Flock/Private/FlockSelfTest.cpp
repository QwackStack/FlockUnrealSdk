// Copyright 2022, Qwacks. All Rights Reserved.

// A dev-only console command that drives the Flock SDK surface and narrates each step to the log,
// so you can watch boot/init behavior without a backend or a baked Game Version. Run from the editor
// or in-game console: `Flock.SelfTest` (also works via -ExecCmds in a development build).
//
// This is a demonstration harness, not a unit test — it exercises a transient subsystem instance
// through the injectable IFlockLogger so the breadcrumbs are visible end to end.

#if !UE_BUILD_SHIPPING

#include "FlockSubsystem.h"
#include "FlockInitConfig.h"
#include "FlockLogger.h"
#include "HAL/IConsoleManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	void RunFlockSelfTest()
	{
		// Verbose logger so every breadcrumb prints, regardless of project settings.
		const TSharedRef<IFlockLogger> Logger = MakeShared<FFlockUnrealLogger>(/*bVerbose*/ true);
		Logger->LogInfo(TEXT("Self-test: starting."));

		UFlockSubsystem* Sdk = NewObject<UFlockSubsystem>(GetTransientPackage());
		Sdk->SetLogger(Logger);

		FFlockInitConfig Config;
		Config.ApiUrl = TEXT("https://api-flock.qwacks.com");
		Config.ApiKey = TEXT("demo-key");
		Config.GameId = TEXT("demo-game");
		Config.GameVersion = TEXT("1.0.0");
		Config.GameVersionId = TEXT("demo-version-id"); // pretend this was baked at edit time
		Config.bEnableDebugLogs = true;

		Logger->LogInfo(TEXT("Self-test: initializing with a demo config (fake baked version id)."));
		Sdk->InitializeWithConfig(Config);
		Logger->LogInfo(FString::Printf(
			TEXT("Self-test: IsInitialized=%s GameId=%s GameVersionId=%s VersionedUrl=%s"),
			Sdk->IsInitialized() ? TEXT("true") : TEXT("false"),
			*Sdk->GetGameId(), *Sdk->GetGameVersionId(), *Sdk->GetVersionedApiUrl()));

		Logger->LogInfo(TEXT("Self-test: attempting a second init (expect a warning + no-op)."));
		Sdk->InitializeWithConfig(Config);

		Logger->LogInfo(TEXT("Self-test: shutting down."));
		Sdk->ShutdownSdk();

		Logger->LogInfo(TEXT("Self-test: attempting init with an unresolved version id (expect a clean failure)."));
		FFlockInitConfig Unresolved = Config;
		Unresolved.GameVersionId = TEXT("");
		Sdk->InitializeWithConfig(Unresolved);
		Logger->LogInfo(FString::Printf(TEXT("Self-test: after unresolved init, IsInitialized=%s Error=%s"),
			Sdk->IsInitialized() ? TEXT("true") : TEXT("false"), *Sdk->GetInitializationError()));

		Logger->LogInfo(TEXT("Self-test: complete."));
	}

	FAutoConsoleCommand GFlockSelfTestCommand(
		TEXT("Flock.SelfTest"),
		TEXT("Drives the Flock SDK surface and narrates each step to the log (development builds only)."),
		FConsoleCommandDelegate::CreateStatic(&RunFlockSelfTest));
}

#endif // !UE_BUILD_SHIPPING
