// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockAnalyticsConfig.h"
#include "Analytics/FlockConsentStore.h"
#include "Config/FlockConfig.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	/** A throwaway path per case so the tests never collide with a real consent file. */
	FString MakeTempConsentPath(const TCHAR* Tag)
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("consent_%s_%s.json"), Tag, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	void DeleteTempFile(const FString& Path)
	{
		FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*Path);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConsentStorePersistenceTest, "Flock.Analytics.Consent.Persistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConsentStorePersistenceTest::RunTest(const FString& Parameters)
{
	const FString Path = MakeTempConsentPath(TEXT("persist"));

	// A store with no file behind it has no decision.
	{
		FFlockConsentStore Store(Path);
		TestFalse(TEXT("no decision initially"), Store.HasDecision());
		bool Granted = true;
		TestFalse(TEXT("load reports nothing stored"), Store.Load(Granted));
		TestTrue(TEXT("out param untouched"), Granted);
	}

	// Granting persists across instances — this is the across-runs behaviour.
	{
		FFlockConsentStore Store(Path);
		Store.Save(true);
		TestTrue(TEXT("has decision after save"), Store.HasDecision());
	}
	{
		FFlockConsentStore Reloaded(Path);
		TestTrue(TEXT("decision survived"), Reloaded.HasDecision());
		bool Granted = false;
		TestTrue(TEXT("loads"), Reloaded.Load(Granted));
		TestTrue(TEXT("granted survived"), Granted);
	}

	// Revoking persists too — a false decision is a decision, not an absence of one.
	{
		FFlockConsentStore Store(Path);
		Store.Save(false);
	}
	{
		FFlockConsentStore Reloaded(Path);
		TestTrue(TEXT("revoke is a recorded decision"), Reloaded.HasDecision());
		bool Granted = true;
		TestTrue(TEXT("loads"), Reloaded.Load(Granted));
		TestFalse(TEXT("revoked survived"), Granted);
	}

	// Clear forgets the decision and removes the file.
	{
		FFlockConsentStore Store(Path);
		Store.Clear();
		TestFalse(TEXT("cleared in memory"), Store.HasDecision());
		TestFalse(TEXT("file removed"), FPlatformFileManager::Get().GetPlatformFile().FileExists(*Path));
	}
	{
		FFlockConsentStore Reloaded(Path);
		TestFalse(TEXT("cleared across instances"), Reloaded.HasDecision());
	}

	DeleteTempFile(Path);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConsentStoreCorruptTest, "Flock.Analytics.Consent.Corrupt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConsentStoreCorruptTest::RunTest(const FString& Parameters)
{
	// Garbage on disk reads as "no decision" rather than throwing or asserting.
	{
		const FString Path = MakeTempConsentPath(TEXT("garbage"));
		FFileHelper::SaveStringToFile(FString(TEXT("}}not json{{")), *Path);
		FFlockConsentStore Store(Path);
		TestFalse(TEXT("corrupt file yields no decision"), Store.HasDecision());

		// And the store stays usable afterwards.
		Store.Save(true);
		TestTrue(TEXT("recovers on save"), Store.HasDecision());
		DeleteTempFile(Path);
	}

	// Well-formed JSON missing the flag is also "no decision".
	{
		const FString Path = MakeTempConsentPath(TEXT("nofield"));
		FFileHelper::SaveStringToFile(FString(TEXT("{\"decided_at\":\"2026-07-21T00:00:00Z\"}")), *Path);
		FFlockConsentStore Store(Path);
		TestFalse(TEXT("missing granted field yields no decision"), Store.HasDecision());
		DeleteTempFile(Path);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConsentStoreEffectiveTest, "Flock.Analytics.Consent.Effective",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConsentStoreEffectiveTest::RunTest(const FString& Parameters)
{
	const FString Path = MakeTempConsentPath(TEXT("effective"));

	// No decision recorded: implied granted by default, hard-gated when opt-in is required.
	{
		FFlockConsentStore Store(Path);
		TestTrue(TEXT("implied granted when consent not required"), Store.ResolveEffective(false));
		TestFalse(TEXT("gated when explicit consent required"), Store.ResolveEffective(true));
	}

	// An explicit grant satisfies the gate.
	{
		FFlockConsentStore Store(Path);
		Store.Save(true);
		TestTrue(TEXT("granted passes the gate"), Store.ResolveEffective(true));
		TestTrue(TEXT("granted also collects when not required"), Store.ResolveEffective(false));
	}

	// An explicit revoke wins even on a project that does not require consent.
	{
		FFlockConsentStore Store(Path);
		Store.Save(false);
		TestFalse(TEXT("revoke wins when consent not required"), Store.ResolveEffective(false));
		TestFalse(TEXT("revoke wins when consent required"), Store.ResolveEffective(true));
	}

	DeleteTempFile(Path);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsConfigFromSettingsTest, "Flock.Analytics.Config.FromSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsConfigFromSettingsTest::RunTest(const FString& Parameters)
{
	// Defaults line up with the shipping defaults on UFlockConfig.
	{
		const UFlockConfig* Defaults = GetDefault<UFlockConfig>();
		const FFlockAnalyticsConfig Config = FFlockAnalyticsConfig::FromSettings(*Defaults);
		TestTrue(TEXT("enabled"), Config.bEnabled);
		TestFalse(TEXT("consent not required by default"), Config.bRequireExplicitConsent);
		TestEqual(TEXT("session timeout"), Config.SessionTimeoutSeconds, 30.f);
		TestEqual(TEXT("heartbeat"), Config.HeartbeatIntervalSeconds, 60.f);
		TestEqual(TEXT("bounce threshold"), Config.BounceThresholdSeconds, 10.f);
		TestEqual(TEXT("max cached"), Config.MaxCachedEvents, 1000);
		TestEqual(TEXT("batch size"), Config.CacheFlushBatchSize, 50);
		TestEqual(TEXT("buffer flush"), Config.EventBufferFlushIntervalSeconds, 10.f);
	}

	// Every knob is carried across, so a settings change can't silently stop reaching the core.
	{
		UFlockConfig* Settings = NewObject<UFlockConfig>();
		Settings->bAnalyticsEnabled = false;
		Settings->bAnalyticsRequireExplicitConsent = true;
		Settings->bAnalyticsAutoStartSession = false;
		Settings->bAnalyticsAutoEndOnQuit = false;
		Settings->AnalyticsSessionTimeout = 11.f;
		Settings->AnalyticsHeartbeatInterval = 12.f;
		Settings->AnalyticsBounceThreshold = 13.f;
		Settings->bAnalyticsPersistSession = false;
		Settings->bAnalyticsTrackFps = false;
		Settings->AnalyticsFpsSampleInterval = 14.f;
		Settings->bAnalyticsCacheFailedEvents = false;
		Settings->AnalyticsMaxCachedEvents = 15;
		Settings->AnalyticsCacheFlushBatchSize = 16;
		Settings->AnalyticsEventBufferFlushInterval = 17.f;

		const FFlockAnalyticsConfig Config = FFlockAnalyticsConfig::FromSettings(*Settings);
		TestFalse(TEXT("enabled"), Config.bEnabled);
		TestTrue(TEXT("require consent"), Config.bRequireExplicitConsent);
		TestFalse(TEXT("auto start"), Config.bAutoStartSession);
		TestFalse(TEXT("auto end"), Config.bAutoEndSessionOnQuit);
		TestEqual(TEXT("session timeout"), Config.SessionTimeoutSeconds, 11.f);
		TestEqual(TEXT("heartbeat"), Config.HeartbeatIntervalSeconds, 12.f);
		TestEqual(TEXT("bounce"), Config.BounceThresholdSeconds, 13.f);
		TestFalse(TEXT("persist session"), Config.bPersistSessionOnDisk);
		TestFalse(TEXT("track fps"), Config.bTrackFps);
		TestEqual(TEXT("fps interval"), Config.FpsSampleIntervalSeconds, 14.f);
		TestFalse(TEXT("cache failed"), Config.bCacheFailedEvents);
		TestEqual(TEXT("max cached"), Config.MaxCachedEvents, 15);
		TestEqual(TEXT("batch size"), Config.CacheFlushBatchSize, 16);
		TestEqual(TEXT("buffer flush"), Config.EventBufferFlushIntervalSeconds, 17.f);
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
