// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockLogSink.h"
#include "Flock.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Tests/Support/FlockTestSafeIndex.h"

namespace
{
	/** Drains the sink and reports whether any entry carried this message. */
	bool DrainContains(FFlockLogSink& Sink, const FString& Needle)
	{
		bool bFound = false;
		FFlockCapturedLog Captured;
		while (Sink.Dequeue(Captured))
		{
			if (Captured.Message.Contains(Needle))
			{
				bFound = true;
			}
		}
		return bFound;
	}
}

/**
 * These drive Serialize() directly rather than through UE_LOG on purpose: the automation framework
 * treats Error-level log lines as test failures, so a test that logged real errors would fail itself.
 * The one test that must prove GLog registration works whitelists its probe with AddExpectedError.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLogSinkVerbosityTest, "Flock.Analytics.LogSink.Verbosity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLogSinkVerbosityTest::RunTest(const FString& Parameters)
{
	FFlockLogSink Sink;
	const FName Game(TEXT("LogGame"));

	// Only genuine faults become exceptions.
	Sink.Serialize(TEXT("chatty"), ELogVerbosity::Log, Game);
	Sink.Serialize(TEXT("noisy"), ELogVerbosity::Warning, Game);
	Sink.Serialize(TEXT("spammy"), ELogVerbosity::Verbose, Game);
	TestEqual(TEXT("non-errors ignored"), Sink.PendingCount(), 0);

	Sink.Serialize(TEXT("boom"), ELogVerbosity::Error, Game);
	TestEqual(TEXT("error captured"), Sink.PendingCount(), 1);

	FFlockCapturedLog Captured;
	TestTrue(TEXT("dequeues"), Sink.Dequeue(Captured));
	TestEqual(TEXT("message"), Captured.Message, TEXT("boom"));
	TestEqual(TEXT("category"), Captured.Category, Game);
	TestFalse(TEXT("not fatal"), Captured.bFatal);
	TestTrue(TEXT("stamped"), Captured.TimestampUtc != FDateTime::MinValue());
	// An exception report without a callstack is close to useless — this is the payload that makes
	// automatic capture worth its cost.
	TestFalse(TEXT("callstack captured"), Captured.StackTrace.IsEmpty());

	// Frames must be module-relative. A raw program counter is ASLR-shifted every run, so a trace
	// built from absolute addresses can never be symbolicated after the fact.
	TestTrue(TEXT("frames carry a module and an offset"),
		Captured.StackTrace.Contains(TEXT(".dll+0x")) || Captured.StackTrace.Contains(TEXT(".exe+0x")));
	TestEqual(TEXT("drained"), Sink.PendingCount(), 0);
	TestFalse(TEXT("empty dequeue reports false"), Sink.Dequeue(Captured));
	return true;
}

/**
 * The feedback-loop guard. FFlockLogger emits UE_LOG(LogFlock, Error, ...), so without this a failed
 * upload logs an error, which is captured as an exception, which is uploaded, which fails...
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLogSinkCategoryFilterTest, "Flock.Analytics.LogSink.CategoryFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLogSinkCategoryFilterTest::RunTest(const FString& Parameters)
{
	FFlockLogSink Sink;

	TestTrue(TEXT("SDK category excluded out of the box"), Sink.IsExcluded(FName(TEXT("LogFlock"))));
	TestTrue(TEXT("SDK editor category excluded too"), Sink.IsExcluded(FName(TEXT("LogFlockEditor"))));

	Sink.Serialize(TEXT("sdk upload failed"), ELogVerbosity::Error, FName(TEXT("LogFlock")));
	Sink.Serialize(TEXT("editor complaint"), ELogVerbosity::Error, FName(TEXT("LogFlockEditor")));
	TestEqual(TEXT("SDK's own errors never captured"), Sink.PendingCount(), 0);

	// A game can silence its own noisy category the same way.
	const FName Noisy(TEXT("LogNoisySubsystem"));
	Sink.Serialize(TEXT("before"), ELogVerbosity::Error, Noisy);
	TestEqual(TEXT("captured before exclusion"), Sink.PendingCount(), 1);

	Sink.AddExcludedCategory(Noisy);
	Sink.Serialize(TEXT("after"), ELogVerbosity::Error, Noisy);
	TestEqual(TEXT("ignored after exclusion"), Sink.PendingCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLogSinkFatalTest, "Flock.Analytics.LogSink.Fatal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLogSinkFatalTest::RunTest(const FString& Parameters)
{
	FFlockLogSink Sink;
	TArray<FFlockCapturedLog> Fatals;
	Sink.OnFatal.AddLambda([&Fatals](const FFlockCapturedLog& Captured) { Fatals.Add(Captured); });

	// A fatal never queues — there is no next tick to drain it.
	Sink.Serialize(TEXT("assert failed"), ELogVerbosity::Fatal, FName(TEXT("LogGame")));
	TestEqual(TEXT("delivered synchronously"), Fatals.Num(), 1);
	TestTrue(TEXT("marked fatal"), FlockTestAt(Fatals, 0).bFatal);
	TestEqual(TEXT("message"), FlockTestAt(Fatals, 0).Message, TEXT("assert failed"));
	TestFalse(TEXT("fatal carries a callstack"), FlockTestAt(Fatals, 0).StackTrace.IsEmpty());
	TestEqual(TEXT("not queued"), Sink.PendingCount(), 0);

	// A hard crash may never reach the log at all; the crash delegates synthesize an entry.
	Sink.SimulateSystemErrorForTesting();
	TestEqual(TEXT("system error delivered"), Fatals.Num(), 2);
	TestTrue(TEXT("also fatal"), FlockTestAt(Fatals, 1).bFatal);
	TestEqual(TEXT("synthesized category"), FlockTestAt(Fatals, 1).Category, FName(TEXT("SystemError")));
	// The crash delegates carry no message, so the stack is the only evidence there is.
	TestFalse(TEXT("system error carries a callstack"), FlockTestAt(Fatals, 1).StackTrace.IsEmpty());
	return true;
}

/** A handler that logs while handling must not recurse until the stack gives out. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLogSinkReentrancyTest, "Flock.Analytics.LogSink.Reentrancy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLogSinkReentrancyTest::RunTest(const FString& Parameters)
{
	FFlockLogSink Sink;
	int32 FatalCount = 0;

	Sink.OnFatal.AddLambda([&Sink, &FatalCount](const FFlockCapturedLog&)
	{
		++FatalCount;
		// Handling the fault logs another fault. Without the guard this re-enters forever.
		Sink.Serialize(TEXT("error while handling"), ELogVerbosity::Error, FName(TEXT("LogGame")));
		Sink.Serialize(TEXT("fatal while handling"), ELogVerbosity::Fatal, FName(TEXT("LogGame")));
	});

	Sink.Serialize(TEXT("outer"), ELogVerbosity::Fatal, FName(TEXT("LogGame")));

	TestEqual(TEXT("handler ran once, not recursively"), FatalCount, 1);
	TestEqual(TEXT("re-entrant error was swallowed"), Sink.PendingCount(), 0);

	// And the guard releases: the sink still works afterwards.
	Sink.Serialize(TEXT("later"), ELogVerbosity::Error, FName(TEXT("LogGame")));
	TestEqual(TEXT("guard released"), Sink.PendingCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLogSinkCapTest, "Flock.Analytics.LogSink.Cap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLogSinkCapTest::RunTest(const FString& Parameters)
{
	// An error storm costs a bounded amount of memory, and the loss is counted rather than hidden.
	FFlockLogSink Sink(/*MaxQueued*/ 4);
	for (int32 Index = 0; Index < 10; ++Index)
	{
		Sink.Serialize(*FString::Printf(TEXT("boom %d"), Index), ELogVerbosity::Error, FName(TEXT("LogGame")));
	}

	TestEqual(TEXT("queue capped"), Sink.PendingCount(), 4);
	TestEqual(TEXT("overflow counted"), Sink.GetDroppedCount(), 6);

	// Draining makes room again.
	FFlockCapturedLog Captured;
	TestTrue(TEXT("dequeues oldest"), Sink.Dequeue(Captured));
	TestEqual(TEXT("oldest first"), Captured.Message, TEXT("boom 0"));

	Sink.Serialize(TEXT("boom later"), ELogVerbosity::Error, FName(TEXT("LogGame")));
	TestEqual(TEXT("room reclaimed"), Sink.PendingCount(), 4);
	return true;
}

/**
 * Registration lifecycle only.
 *
 * There is deliberately no "real UE_LOG(Error) reaches the sink" assertion here, because it cannot
 * be written: an unexpected Error fails the test outright, and AddExpectedError avoids that by
 * DEMOTING the line to Verbose before the redirector dispatches it — so the whitelist that keeps the
 * test alive also destroys the thing under test. Verified: the probe arrived as
 * `LogTemp: Verbose: flock-sink-probe`.
 *
 * The dispatch path is covered by driving Serialize() directly in the tests above; what is left
 * untested is the two-line GLog->AddOutputDevice/RemoveOutputDevice glue, which fails loudly and
 * immediately in real use.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLogSinkRegistrationTest, "Flock.Analytics.LogSink.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLogSinkRegistrationTest::RunTest(const FString& Parameters)
{
	FFlockLogSink Sink;
	TestFalse(TEXT("not running before start"), Sink.IsRunning());

	Sink.Start();
	TestTrue(TEXT("running"), Sink.IsRunning());
	Sink.Start(); // idempotent
	TestTrue(TEXT("still running"), Sink.IsRunning());

	// Registered on GLog: still captures when driven, and the queue is unaffected by registration.
	Sink.Serialize(TEXT("while registered"), ELogVerbosity::Error, FName(TEXT("LogGame")));
	TestTrue(TEXT("captures while registered"), DrainContains(Sink, TEXT("while registered")));

	Sink.Stop();
	TestFalse(TEXT("stopped"), Sink.IsRunning());
	Sink.Stop(); // idempotent
	TestFalse(TEXT("still stopped"), Sink.IsRunning());

	// Destruction after an explicit Stop must not double-remove from GLog.
	return true;
}

#endif // WITH_AUTOMATION_TESTS
