// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Blueprint/FlockLibrary.h"

// The convenience library resolves the subsystem from a world context. With no resolvable SDK (a null
// context), every pure read must return a default and every fire-and-forget call must be a safe no-op —
// the same "safe before init" contract the subsystem methods carry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLibrarySafeWithoutSdkTest, "Flock.Library.SafeWithoutSdk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLibrarySafeWithoutSdkTest::RunTest(const FString& Parameters)
{
	// Pure reads default when the SDK can't be resolved.
	TestFalse(TEXT("IsAuthenticated default"), UFlockLibrary::IsAuthenticated(nullptr));
	TestEqual(TEXT("GetPlayerId default"), UFlockLibrary::GetPlayerId(nullptr), FString());
	TestFalse(TEXT("IsRestoringSession default"), UFlockLibrary::IsRestoringSession(nullptr));
	TestFalse(TEXT("IsInitialized default"), UFlockLibrary::IsInitialized(nullptr));
	TestEqual(TEXT("GetInitializationError default"), UFlockLibrary::GetInitializationError(nullptr), FString());
	TestEqual(TEXT("GetGameId default"), UFlockLibrary::GetGameId(nullptr), FString());
	TestEqual(TEXT("GetGameVersionId default"), UFlockLibrary::GetGameVersionId(nullptr), FString());
	TestEqual(TEXT("GetApiUrl default"), UFlockLibrary::GetApiUrl(nullptr), FString());
	TestFalse(TEXT("HasAnalyticsConsent default"), UFlockLibrary::HasAnalyticsConsent(nullptr));
	TestFalse(TEXT("HasActiveAnalyticsSession default"), UFlockLibrary::HasActiveAnalyticsSession(nullptr));
	TestEqual(TEXT("GetAnalyticsSessionId default"), UFlockLibrary::GetAnalyticsSessionId(nullptr), FString());
	TestNull(TEXT("GetEvents default"), UFlockLibrary::GetEvents(nullptr));

	// Fire-and-forget calls must not crash without an SDK.
	const TMap<FString, FString> Empty;
	const FFlockLogDetails Details;
	UFlockLibrary::LogEvent(nullptr, TEXT("m"), Empty);
	UFlockLibrary::LogError(nullptr, TEXT("e"), Details);
	UFlockLibrary::LogException(nullptr, TEXT("x"), FString(), Details);
	UFlockLibrary::RecordScreenView(nullptr, TEXT("Menu"));
	UFlockLibrary::SetAnalyticsConsent(nullptr, true);
	UFlockLibrary::EraseLocalAnalyticsData(nullptr);
	UFlockLibrary::Logout(nullptr);
	UFlockLibrary::GetAnalyticsSnapshot(nullptr);
	TestTrue(TEXT("no crash on any call without an SDK"), true);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
