// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockLifecyclePump.h"
#include "Analytics/FlockSession.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Tests/Support/FlockTestSafeIndex.h"

namespace
{
	FString MakeTempStatePath()
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("session_%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	void DeleteTempFile(const FString& Path)
	{
		IFileManager::Get().Delete(*Path);
	}

	/** A clock the test advances by hand, so nothing has to sleep. */
	struct FFakeClock
	{
		TSharedRef<FDateTime> Now = MakeShared<FDateTime>(FDateTime(2026, 7, 21, 12, 0, 0));

		FFlockSession::FClock Get() const
		{
			TSharedRef<FDateTime> Handle = Now;
			return [Handle]() { return *Handle; };
		}

		void Advance(double Seconds) const { *Now += FTimespan::FromSeconds(Seconds); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSessionLifecycleTest, "Flock.Analytics.Session.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSessionLifecycleTest::RunTest(const FString& Parameters)
{
	const FString StatePath = MakeTempStatePath();
	const FFlockAnalyticsConfig Config;
	const FFakeClock Clock;

	{
		FFlockSession Session(Config, StatePath, Clock.Get());
		TestFalse(TEXT("inactive before start"), Session.IsActive());

		const FString Id = Session.Start(TEXT("p-1"));
		TestFalse(TEXT("issues a session id"), Id.IsEmpty());
		TestTrue(TEXT("active"), Session.IsActive());
		TestEqual(TEXT("player id"), Session.GetPlayerId(), TEXT("p-1"));
		TestEqual(TEXT("first session"), Session.GetSessionNumber(), 1);
		TestTrue(TEXT("snapshot says first session"), Session.TakeSnapshot().IsFirstSession);

		// Starting twice is a no-op, not a second session.
		TestEqual(TEXT("re-start returns the same id"), Session.Start(TEXT("p-2")), Id);
		TestEqual(TEXT("player id unchanged"), Session.GetPlayerId(), TEXT("p-1"));

		// Retag after a late sign-in.
		Session.SetPlayerId(TEXT("p-real"));
		Session.SetServerSessionId(TEXT("srv-1"));
		TestEqual(TEXT("retagged"), Session.GetPlayerId(), TEXT("p-real"));
		TestEqual(TEXT("server id"), Session.GetServerSessionId(), TEXT("srv-1"));

		Session.End();
		TestFalse(TEXT("inactive after end"), Session.IsActive());
		TestFalse(TEXT("snapshot inactive"), Session.TakeSnapshot().IsActive);

		Session.End(); // idempotent
		TestFalse(TEXT("still inactive"), Session.IsActive());
	}

	// The session counter survives the process.
	{
		FFlockSession Session(Config, StatePath, Clock.Get());
		Session.Start(TEXT("p-1"));
		TestEqual(TEXT("counter advanced across instances"), Session.GetSessionNumber(), 2);
		TestFalse(TEXT("no longer the first session"), Session.TakeSnapshot().IsFirstSession);
	}

	DeleteTempFile(StatePath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSessionAccountingTest, "Flock.Analytics.Session.Accounting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSessionAccountingTest::RunTest(const FString& Parameters)
{
	const FString StatePath = MakeTempStatePath();
	FFlockAnalyticsConfig Config;
	Config.bTrackFps = false; // covered separately
	const FFakeClock Clock;

	FFlockSession Session(Config, StatePath, Clock.Get());

	// Ticks before start do not accrue.
	Session.Tick(1.f);
	TestEqual(TEXT("no duration before start"), Session.GetDurationSeconds(), 0.f);

	Session.Start(TEXT("p-1"));
	Session.Tick(0.5f);
	Session.Tick(0.5f);
	TestEqual(TEXT("duration accumulates from tick deltas"), Session.GetDurationSeconds(), 1.f);

	// Backgrounding freezes duration and counts a pause.
	Session.Pause();
	TestTrue(TEXT("paused"), Session.IsPaused());
	Session.Tick(0.5f);
	TestEqual(TEXT("no duration while paused"), Session.GetDurationSeconds(), 1.f);

	// Away for less than the timeout resumes the same session.
	Clock.Advance(5.0);
	TestFalse(TEXT("short absence does not rotate"), Session.Resume());
	TestFalse(TEXT("resumed"), Session.IsPaused());

	Session.Tick(0.5f);
	TestEqual(TEXT("duration resumes"), Session.GetDurationSeconds(), 1.5f);

	// Away longer than SessionTimeoutSeconds (30 by default) asks the caller to rotate.
	Session.Pause();
	Clock.Advance(45.0);
	TestTrue(TEXT("long absence signals rotation"), Session.Resume());

	const FFlockSessionSnapshot Snapshot = Session.TakeSnapshot();
	TestEqual(TEXT("two pauses counted"), Snapshot.PauseCount, 2);
	TestEqual(TEXT("pause duration totalled"), Snapshot.TotalPauseDurationSeconds, 50.f);

	// Negative and zero deltas are ignored rather than rewinding the clock.
	const float Before = Session.GetDurationSeconds();
	Session.Tick(0.f);
	Session.Tick(-5.f);
	TestEqual(TEXT("non-positive deltas ignored"), Session.GetDurationSeconds(), Before);

	DeleteTempFile(StatePath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSessionScreensAndBounceTest, "Flock.Analytics.Session.ScreensAndBounce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSessionScreensAndBounceTest::RunTest(const FString& Parameters)
{
	const FString StatePath = MakeTempStatePath();
	FFlockAnalyticsConfig Config;
	Config.bTrackFps = false;
	const FFakeClock Clock;

	FFlockSession Session(Config, StatePath, Clock.Get());
	Session.Start(TEXT("p-1"));

	// The name list is capped; the view count is not.
	for (int32 Index = 0; Index < FFlockSession::MaxTrackedScreenNames + 25; ++Index)
	{
		Session.RecordScreenView(FString::Printf(TEXT("Screen%d"), Index));
	}
	Session.RecordScreenView(FString()); // empty names are ignored

	const FFlockSessionSnapshot Snapshot = Session.TakeSnapshot();
	TestEqual(TEXT("every view counted"), Snapshot.ScreensViewed, FFlockSession::MaxTrackedScreenNames + 25);
	TestEqual(TEXT("name list capped"), Snapshot.ScreenNames.Num(), FFlockSession::MaxTrackedScreenNames);
	TestEqual(TEXT("kept the earliest"), FlockTestAt(Snapshot.ScreenNames, 0), TEXT("Screen0"));

	// Short session is a bounce (default threshold 10s).
	TestTrue(TEXT("short session bounces"), Session.IsBounce());
	Session.Tick(12.f);
	TestFalse(TEXT("long session does not bounce"), Session.IsBounce());
	TestFalse(TEXT("snapshot agrees"), Session.TakeSnapshot().IsBounce);

	// Heartbeat stamps the snapshot.
	TestTrue(TEXT("no heartbeat yet"), Session.TakeSnapshot().LastHeartbeatUtc.IsEmpty());
	Session.MarkHeartbeat();
	TestFalse(TEXT("heartbeat stamped"), Session.TakeSnapshot().LastHeartbeatUtc.IsEmpty());

	DeleteTempFile(StatePath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSessionFpsTest, "Flock.Analytics.Session.Fps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSessionFpsTest::RunTest(const FString& Parameters)
{
	const FString StatePath = MakeTempStatePath();
	FFlockAnalyticsConfig Config;
	Config.bTrackFps = true;
	Config.FpsSampleIntervalSeconds = 1.f;
	const FFakeClock Clock;

	FFlockSession Session(Config, StatePath, Clock.Get());
	Session.Start(TEXT("p-1"));

	// A partial window produces no sample yet.
	Session.Tick(0.5f);
	TestEqual(TEXT("no sample mid-window"), Session.TakeSnapshot().AverageFps, 0.f);

	// 2 frames across 1s -> 2 fps. (0.5 and 0.25 are exact in binary, so the windows close cleanly.)
	Session.Tick(0.5f);
	{
		const FFlockSessionSnapshot Snapshot = Session.TakeSnapshot();
		TestEqual(TEXT("first sample"), Snapshot.AverageFps, 2.f);
		TestEqual(TEXT("min seeded"), Snapshot.MinFps, 2.f);
		TestEqual(TEXT("max seeded"), Snapshot.MaxFps, 2.f);
	}

	// 4 frames across 1s -> 4 fps; average of the two samples is 3.
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Session.Tick(0.25f);
	}
	{
		const FFlockSessionSnapshot Snapshot = Session.TakeSnapshot();
		TestEqual(TEXT("running average"), Snapshot.AverageFps, 3.f);
		TestEqual(TEXT("min held"), Snapshot.MinFps, 2.f);
		TestEqual(TEXT("max raised"), Snapshot.MaxFps, 4.f);
	}

	// With tracking off nothing is sampled at all.
	{
		FFlockAnalyticsConfig Off;
		Off.bTrackFps = false;
		const FString OffPath = MakeTempStatePath();
		FFlockSession Quiet(Off, OffPath, Clock.Get());
		Quiet.Start(TEXT("p-1"));
		Quiet.Tick(0.5f);
		Quiet.Tick(0.5f);
		TestEqual(TEXT("no fps when disabled"), Quiet.TakeSnapshot().AverageFps, 0.f);
		DeleteTempFile(OffPath);
	}

	DeleteTempFile(StatePath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLifecyclePumpTest, "Flock.Analytics.Pump.Signals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLifecyclePumpTest::RunTest(const FString& Parameters)
{
	FFlockLifecyclePump Pump;
	float TickedSeconds = 0.f;
	int32 TickCount = 0;
	TArray<bool> BackgroundChanges;
	int32 QuitCount = 0;

	Pump.OnTick.AddLambda([&TickedSeconds, &TickCount](float Delta)
	{
		TickedSeconds += Delta;
		++TickCount;
	});
	Pump.OnBackgroundChanged.AddLambda([&BackgroundChanges](bool bBackgrounded)
	{
		BackgroundChanges.Add(bBackgrounded);
	});
	Pump.OnQuit.AddLambda([&QuitCount]() { ++QuitCount; });

	TestFalse(TEXT("not running before start"), Pump.IsRunning());
	Pump.Start();
	TestTrue(TEXT("running"), Pump.IsRunning());
	Pump.Start(); // idempotent
	TestTrue(TEXT("still running"), Pump.IsRunning());

	Pump.TickForTesting(0.5f);
	TestEqual(TEXT("tick delivered"), TickCount, 1);
	TestEqual(TEXT("delta delivered"), TickedSeconds, 0.5f);

	// Backgrounded apps keep ticking on some platforms; session time must not accrue then.
	Pump.SetBackgroundedForTesting(true);
	Pump.TickForTesting(0.5f);
	TestEqual(TEXT("no tick while backgrounded"), TickCount, 1);
	TestEqual(TEXT("no time accrued while backgrounded"), TickedSeconds, 0.5f);

	Pump.SetBackgroundedForTesting(true); // repeat state change is swallowed
	Pump.SetBackgroundedForTesting(false);
	Pump.TickForTesting(0.25f);
	TestEqual(TEXT("ticking resumes"), TickCount, 2);
	TestEqual(TEXT("one background transition each way"), BackgroundChanges.Num(), 2);
	TestTrue(TEXT("went background"), FlockTestAt(BackgroundChanges, 0));
	TestFalse(TEXT("came back"), FlockTestAt(BackgroundChanges, 1));

	// UE can raise both terminate and pre-exit for one shutdown; the session must not end twice.
	Pump.QuitForTesting();
	Pump.QuitForTesting();
	TestEqual(TEXT("quit fires once"), QuitCount, 1);

	Pump.Stop();
	TestFalse(TEXT("stopped"), Pump.IsRunning());
	Pump.Stop(); // idempotent
	TestFalse(TEXT("still stopped"), Pump.IsRunning());
	return true;
}

#endif // WITH_AUTOMATION_TESTS
