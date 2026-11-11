// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Flock.h"
#include "FlockSubsystem.h"
#include "FlockInitConfig.h"
#include "FlockLogger.h"
#include "Providers/FlockGameProvider.h"
#include "Config/FlockConfig.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"
#include "Tests/Support/FlockTestSdk.h"
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

	/** Catches what the default logger sends to the engine's log under LogFlock. */
	class FFlockLineCatcher : public FOutputDevice
	{
	public:
		FFlockLineCatcher() { GLog->AddOutputDevice(this); }
		virtual ~FFlockLineCatcher() override { GLog->RemoveOutputDevice(this); }
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category == LogFlock.GetCategoryName())
			{
				FScopeLock Lock(&Guard);
				Lines.Add(Text);
			}
		}
		bool Caught(const TCHAR* Part)
		{
			GLog->Flush();
			FScopeLock Lock(&Guard);
			return Lines.ContainsByPredicate([Part](const FString& Line) { return Line.Contains(Part); });
		}

	private:
		FCriticalSection Guard;
		TArray<FString> Lines;
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLoggerRoutingTest, "Flock.Runtime.Logger.Routing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLoggerRoutingTest::RunTest(const FString& Parameters)
{
	// An injected logger captures, so no error reaches the automation framework (no AddExpectedError).
	const FFlockTestSdk Test;
	UFlockSubsystem* Sdk = Test.Sdk;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockDefaultLoggerFollowsTheConfigTest, "Flock.Runtime.Logger.DefaultLoggerFollowsTheConfigsDebugLogsEvenAfterALineBeforeInitializing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockDefaultLoggerFollowsTheConfigTest::RunTest(const FString& Parameters)
{
	// Something logs before the game initializes (Auto-Initialize On Load being off says so), which makes the default logger
	// from the settings' Enable Debug Logs; the config the game then initializes with decides, as its flag promises.
	UFlockConfig* Settings = GetMutableDefault<UFlockConfig>();
	const bool bSettingWas = Settings->bEnableDebugLogs;
	const ELogVerbosity::Type CategoryWas = LogFlock.GetVerbosity();
	AddExpectedMessagePlain(TEXT("Logout called before the SDK was initialized"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	for (const bool bOnInTheConfig : { true, false })
	{
		Settings->bEnableDebugLogs = !bOnInTheConfig;
		const FFlockTestSdk Test;
		FFlockLineCatcher Catcher;
		Test.Sdk->Logout();
		FFlockInitConfig Config = MakeValidConfig();
		Config.bEnableDebugLogs = bOnInTheConfig;
		Test.Sdk->InitializeWithConfig(Config);
		TestEqual(FString::Printf(TEXT("the breadcrumbs follow the config's Enable Debug Logs (%s there, %s in the settings)"),
			bOnInTheConfig ? TEXT("on") : TEXT("off"), bOnInTheConfig ? TEXT("off") : TEXT("on")),
			Catcher.Caught(TEXT("Initializing Flock SDK.")), bOnInTheConfig);
	}
	Settings->bEnableDebugLogs = bSettingWas;
	LogFlock.SetVerbosity(CategoryWas);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNullLoggerTest, "Flock.Runtime.Logger.Null",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockNullLoggerTest::RunTest(const FString& Parameters)
{
	// The null logger swallows everything without touching UE_LOG — safe to drive a failure through it.
	const FFlockTestSdk Test;
	UFlockSubsystem* Sdk = Test.Sdk;
	Sdk->SetLogger(MakeShared<FFlockNullLogger>());

	FFlockInitConfig Unresolved = MakeValidConfig();
	Unresolved.GameVersionId = TEXT("");
	Sdk->InitializeWithConfig(Unresolved);

	TestFalse(TEXT("Init still fails cleanly with a silent logger"), Sdk->IsInitialized());
	TestFalse(TEXT("Error is still recorded programmatically"), Sdk->GetInitializationError().IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLoggerSetAfterInitializingTest, "Flock.Runtime.Logger.ALoggerSetAfterInitializingReachesEveryPart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLoggerSetAfterInitializingTest::RunTest(const FString& Parameters)
{
	// The parts are made at initialization and keep the logger they were handed; a logger the game sets afterwards must still
	// get their lines, and the subsystem's own.
	const FFlockTestSdk Test;
	UFlockSubsystem* Sdk = Test.Sdk;
	Sdk->SetLogger(MakeShared<FFlockNullLogger>());
	Sdk->InitializeWithConfig(MakeValidConfig());
	if (!TestTrue(TEXT("the SDK initialized"), Sdk->IsInitialized()))
	{
		return false;
	}
	const TSharedRef<FCapturingLogger> Capture = MakeShared<FCapturingLogger>();
	Sdk->SetLogger(Capture);

	const TArray<TPair<const TCHAR*, FFlockProviderBase*>> Parts = {
		{ TEXT("auth"), Sdk->GetAuthProvider() }, { TEXT("analytics"), Sdk->GetAnalyticsProvider() },
		{ TEXT("config"), Sdk->GetConfigProvider() }, { TEXT("game"), Sdk->GetGameProvider() },
		{ TEXT("shop"), Sdk->GetShopProvider() }, { TEXT("assets"), Sdk->GetAssetProvider() },
		{ TEXT("leaderboards"), Sdk->GetLeaderboardProvider() }, { TEXT("notifications"), Sdk->GetNotificationProvider() },
		{ TEXT("multiplayer"), Sdk->GetMultiplayerProvider() }, { TEXT("players"), Sdk->GetPlayerProvider() },
		{ TEXT("commands"), Sdk->GetCommandProvider() } };
	for (const TPair<const TCHAR*, FFlockProviderBase*>& Part : Parts)
	{
		if (TestNotNull(*FString::Printf(TEXT("the %s part exists"), Part.Key), Part.Value))
		{
			Part.Value->GetLogger().LogInfo(FString::Printf(TEXT("a line from the %s part"), Part.Key));
			TestTrue(*FString::Printf(TEXT("the line from the %s part reaches the logger set after initializing"), Part.Key),
				Capture->Infos.Contains(FString::Printf(TEXT("a line from the %s part"), Part.Key)));
		}
	}

	// A call's network trace, from the HTTP client made at initialization (the test's transport never answers).
	Sdk->GetGameProvider()->GetGame([](TFlockResult<FFlockGameSchema>) {});
	TestTrue(TEXT("the call's network trace reaches it"),
		Capture->Debugs.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("-> GET ")); }));

	// The subsystem's own line, the events hub's, and one after the SDK shut down and started again.
	Sdk->InitializeWithConfig(MakeValidConfig());
	TestTrue(TEXT("the subsystem's warning reaches it"), Capture->Warnings.Num() > 0);
	Sdk->ShutdownSdk();
	TestTrue(TEXT("the events hub's line reaches it"),
		Capture->Debugs.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT(" fired -> ")); }));
	const int32 InfosBefore = Capture->Infos.Num();
	Sdk->InitializeWithConfig(MakeValidConfig());
	TestTrue(TEXT("and it stays after shutting down and initializing again"), Capture->Infos.Num() > InfosBefore);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
