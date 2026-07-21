// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Blueprint/FlockAnalyticsAsyncActions.h"
#include "Engine/GameInstance.h"
#include "FlockSubsystem.h"
#include "Tests/Support/FlockEventTestListener.h"

/**
 * A Blueprint graph can reach these nodes before the SDK is up, or with analytics switched off in
 * settings. Exactly one pin must fire, carrying a Validation error — never zero pins (the graph
 * would hang) and never a crash.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsNodeUninitializedTest, "Flock.Analytics.Node.Uninitialized",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsNodeUninitializedTest::RunTest(const FString& Parameters)
{
	UFlockAuthNodeTestListener* Listener = NewObject<UFlockAuthNodeTestListener>();
	// No world context resolves to no subsystem, which is the uninitialized case.
	UObject* BadContext = nullptr;

	{
		UFlockFlushAnalyticsAction* Action = UFlockFlushAnalyticsAction::FlushAnalytics(BadContext);
		Action->OnFailure.AddDynamic(Listener, &UFlockAuthNodeTestListener::HandleFlushPin);
		Action->OnSuccess.AddDynamic(Listener, &UFlockAuthNodeTestListener::HandleFlushPin);
		Action->Activate();
		TestEqual(TEXT("flush fired exactly one pin"), Listener->FlushPinCount, 1);
		TestEqual(TEXT("validation error"), static_cast<int32>(Listener->LastError.Type),
			static_cast<int32>(EFlockErrorType::Validation));
	}

	{
		UFlockAnalyticsSessionAction* Action =
			UFlockAnalyticsSessionAction::StartAnalyticsSession(BadContext, TEXT("p-1"));
		Action->OnFailure.AddDynamic(Listener, &UFlockAuthNodeTestListener::HandleSessionPin);
		Action->OnSuccess.AddDynamic(Listener, &UFlockAuthNodeTestListener::HandleSessionPin);
		Action->Activate();
		TestEqual(TEXT("start session fired exactly one pin"), Listener->SessionPinCount, 1);
		TestEqual(TEXT("validation error"), static_cast<int32>(Listener->LastError.Type),
			static_cast<int32>(EFlockErrorType::Validation));
	}

	{
		UFlockAnalyticsSessionAction* Action = UFlockAnalyticsSessionAction::EndAnalyticsSession(BadContext);
		Action->OnFailure.AddDynamic(Listener, &UFlockAuthNodeTestListener::HandleSessionPin);
		Action->OnSuccess.AddDynamic(Listener, &UFlockAuthNodeTestListener::HandleSessionPin);
		Action->Activate();
		TestEqual(TEXT("end session fired exactly one pin"), Listener->SessionPinCount, 2);
	}
	return true;
}

/**
 * The same reachable-too-early problem on the subsystem passthroughs. These are BlueprintCallable, so
 * a graph can call them before init; every one must be a safe no-op rather than a null dereference.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSubsystemGuardTest, "Flock.Analytics.Subsystem.Guards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSubsystemGuardTest::RunTest(const FString& Parameters)
{
	// UFlockSubsystem is ClassWithin=UGameInstance, so its Outer must be one.
	UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
	UFlockSubsystem* Sdk = NewObject<UFlockSubsystem>(GameInstance);

	TestNull(TEXT("no provider before initialization"), Sdk->GetAnalyticsProvider());

	// None of these may crash.
	Sdk->LogAnalyticsEvent(TEXT("early"), TMap<FString, FString>());
	Sdk->LogAnalyticsError(TEXT("early"), TEXT("x"), TEXT("E1"), TMap<FString, FString>());
	Sdk->LogAnalyticsException(TEXT("early"), TEXT("trace"), TMap<FString, FString>());
	Sdk->RecordScreenView(TEXT("Screen"));
	Sdk->SetAnalyticsConsent(true);
	Sdk->EraseLocalAnalyticsData();

	TestFalse(TEXT("no consent without a provider"), Sdk->HasAnalyticsConsent());
	TestFalse(TEXT("no active session"), Sdk->HasActiveAnalyticsSession());
	TestTrue(TEXT("no session id"), Sdk->GetAnalyticsSessionId().IsEmpty());
	TestEqual(TEXT("empty snapshot"), Sdk->GetAnalyticsSnapshot().ScreensViewed, 0);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
