// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockTerminationTracker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	FString MakeTempMarkerPath()
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("marker_%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	void DeleteTempFile(const FString& Path)
	{
		IFileManager::Get().Delete(*Path);
	}

	bool MarkerFileExists(const FString& Path)
	{
		return FPlatformFileManager::Get().GetPlatformFile().FileExists(*Path);
	}

	FFlockTerminationMarker MakeSeed()
	{
		FFlockTerminationMarker Seed;
		Seed.SessionId = TEXT("sess-1");
		Seed.PlayerId = TEXT("p-1");
		Seed.AppVersion = TEXT("1.2.3");
		Seed.SdkVersion = TEXT("0.7.0");
		return Seed;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockTerminationClassifyTest, "Flock.Analytics.Termination.Classify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockTerminationClassifyTest::RunTest(const FString& Parameters)
{
	FFlockTerminationMarker Marker = MakeSeed();

	Marker.LastState = FFlockTerminationTracker::StateBackground;
	TestEqual(TEXT("died backgrounded"), FFlockTerminationTracker::Classify(Marker),
		FString(FFlockTerminationTracker::ClassBackgroundKill));

	Marker.LastState = FFlockTerminationTracker::StateForeground;
	TestEqual(TEXT("died foregrounded"), FFlockTerminationTracker::Classify(Marker),
		FString(FFlockTerminationTracker::ClassAbnormal));

	// Anything that is not explicitly "background" is a foreground death.
	Marker.LastState = TEXT("something else");
	TestEqual(TEXT("unknown state is abnormal"), FFlockTerminationTracker::Classify(Marker),
		FString(FFlockTerminationTracker::ClassAbnormal));

	// A marker with nothing to attribute is not a marker.
	FFlockTerminationMarker Empty;
	TestFalse(TEXT("empty marker invalid"), Empty.IsValid());
	TestTrue(TEXT("invalid classifies to nothing"), FFlockTerminationTracker::Classify(Empty).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockTerminationCleanExitTest, "Flock.Analytics.Termination.CleanExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockTerminationCleanExitTest::RunTest(const FString& Parameters)
{
	const FString Path = MakeTempMarkerPath();

	// A run that shuts down through the quit path leaves nothing behind.
	{
		FFlockTerminationTracker Tracker(/*bEnabled*/ true, Path);
		Tracker.BeginTracking(MakeSeed());
		TestTrue(TEXT("tracking"), Tracker.IsTracking());
		TestTrue(TEXT("marker written while alive"), MarkerFileExists(Path));

		Tracker.StopTracking();
		TestFalse(TEXT("no longer tracking"), Tracker.IsTracking());
		TestFalse(TEXT("marker removed on clean exit"), MarkerFileExists(Path));
	}

	// So the next launch sees no dirty exit.
	{
		FFlockTerminationTracker NextLaunch(true, Path);
		FFlockTerminationMarker Survivor;
		TestFalse(TEXT("nothing survived a clean exit"), NextLaunch.ReadSurvivingMarker(Survivor));
	}

	DeleteTempFile(Path);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockTerminationDirtyExitTest, "Flock.Analytics.Termination.DirtyExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockTerminationDirtyExitTest::RunTest(const FString& Parameters)
{
	// Died in the foreground: no StopTracking, so the marker survives.
	{
		const FString Path = MakeTempMarkerPath();
		{
			FFlockTerminationTracker Tracker(true, Path);
			Tracker.BeginTracking(MakeSeed());
			Tracker.SetServerSessionId(TEXT("srv-9"));
			// ... and the process dies here.
		}
		{
			FFlockTerminationTracker NextLaunch(true, Path);
			FFlockTerminationMarker Survivor;
			TestTrue(TEXT("marker survived"), NextLaunch.ReadSurvivingMarker(Survivor));
			TestEqual(TEXT("session attributed"), Survivor.SessionId, TEXT("sess-1"));
			TestEqual(TEXT("server session attributed"), Survivor.ServerSessionId, TEXT("srv-9"));
			TestEqual(TEXT("player attributed"), Survivor.PlayerId, TEXT("p-1"));
			TestEqual(TEXT("app version carried"), Survivor.AppVersion, TEXT("1.2.3"));
			TestEqual(TEXT("sdk version carried"), Survivor.SdkVersion, TEXT("0.7.0"));
			TestEqual(TEXT("classified abnormal"), FFlockTerminationTracker::Classify(Survivor),
				FString(FFlockTerminationTracker::ClassAbnormal));
		}
		DeleteTempFile(Path);
	}

	// Died backgrounded: OS eviction or a swipe-close, which backgrounds first.
	{
		const FString Path = MakeTempMarkerPath();
		{
			FFlockTerminationTracker Tracker(true, Path);
			Tracker.BeginTracking(MakeSeed());
			Tracker.SetBackgrounded(true);
		}
		{
			FFlockTerminationTracker NextLaunch(true, Path);
			FFlockTerminationMarker Survivor;
			TestTrue(TEXT("marker survived"), NextLaunch.ReadSurvivingMarker(Survivor));
			TestEqual(TEXT("classified background kill"), FFlockTerminationTracker::Classify(Survivor),
				FString(FFlockTerminationTracker::ClassBackgroundKill));
		}
		DeleteTempFile(Path);
	}

	// Came back to the foreground before dying: back to abnormal.
	{
		const FString Path = MakeTempMarkerPath();
		{
			FFlockTerminationTracker Tracker(true, Path);
			Tracker.BeginTracking(MakeSeed());
			Tracker.SetBackgrounded(true);
			Tracker.SetBackgrounded(false);
		}
		{
			FFlockTerminationTracker NextLaunch(true, Path);
			FFlockTerminationMarker Survivor;
			TestTrue(TEXT("marker survived"), NextLaunch.ReadSurvivingMarker(Survivor));
			TestEqual(TEXT("foreground again"), FFlockTerminationTracker::Classify(Survivor),
				FString(FFlockTerminationTracker::ClassAbnormal));
		}
		DeleteTempFile(Path);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockTerminationExceptionFoldingTest, "Flock.Analytics.Termination.ExceptionFolding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockTerminationExceptionFoldingTest::RunTest(const FString& Parameters)
{
	const FString Path = MakeTempMarkerPath();
	{
		FFlockTerminationTracker Tracker(true, Path);
		Tracker.BeginTracking(MakeSeed());

		// Exceptions accumulate in memory: a storm must not become a write storm.
		Tracker.NoteException();
		Tracker.NoteException();
		Tracker.NoteException();
		TestEqual(TEXT("pending in memory"), Tracker.GetPendingExceptionCount(), 3);

		FFlockTerminationMarker OnDisk;
		FFlockTerminationTracker Reader(true, Path);
		TestTrue(TEXT("marker readable"), Reader.ReadSurvivingMarker(OnDisk));
		TestEqual(TEXT("not yet folded to disk"), OnDisk.ExceptionCount, 0);

		// The heartbeat is what persists them.
		Tracker.HandleHeartbeat();
		TestEqual(TEXT("pending cleared"), Tracker.GetPendingExceptionCount(), 0);
		TestTrue(TEXT("marker readable"), Reader.ReadSurvivingMarker(OnDisk));
		TestEqual(TEXT("folded on heartbeat"), OnDisk.ExceptionCount, 3);

		// Backgrounding folds too, since the app may never heartbeat again.
		Tracker.NoteException();
		Tracker.SetBackgrounded(true);
		TestTrue(TEXT("marker readable"), Reader.ReadSurvivingMarker(OnDisk));
		TestEqual(TEXT("folded on background"), OnDisk.ExceptionCount, 4);
	}
	DeleteTempFile(Path);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockTerminationResilienceTest, "Flock.Analytics.Termination.Resilience",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockTerminationResilienceTest::RunTest(const FString& Parameters)
{
	// A corrupt marker is discarded, not re-read on every future launch.
	{
		const FString Path = MakeTempMarkerPath();
		FFileHelper::SaveStringToFile(FString(TEXT("}}garbage{{")), *Path);

		FFlockTerminationTracker Tracker(true, Path);
		FFlockTerminationMarker Survivor;
		TestFalse(TEXT("corrupt marker reports nothing"), Tracker.ReadSurvivingMarker(Survivor));
		TestFalse(TEXT("and is deleted so it cannot poison later launches"), MarkerFileExists(Path));
		DeleteTempFile(Path);
	}

	// Well-formed but unattributable is treated the same way.
	{
		const FString Path = MakeTempMarkerPath();
		FFileHelper::SaveStringToFile(FString(TEXT("{\"last_state\":\"background\"}")), *Path);

		FFlockTerminationTracker Tracker(true, Path);
		FFlockTerminationMarker Survivor;
		TestFalse(TEXT("no session id means no marker"), Tracker.ReadSurvivingMarker(Survivor));
		TestFalse(TEXT("deleted"), MarkerFileExists(Path));
		DeleteTempFile(Path);
	}

	// Disabled: leaves no tombstone at all.
	{
		const FString Path = MakeTempMarkerPath();
		FFlockTerminationTracker Tracker(/*bEnabled*/ false, Path);
		Tracker.BeginTracking(MakeSeed());
		TestFalse(TEXT("not tracking when disabled"), Tracker.IsTracking());
		TestFalse(TEXT("no marker written"), MarkerFileExists(Path));
		DeleteTempFile(Path);
	}

	// But a disabled tracker can still clear a marker left by an earlier, enabled run —
	// otherwise an undeliverable dirty exit would be reported forever.
	{
		const FString Path = MakeTempMarkerPath();
		{
			FFlockTerminationTracker Enabled(true, Path);
			Enabled.BeginTracking(MakeSeed());
		}
		TestTrue(TEXT("marker exists"), MarkerFileExists(Path));

		FFlockTerminationTracker Disabled(false, Path);
		FFlockTerminationMarker Survivor;
		TestTrue(TEXT("disabled tracker can still read it"), Disabled.ReadSurvivingMarker(Survivor));
		Disabled.ClearMarker();
		TestFalse(TEXT("and drop it"), MarkerFileExists(Path));
		DeleteTempFile(Path);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockTerminationLastAliveTest, "Flock.Analytics.Termination.LastAlive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockTerminationLastAliveTest::RunTest(const FString& Parameters)
{
	const FString Path = MakeTempMarkerPath();
	const TSharedRef<FDateTime> Now = MakeShared<FDateTime>(FDateTime(2026, 7, 21, 12, 0, 0));
	const FFlockTerminationTracker::FClock Clock = [Now]() { return *Now; };

	{
		FFlockTerminationTracker Tracker(true, Path, Clock);
		Tracker.BeginTracking(MakeSeed());

		FFlockTerminationTracker Reader(true, Path, Clock);
		FFlockTerminationMarker OnDisk;
		TestTrue(TEXT("readable"), Reader.ReadSurvivingMarker(OnDisk));
		TestEqual(TEXT("death estimate starts at begin"), OnDisk.LastAliveUtc, FDateTime(2026, 7, 21, 12, 0, 0));

		// Each heartbeat pushes the death estimate forward.
		*Now += FTimespan::FromSeconds(60);
		Tracker.HandleHeartbeat();
		TestTrue(TEXT("readable"), Reader.ReadSurvivingMarker(OnDisk));
		TestEqual(TEXT("death estimate advanced"), OnDisk.LastAliveUtc, FDateTime(2026, 7, 21, 12, 1, 0));
	}
	DeleteTempFile(Path);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
