// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockAnalyticsConfig.h"
#include "Analytics/FlockConsentStore.h"
#include "Config/FlockConfig.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockTemporaryFiles.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/Support/FlockTemporaryFilesTestSupport.h"

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

/** A new decision is written beside the old one and moved over it, so a kill part-way through never leaves a torn file. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConsentStoreSavesThroughATemporaryFileTest, "Flock.Analytics.Consent.SavesThroughATemporaryFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockConsentStoreSavesThroughATemporaryFileTest::RunTest(const FString& Parameters)
{
	const FString Path = MakeTempConsentPath(TEXT("temporary"));
	FFlockConsentStore Store(Path);
	Store.Save(true);

	bool bWrittenAside = false;
	bool bPreviousDecisionReadsBack = false;
	bool bPreviousGranted = false;
	ON_SCOPE_EXIT { FFlockTemporaryFiles::SetBeforeNextMoveForTesting(nullptr); };
	FFlockTemporaryFiles::SetBeforeNextMoveForTesting([&bWrittenAside, &bPreviousDecisionReadsBack, &bPreviousGranted, &Path](const FString&)
	{
		// The game is killed at this moment: what does the next launch read?
		bWrittenAside = true;
		const FFlockConsentStore NextLaunch(Path);
		bPreviousDecisionReadsBack = NextLaunch.Load(bPreviousGranted);
	});
	Store.Save(false);

	TestTrue(TEXT("The new decision is written to a temporary file of its own first"), bWrittenAside);
	TestTrue(TEXT("Until it is moved into place, the previous decision still reads back whole"), bPreviousDecisionReadsBack && bPreviousGranted);
	bool bGranted = true;
	TestTrue(TEXT("The new decision lands"), FFlockConsentStore(Path).Load(bGranted) && !bGranted);
	TestEqual(TEXT("No temporary file is left"), FFlockTemporaryFiles::FindTemporaryFilesOf(Path).Num(), 0);
	DeleteTempFile(Path);
	return true;
}

/** A decision a kill cut off part-way through its save is not lost, so a player who opted out stays opted out. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConsentStoreKeepsDecisionCutOffMidSaveTest, "Flock.Analytics.Consent.KeepsADecisionCutOffMidSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockConsentStoreKeepsDecisionCutOffMidSaveTest::RunTest(const FString& Parameters)
{
	// What a kill in the middle of a save straight over the file leaves: an empty file. It reads as no decision, so a project
	// that does not require explicit consent collects again from a player who had opted out.
	{
		const FString Path = MakeTempConsentPath(TEXT("emptied"));
		FFileHelper::SaveStringToFile(FString(), *Path);
		const FFlockConsentStore Store(Path);
		TestFalse(TEXT("An empty consent file reads as no decision"), Store.HasDecision());
		TestTrue(TEXT("Which collects when consent is not required"), Store.ResolveEffective(/*bRequireExplicitConsent*/ false));
		DeleteTempFile(Path);
	}

	// What a kill between the engine deleting the old file and moving the new one in leaves: no file, and the new decision
	// whole in its temporary file. The next launch may come days later, so the temporary file is old.
	{
		const FString Path = MakeTempConsentPath(TEXT("cutoff"));
		const FString TemporaryFile = FFlockTemporaryFiles::MakePath(Path);
		TestTrue(TEXT("Precondition: the new decision is whole in its temporary file"),
			FFileHelper::SaveStringToFile(TEXT("{\"granted\":false,\"decided_at\":\"2026-09-16T00:00:00Z\"}"), *TemporaryFile));
		FlockMoveTestFileTimeBack(TemporaryFile);
		const FFlockConsentStore Store(Path);
		bool bGranted = true;
		TestTrue(TEXT("The decision is read back from its temporary file"), Store.Load(bGranted));
		TestFalse(TEXT("As the player left it: opted out"), bGranted);
		TestFalse(TEXT("So nothing is collected"), Store.ResolveEffective(/*bRequireExplicitConsent*/ false));
		TestTrue(TEXT("And it is moved into place"), IFileManager::Get().FileExists(*Path));
		TestEqual(TEXT("Leaving no temporary file"), FFlockTemporaryFiles::FindTemporaryFilesOf(Path).Num(), 0);
		DeleteTempFile(Path);
	}

	// Control: a temporary file cut off while it was written holds no decision.
	{
		const FString Path = MakeTempConsentPath(TEXT("halfwritten"));
		const FString TemporaryFile = FFlockTemporaryFiles::MakePath(Path);
		FFileHelper::SaveStringToFile(TEXT("{\"granted\":"), *TemporaryFile);
		const FFlockConsentStore Store(Path);
		TestFalse(TEXT("Control: a half-written temporary file is not a decision"), Store.HasDecision());
		DeleteTempFile(TemporaryFile);
		DeleteTempFile(Path);
	}

	// Control: the newest temporary file was cut off while it was written; the whole one before it is the decision.
	{
		const FString Path = MakeTempConsentPath(TEXT("newesthalf"));
		const FString WholeFile = FFlockTemporaryFiles::MakePath(Path);
		const FString HalfFile = FFlockTemporaryFiles::MakePath(Path);
		FFileHelper::SaveStringToFile(TEXT("{\"granted\":true}"), *WholeFile);
		FFileHelper::SaveStringToFile(TEXT("{\"granted\":"), *HalfFile);
		FlockMoveTestFileTimeBack(WholeFile, 5.0);
		FlockMoveTestFileTimeBack(HalfFile, 2.0);
		const FFlockConsentStore Store(Path);
		bool bGranted = false;
		TestTrue(TEXT("Control: a half-written newest file is passed over for the whole one before it"), Store.Load(bGranted) && bGranted);
		DeleteTempFile(HalfFile);
		DeleteTempFile(Path);
	}

	// A decision that cannot be moved into place stays in its temporary file, so a later launch still finds it.
	{
		const FString Path = MakeTempConsentPath(TEXT("cannotmove"));
		const FString TemporaryFile = FFlockTemporaryFiles::MakePath(Path);
		FFileHelper::SaveStringToFile(TEXT("{\"granted\":false}"), *TemporaryFile);
		FlockMoveTestFileTimeBack(TemporaryFile);
		// A folder stands where the file goes, so nothing can be moved over it.
		IFileManager::Get().MakeDirectory(*Path, /*Tree*/ true);

		const FFlockConsentStore Store(Path);
		bool bGranted = true;
		TestTrue(TEXT("The decision is still read"), Store.Load(bGranted) && !bGranted);
		TestTrue(TEXT("And kept where it is rather than swept, as it is the only copy"), IFileManager::Get().FileExists(*TemporaryFile));
		IFileManager::Get().Delete(*TemporaryFile);
		IFileManager::Get().DeleteDirectory(*Path, /*RequireExists*/ false, /*Tree*/ true);
	}

	// Control: while the file itself is there it is the decision; a temporary file beside it is a save still in progress.
	{
		const FString Path = MakeTempConsentPath(TEXT("present"));
		const FString TemporaryFile = FFlockTemporaryFiles::MakePath(Path);
		FFileHelper::SaveStringToFile(TEXT("{\"granted\":true}"), *Path);
		FFileHelper::SaveStringToFile(TEXT("{\"granted\":false}"), *TemporaryFile);
		const FFlockConsentStore Store(Path);
		bool bGranted = false;
		TestTrue(TEXT("Control: the file beats a temporary file beside it"), Store.Load(bGranted) && bGranted);
		DeleteTempFile(TemporaryFile);
		DeleteTempFile(Path);
	}
	return true;
}

/** Temporary files a crash left beside the consent file are deleted once they are old; a fresh one may be another game's save. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConsentStoreDeletesOldLeftOverFilesTest, "Flock.Analytics.Consent.DeletesOnlyOldLeftOverFiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockConsentStoreDeletesOldLeftOverFilesTest::RunTest(const FString& Parameters)
{
	const FString Path = MakeTempConsentPath(TEXT("leftovers"));
	FFileHelper::SaveStringToFile(TEXT("{\"granted\":true}"), *Path);
	const FString OldFile = FFlockTemporaryFiles::MakePath(Path);
	const FString FreshFile = FFlockTemporaryFiles::MakePath(Path);
	FFileHelper::SaveStringToFile(TEXT("{\"granted\":"), *OldFile);
	FFileHelper::SaveStringToFile(TEXT("{\"granted\":"), *FreshFile);
	FlockMoveTestFileTimeBack(OldFile);

	const FFlockConsentStore Store(Path);
	TestFalse(TEXT("A temporary file a crash left long ago is deleted"), IFileManager::Get().FileExists(*OldFile));
	TestTrue(TEXT("A fresh one may be another game's save in progress, and is kept"), IFileManager::Get().FileExists(*FreshFile));
	DeleteTempFile(FreshFile);
	DeleteTempFile(Path);
	return true;
}

/** Erasing the decision erases a save from moments ago too, so nothing brings it back on the next launch. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConsentStoreClearForgetsASaveInProgressTest, "Flock.Analytics.Consent.ClearForgetsASaveMomentsOld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockConsentStoreClearForgetsASaveInProgressTest::RunTest(const FString& Parameters)
{
	const FString Path = MakeTempConsentPath(TEXT("cleared"));
	FFlockConsentStore Store(Path);
	Store.Save(true);
	// A save from moments ago, cut off before its move: fresh, so no sweep would touch it.
	const FString TemporaryFile = FFlockTemporaryFiles::MakePath(Path);
	TestTrue(TEXT("Precondition: a fresh temporary file sits beside the decision"),
		FFileHelper::SaveStringToFile(TEXT("{\"granted\":true}"), *TemporaryFile));

	Store.Clear();
	TestFalse(TEXT("The decision is forgotten"), Store.HasDecision());
	TestFalse(TEXT("Its file is gone"), IFileManager::Get().FileExists(*Path));
	TestEqual(TEXT("And so is the save from moments ago"), FFlockTemporaryFiles::FindTemporaryFilesOf(Path).Num(), 0);
	TestFalse(TEXT("So the next launch finds no decision"), FFlockConsentStore(Path).HasDecision());
	DeleteTempFile(Path);
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
		TestTrue(TEXT("no session platform, so the engine's name is sent"), Config.SessionPlatform.IsEmpty());
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
		Settings->AnalyticsSessionPlatform = TEXT("steam");

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
		TestEqual(TEXT("session platform"), Config.SessionPlatform, FString(TEXT("steam")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsConfigSessionPlatformTest, "Flock.Analytics.Config.SessionPlatform",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsConfigSessionPlatformTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	TestFalse(TEXT("Empty is not a usable platform"), Config.HasUsableSessionPlatform());
	TestEqual(TEXT("Empty sends the engine's platform name"), Config.GetSessionPlatform(TEXT("Windows")), FString(TEXT("Windows")));

	Config.SessionPlatform = TEXT("steam");
	TestEqual(TEXT("A set value is sent"), Config.GetSessionPlatform(TEXT("Windows")), FString(TEXT("steam")));

	Config.SessionPlatform = TEXT("Steam Deck");
	TestEqual(TEXT("A space inside the name is kept, letter case included"), Config.GetSessionPlatform(TEXT("Windows")),
		FString(TEXT("Steam Deck")));

	// Refused, never trimmed: a trimmed value would hide the mistake, and a verbatim one files sessions under a
	// platform nobody meant.
	for (const TCHAR* Unusable : { TEXT(" steam"), TEXT("steam "), TEXT("steam\n"), TEXT("\tsteam") })
	{
		Config.SessionPlatform = Unusable;
		TestFalse(FString::Printf(TEXT("'%s' is not usable"), Unusable), Config.HasUsableSessionPlatform());
		TestEqual(FString::Printf(TEXT("'%s' sends the engine's platform name"), Unusable),
			Config.GetSessionPlatform(TEXT("Windows")), FString(TEXT("Windows")));
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
