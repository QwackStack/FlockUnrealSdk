// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockSubsystem.h"
#include "FlockEvents.h"
#include "FlockInitConfig.h"
#include "Engine/GameInstance.h"
#include "Misc/Base64.h"
#include "Tests/Support/FlockEventTestListener.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
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

	// UFlockSubsystem is a UGameInstanceSubsystem (ClassWithin=UGameInstance), so its Outer must be a
	// UGameInstance. Creating it under the transient package trips a "created in invalid Outer" ensure.
	UFlockSubsystem* NewTransientSubsystem()
	{
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		return NewObject<UFlockSubsystem>(GameInstance);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSubsystemInitGateTest, "Flock.Runtime.Subsystem.InitGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSubsystemInitGateTest::RunTest(const FString& Parameters)
{
	// The clean-failure path logs an Error on purpose; tell the framework to expect it.
	AddExpectedError(TEXT("Initialize failed"), EAutomationExpectedErrorFlags::Contains, 1);

	UFlockSubsystem* Sdk = NewTransientSubsystem();

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
	UFlockSubsystem* Sdk = NewTransientSubsystem();
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

namespace FlockSubsystemAuthTestHelpers
{
	inline FString Base64Url(const FString& In)
	{
		FString Encoded = FBase64::Encode(In);
		Encoded.ReplaceInline(TEXT("+"), TEXT("-"));
		Encoded.ReplaceInline(TEXT("/"), TEXT("_"));
		Encoded.ReplaceInline(TEXT("="), TEXT(""));
		return Encoded;
	}

	inline FString MakeJwt(const FString& PlayerId, int64 ExpiryOffsetSeconds = 3600)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + ExpiryOffsetSeconds;
		const FString Payload = FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp);
		return FString::Printf(TEXT("h.%s.s"), *Base64Url(Payload));
	}

	struct FSubsystemAuthFixture
	{
		UGameInstance* GameInstance = nullptr;
		UFlockSubsystem* Sdk = nullptr;
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		UFlockEventTestListener* Listener = nullptr;

		FSubsystemAuthFixture()
		{
			GameInstance = NewObject<UGameInstance>(GetTransientPackage());
			Sdk = NewObject<UFlockSubsystem>(GameInstance);
			Sdk->SetHttpAdapterForTesting(Fake);
			Sdk->SetTokenStoreForTesting(Store);
			Listener = NewObject<UFlockEventTestListener>();
			Sdk->GetEvents()->OnAuthenticated.AddDynamic(Listener, &UFlockEventTestListener::HandleAuthenticated);
			Sdk->GetEvents()->OnSessionRestored.AddDynamic(Listener, &UFlockEventTestListener::HandleSessionRestored);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSubsystemAuthWiringTest, "Flock.Runtime.Subsystem.AuthWiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSubsystemAuthWiringTest::RunTest(const FString& Parameters)
{
	using namespace FlockSubsystemAuthTestHelpers;

	// Before init: no provider, safe no-op Logout, signed-out getters.
	{
		FSubsystemAuthFixture F;
		TestNull(TEXT("no provider before init"), F.Sdk->GetAuthProvider());
		TestFalse(TEXT("not authenticated"), F.Sdk->IsAuthenticated());
		TestTrue(TEXT("player id empty"), F.Sdk->GetPlayerId().IsEmpty());
		F.Sdk->Logout(); // must not crash
	}
	// Init wires the auth stack; shutdown drops it.
	{
		FSubsystemAuthFixture F;
		F.Sdk->InitializeWithConfig(MakeValidConfig());
		TestTrue(TEXT("initialized"), F.Sdk->IsInitialized());
		TestNotNull(TEXT("provider wired"), F.Sdk->GetAuthProvider());
		TestFalse(TEXT("no session -> not authenticated"), F.Sdk->IsAuthenticated());
		TestEqual(TEXT("restore attempted (event fired false)"), F.Listener->SessionRestoredCount, 1);
		TestFalse(TEXT("nothing restored"), F.Listener->bLastSessionRestored);

		F.Sdk->ShutdownSdk();
		TestNull(TEXT("provider dropped"), F.Sdk->GetAuthProvider());
		TestFalse(TEXT("signed-out getters safe"), F.Sdk->IsAuthenticated());
	}
	// A persisted session is auto-restored on init and surfaces through the subsystem getters.
	{
		FSubsystemAuthFixture F;
		F.Store->bHasTokens = true;
		F.Store->Stored.AccessToken = MakeJwt(TEXT("p-42"));
		F.Store->Stored.RefreshToken = TEXT("r-42");
		F.Store->Stored.AuthMethod = EFlockAuthMethod::Device;

		F.Sdk->InitializeWithConfig(MakeValidConfig());

		TestTrue(TEXT("restored"), F.Sdk->IsAuthenticated());
		TestEqual(TEXT("player id"), F.Sdk->GetPlayerId(), FString(TEXT("p-42")));
		TestFalse(TEXT("restore finished"), F.Sdk->IsRestoringSession());
		TestEqual(TEXT("authenticated event"), F.Listener->AuthenticatedCount, 1);
		TestEqual(TEXT("via session-restore"), static_cast<int32>(F.Listener->LastAuthInfo.Method),
			static_cast<int32>(EFlockAuthMethod::SessionRestore));

		// Logout through the subsystem clears the restored session.
		F.Sdk->Logout();
		TestFalse(TEXT("logged out"), F.Sdk->IsAuthenticated());
		TestFalse(TEXT("store cleared"), F.Store->bHasTokens);

		F.Sdk->ShutdownSdk();
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
