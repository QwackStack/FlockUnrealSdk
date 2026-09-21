// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockLogSink.h"
#include "Flock.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Tests/Support/FlockTestSafeIndex.h"
#include "UObject/Script.h"

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

	// The Fatal line is this process's crash. The engine then raises its crash delegates, and they add nothing.
	Sink.SimulateSystemErrorForTesting(TEXT("assert failed"));
	Sink.Serialize(TEXT("fatal after the crash"), ELogVerbosity::Fatal, FName(TEXT("LogGame")));
	TestEqual(TEXT("one crash, one report"), Fatals.Num(), 1);
	return true;
}

/**
 * A hard crash may never reach the log at all, so the crash delegates make a report of their own. The engine raises
 * them up to three times for one crash, and each used to become its own "Unhandled system error" on the dashboard.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLogSinkOneReportPerCrashTest, "Flock.Analytics.LogSink.OneReportPerCrash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLogSinkOneReportPerCrashTest::RunTest(const FString& Parameters)
{
	FFlockLogSink Sink;
	TArray<FFlockCapturedLog> Fatals;
	Sink.OnFatal.AddLambda([&Fatals](const FFlockCapturedLog& Captured) { Fatals.Add(Captured); });

	// What Windows does for one assertion: the crash-reporting thread, the error handler, then shutting down after it.
	const TCHAR* Record = TEXT("Assertion failed: bCreatingCDO || !InOuter [File:UObjectGlobals.cpp] [Line: 3977]\r\n")
		TEXT("NewObject with an outer of the wrong class\r\n\r\n");
	Sink.SimulateSystemErrorForTesting(Record);
	Sink.SimulateSystemErrorForTesting(Record);
	Sink.SimulateSystemErrorForTesting(Record);

	TestEqual(TEXT("one crash, one report"), Fatals.Num(), 1);
	const FFlockCapturedLog Crash = FlockTestAt(Fatals, 0);
	TestTrue(TEXT("fatal"), Crash.bFatal);
	TestEqual(TEXT("filed as a system error"), Crash.Category, FName(TEXT("SystemError")));
	TestEqual(TEXT("from the crash path"), Crash.Source, FString(TEXT("crash")));
	TestEqual(TEXT("named by what the engine wrote down"), Crash.Message,
		FString(TEXT("Assertion failed: bCreatingCDO || !InOuter [File:UObjectGlobals.cpp] [Line: 3977]")));
	TestFalse(TEXT("with a callstack"), Crash.StackTrace.IsEmpty());

	// Nor does a Fatal line the dying process logs afterwards start a second report.
	Sink.Serialize(TEXT("fatal while going down"), ELogVerbosity::Fatal, FName(TEXT("LogGame")));
	TestEqual(TEXT("still one"), Fatals.Num(), 1);
	return true;
}

/** What a crash report is called when the engine wrote something down, and when it wrote nothing. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLogSinkDescribeCrashTest, "Flock.Analytics.LogSink.DescribeCrash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLogSinkDescribeCrashTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("the first line of an assertion"),
		FFlockLogSink::DescribeCrash(TEXT("Assertion failed: Index >= 0 [File:A.cpp] [Line: 12]\r\nwhy\r\n")),
		FString(TEXT("Assertion failed: Index >= 0 [File:A.cpp] [Line: 12]")));
	TestEqual(TEXT("blank lines and spaces before it are skipped"),
		FFlockLogSink::DescribeCrash(TEXT("\r\n   \r\n  Unhandled Exception: EXCEPTION_ACCESS_VIOLATION reading address 0x0  \n")),
		FString(TEXT("Unhandled Exception: EXCEPTION_ACCESS_VIOLATION reading address 0x0")));
	TestEqual(TEXT("nothing written down"), FFlockLogSink::DescribeCrash(TEXT("")), FString(TEXT("Unhandled system error")));
	TestEqual(TEXT("only blank lines"), FFlockLogSink::DescribeCrash(TEXT(" \r\n\t\r\n")), FString(TEXT("Unhandled system error")));
	TestEqual(TEXT("no record at all"), FFlockLogSink::DescribeCrash(nullptr), FString(TEXT("Unhandled system error")));

	// A crash with nothing written down is still reported, under the plain name.
	FFlockLogSink Sink;
	TArray<FFlockCapturedLog> Fatals;
	Sink.OnFatal.AddLambda([&Fatals](const FFlockCapturedLog& Captured) { Fatals.Add(Captured); });
	Sink.SimulateSystemErrorForTesting();
	TestEqual(TEXT("reported"), Fatals.Num(), 1);
	TestEqual(TEXT("under the plain name"), FlockTestAt(Fatals, 0).Message, FString(TEXT("Unhandled system error")));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLogSinkScriptExceptionTypesTest, "Flock.Analytics.LogSink.ScriptExceptionTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLogSinkScriptExceptionTypesTest::RunTest(const FString& Parameters)
{
	// Faults are reported, under the spelling a dashboard filters on.
	TestEqual(TEXT("access violation"),
		FFlockLogSink::ScriptExceptionTypeToWire(EBlueprintExceptionType::AccessViolation), TEXT("access_violation"));
	TestEqual(TEXT("infinite loop"),
		FFlockLogSink::ScriptExceptionTypeToWire(EBlueprintExceptionType::InfiniteLoop), TEXT("infinite_loop"));
	TestEqual(TEXT("non-fatal error"),
		FFlockLogSink::ScriptExceptionTypeToWire(EBlueprintExceptionType::NonFatalError), TEXT("non_fatal_error"));
	TestEqual(TEXT("fatal error"),
		FFlockLogSink::ScriptExceptionTypeToWire(EBlueprintExceptionType::FatalError), TEXT("fatal_error"));
	TestEqual(TEXT("abort execution"),
		FFlockLogSink::ScriptExceptionTypeToWire(EBlueprintExceptionType::AbortExecution), TEXT("abort_execution"));
	TestTrue(TEXT("a fault is reportable"), FFlockLogSink::IsReportableScriptException(EBlueprintExceptionType::AccessViolation));

	// The debugger's own traffic rides the same delegate and is not a fault.
	TestFalse(TEXT("breakpoint"), FFlockLogSink::IsReportableScriptException(EBlueprintExceptionType::Breakpoint));
	TestFalse(TEXT("tracepoint"), FFlockLogSink::IsReportableScriptException(EBlueprintExceptionType::Tracepoint));
	TestFalse(TEXT("wire tracepoint"), FFlockLogSink::IsReportableScriptException(EBlueprintExceptionType::WireTracepoint));
	TestTrue(TEXT("and has no wire name"), FFlockLogSink::ScriptExceptionTypeToWire(EBlueprintExceptionType::Breakpoint).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLogSinkScriptExceptionCaptureTest, "Flock.Analytics.LogSink.ScriptExceptionCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLogSinkScriptExceptionCaptureTest::RunTest(const FString& Parameters)
{
	// Never started, so the capture is under test on its own: a sink that never bound writes nothing back to the log.
	FFlockLogSink Sink(/*MaxQueued*/ 2);
	const FString ScriptStack = TEXT("Script call stack:\n\tBP_Player_C.ExecuteUbergraph_BP_Player\n\tBP_Player_C.ReceiveTick");

	Sink.SimulateScriptExceptionForTesting(EBlueprintExceptionType::AccessViolation,
		TEXT("Accessed None trying to read property Weapon"), ScriptStack);
	TestEqual(TEXT("queued like an error"), Sink.PendingCount(), 1);

	FFlockCapturedLog Captured;
	TestTrue(TEXT("dequeues"), Sink.Dequeue(Captured));
	TestEqual(TEXT("the engine's description"), Captured.Message, TEXT("Accessed None trying to read property Weapon"));
	TestEqual(TEXT("under the engine's script category"), Captured.Category, FName(TEXT("LogScript")));
	TestEqual(TEXT("marked as Blueprint"), Captured.Source, TEXT("blueprint"));
	TestEqual(TEXT("with its kind"), Captured.ScriptExceptionType, TEXT("access_violation"));
	TestEqual(TEXT("carrying the Blueprint call stack, which is what locates the node"), Captured.StackTrace, ScriptStack);
	TestFalse(TEXT("not fatal: the engine carries on"), Captured.bFatal);
	TestTrue(TEXT("stamped"), Captured.TimestampUtc != FDateTime::MinValue());

	// A description-less exception still says what it is.
	Sink.SimulateScriptExceptionForTesting(EBlueprintExceptionType::NonFatalError, FString(), ScriptStack);
	TestTrue(TEXT("dequeues"), Sink.Dequeue(Captured));
	TestEqual(TEXT("named when the engine gave nothing"), Captured.Message, TEXT("Blueprint script exception"));

	// The debugger's traffic never becomes a report.
	Sink.SimulateScriptExceptionForTesting(EBlueprintExceptionType::Breakpoint, TEXT("breakpoint hit"), ScriptStack);
	Sink.SimulateScriptExceptionForTesting(EBlueprintExceptionType::Tracepoint, TEXT("tracepoint"), ScriptStack);
	Sink.SimulateScriptExceptionForTesting(EBlueprintExceptionType::WireTracepoint, TEXT("wire tracepoint"), ScriptStack);
	TestEqual(TEXT("debugger traffic ignored"), Sink.PendingCount(), 0);

	// A Blueprint storm is bounded by the same cap as an error storm.
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Sink.SimulateScriptExceptionForTesting(EBlueprintExceptionType::AccessViolation, TEXT("storm"), ScriptStack);
	}
	TestEqual(TEXT("capped"), Sink.PendingCount(), 2);
	TestEqual(TEXT("overflow counted"), Sink.GetDroppedCount(), 1);
	while (Sink.Dequeue(Captured))
	{
	}

	// A project silences Blueprint reports the way it silences any category.
	Sink.AddExcludedCategory(FName(TEXT("LogScript")));
	Sink.SimulateScriptExceptionForTesting(EBlueprintExceptionType::AccessViolation, TEXT("silenced"), ScriptStack);
	TestEqual(TEXT("excluded by category"), Sink.PendingCount(), 0);

	TestEqual(TEXT("nothing written back without having bound"), Sink.GetWrittenBackScriptWarningCountForTesting(), 0);
	return true;
}

// Client context as well: the editor's Blueprint debugger already listens, so only a game process reaches the branch
// where this sink is the first listener and owes the engine's line back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLogSinkScriptExceptionBindingTest, "Flock.Analytics.LogSink.ScriptExceptionBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLogSinkScriptExceptionBindingTest::RunTest(const FString& Parameters)
{
	// Whether something already listens depends on where the suite runs — the editor's Blueprint debugger does — so
	// the written-back line is asserted against that rather than assumed.
	const bool bOthersListening = FBlueprintCoreDelegates::OnScriptException.IsBound();
	const FString ScriptStack = TEXT("Script call stack:\n\tBP_Flock_SinkProbe_C.ReceiveTick");

	FFlockLogSink Sink;
	TestFalse(TEXT("not bound before start"), FBlueprintCoreDelegates::OnScriptException.IsBoundToObject(&Sink));
	Sink.Start();
	TestTrue(TEXT("bound after start"), FBlueprintCoreDelegates::OnScriptException.IsBoundToObject(&Sink));
	TestTrue(TEXT("and says so"), Sink.IsListeningToScriptExceptions());

	if (!bOthersListening)
	{
		// The engine logs this line only while nothing is bound, so binding would have taken it away.
		AddExpectedMessagePlain(TEXT("BP_Flock_SinkProbe_C.ReceiveTick"), ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains, 1);
	}
	Sink.SimulateScriptExceptionForTesting(EBlueprintExceptionType::AccessViolation, TEXT("Accessed None"), ScriptStack);
	TestEqual(TEXT("the engine's stack line is written back only when this sink took it away"),
		Sink.GetWrittenBackScriptWarningCountForTesting(), bOthersListening ? 0 : 1);

	// The debugger's traffic owes no line.
	Sink.SimulateScriptExceptionForTesting(EBlueprintExceptionType::Breakpoint, TEXT("breakpoint hit"), ScriptStack);
	TestEqual(TEXT("no line for a breakpoint"), Sink.GetWrittenBackScriptWarningCountForTesting(), bOthersListening ? 0 : 1);

	// Both answers, forced, so the branch this process's listeners did not choose is exercised as well.
	const int32 WrittenBefore = Sink.GetWrittenBackScriptWarningCountForTesting();
	Sink.SetWriteBackScriptWarningForTesting(true);
	AddExpectedMessagePlain(TEXT("BP_Flock_SinkProbe_C.WhenFirstListener"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	Sink.SimulateScriptExceptionForTesting(EBlueprintExceptionType::AccessViolation, TEXT("Accessed None"),
		TEXT("Script call stack:\n\tBP_Flock_SinkProbe_C.WhenFirstListener"));
	TestEqual(TEXT("written back when this sink is the first listener"),
		Sink.GetWrittenBackScriptWarningCountForTesting(), WrittenBefore + 1);
	Sink.SetWriteBackScriptWarningForTesting(false);
	Sink.SimulateScriptExceptionForTesting(EBlueprintExceptionType::AccessViolation, TEXT("Accessed None"),
		TEXT("Script call stack:\n\tBP_Flock_SinkProbe_C.WhenAnotherListens"));
	TestEqual(TEXT("not written back while another listener still gets the engine's line"),
		Sink.GetWrittenBackScriptWarningCountForTesting(), WrittenBefore + 1);

	Sink.Stop();
	// A sink destroyed while still bound would be called through a dangling pointer on the next script fault.
	TestFalse(TEXT("unbound on stop"), FBlueprintCoreDelegates::OnScriptException.IsBoundToObject(&Sink));
	TestFalse(TEXT("and says so"), Sink.IsListeningToScriptExceptions());
	return true;
}

#endif // WITH_AUTOMATION_TESTS
