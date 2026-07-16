// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockSubsystem.h"
#include "FlockEvents.h"
#include "FlockInitConfig.h"
#include "FlockLogger.h"
#include "Engine/GameInstance.h"
#include "UObject/Package.h"
#include "Tests/Support/FlockEventTestListener.h"

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

	// UFlockSubsystem is a UGameInstanceSubsystem (ClassWithin=UGameInstance), so its Outer must be a
	// UGameInstance. The null logger keeps the failure paths quiet for the automation framework.
	UFlockSubsystem* NewQuietSubsystem()
	{
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		UFlockSubsystem* Sdk = NewObject<UFlockSubsystem>(GameInstance);
		Sdk->SetLogger(MakeShared<FFlockNullLogger>());
		return Sdk;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockEventsLifecycleTest, "Flock.Runtime.Events.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockEventsLifecycleTest::RunTest(const FString& Parameters)
{
	UFlockSubsystem* Sdk = NewQuietSubsystem();
	UFlockEventTestListener* Listener = NewObject<UFlockEventTestListener>(GetTransientPackage());
	Sdk->GetEvents()->OnInitialized.AddDynamic(Listener, &UFlockEventTestListener::HandleInitialized);
	Sdk->GetEvents()->OnInitializationFailed.AddDynamic(Listener, &UFlockEventTestListener::HandleInitializationFailed);
	Sdk->GetEvents()->OnShutdown.AddDynamic(Listener, &UFlockEventTestListener::HandleShutdown);

	Sdk->InitializeWithConfig(MakeValidConfig());
	TestEqual(TEXT("initialized raised once"), Listener->InitializedCount, 1);
	TestEqual(TEXT("no failure on success"), Listener->FailedCount, 0);

	// The already-initialized misuse guard must not raise anything.
	Sdk->InitializeWithConfig(MakeValidConfig());
	TestEqual(TEXT("misuse guard raises nothing"), Listener->InitializedCount, 1);
	TestEqual(TEXT("misuse guard raises no failure"), Listener->FailedCount, 0);

	Sdk->ShutdownSdk();
	TestEqual(TEXT("shutdown raised"), Listener->ShutdownCount, 1);

	FFlockInitConfig Unresolved = MakeValidConfig();
	Unresolved.GameVersionId = TEXT("");
	Sdk->InitializeWithConfig(Unresolved);
	TestEqual(TEXT("failure raised"), Listener->FailedCount, 1);
	TestFalse(TEXT("failure carries the message"), Listener->LastError.IsEmpty());

	// Subscriptions survive shutdown: a re-init reaches the same listener again.
	Sdk->InitializeWithConfig(MakeValidConfig());
	TestEqual(TEXT("re-init reaches surviving subscription"), Listener->InitializedCount, 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockEventsCallOrRegisterTest, "Flock.Runtime.Events.CallOrRegister",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockEventsCallOrRegisterTest::RunTest(const FString& Parameters)
{
	UFlockSubsystem* Sdk = NewQuietSubsystem();
	UFlockEventTestListener* Listener = NewObject<UFlockEventTestListener>(GetTransientPackage());

	// Registered before init: parked, fires on the init that follows.
	FFlockInitializedCallback OnReady;
	OnReady.BindDynamic(Listener, &UFlockEventTestListener::HandleInitialized);
	Sdk->GetEvents()->CallOrRegister_OnInitialized(OnReady);
	TestEqual(TEXT("parked before init"), Listener->InitializedCount, 0);

	Sdk->InitializeWithConfig(MakeValidConfig());
	TestEqual(TEXT("fires on init"), Listener->InitializedCount, 1);

	// Registered after init: fires immediately (the auto-init case a plain binding would miss).
	Sdk->GetEvents()->CallOrRegister_OnInitialized(OnReady);
	TestEqual(TEXT("fires immediately when already initialized"), Listener->InitializedCount, 2);

	// One-shot: a shutdown + re-init must not re-fire earlier registrations.
	Sdk->ShutdownSdk();
	Sdk->InitializeWithConfig(MakeValidConfig());
	TestEqual(TEXT("one-shot across re-init"), Listener->InitializedCount, 2);

	// Failure replay: registering after a failure gets the stored error immediately.
	Sdk->ShutdownSdk();
	FFlockInitConfig Unresolved = MakeValidConfig();
	Unresolved.GameVersionId = TEXT("");
	Sdk->InitializeWithConfig(Unresolved);

	FFlockInitializationFailedCallback OnFailed;
	OnFailed.BindDynamic(Listener, &UFlockEventTestListener::HandleInitializationFailed);
	Sdk->GetEvents()->CallOrRegister_OnInitializationFailed(OnFailed);
	TestEqual(TEXT("failure replayed to late registrant"), Listener->FailedCount, 1);
	TestFalse(TEXT("replayed error is non-empty"), Listener->LastError.IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockEventsFeatureRaisesTest, "Flock.Runtime.Events.FeatureRaises",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockEventsFeatureRaisesTest::RunTest(const FString& Parameters)
{
	UFlockSubsystem* Sdk = NewQuietSubsystem();
	UFlockEvents* Events = Sdk->GetEvents();
	UFlockEventTestListener* Listener = NewObject<UFlockEventTestListener>(GetTransientPackage());
	Events->OnAuthenticated.AddDynamic(Listener, &UFlockEventTestListener::HandleAuthenticated);
	Events->OnConsentChanged.AddDynamic(Listener, &UFlockEventTestListener::HandleConsentChanged);
	Events->OnSessionEnded.AddDynamic(Listener, &UFlockEventTestListener::HandleSessionEnded);

	FFlockAuthInfo Info;
	Info.PlayerId = TEXT("player-1");
	Info.Method = EFlockAuthMethod::Device;
	Events->InvokeAuthenticated(Info);
	TestEqual(TEXT("authenticated delivered"), Listener->AuthenticatedCount, 1);
	TestEqual(TEXT("player id carried"), Listener->LastAuthInfo.PlayerId, FString(TEXT("player-1")));
	TestEqual(TEXT("method carried"), static_cast<int32>(Listener->LastAuthInfo.Method),
		static_cast<int32>(EFlockAuthMethod::Device));

	Events->InvokeConsentChanged(true);
	TestEqual(TEXT("consent delivered"), Listener->ConsentCount, 1);
	TestTrue(TEXT("consent state carried"), Listener->bLastConsent);

	FFlockSessionEndedArgs Args;
	Args.Reason = EFlockSessionEndReason::Timeout;
	Args.Snapshot.SessionId = TEXT("session-1");
	Events->InvokeSessionEnded(Args);
	TestEqual(TEXT("session end delivered"), Listener->SessionEndedCount, 1);
	TestEqual(TEXT("reason carried"), static_cast<int32>(Listener->LastSessionEnded.Reason),
		static_cast<int32>(EFlockSessionEndReason::Timeout));
	TestEqual(TEXT("snapshot carried"), Listener->LastSessionEnded.Snapshot.SessionId, FString(TEXT("session-1")));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
