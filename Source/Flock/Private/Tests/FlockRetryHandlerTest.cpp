// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockRetryHandler.h"
#include "Http/FlockResult.h"
#include "Http/FlockError.h"
#include "FlockLogger.h"
#include "Tests/Support/FlockRecordingLogger.h"

namespace
{
	FFlockError Net(int32 Status)
	{
		return FFlockError::Make(EFlockErrorType::Network, TEXT("boom"), Status);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRetryClassificationTest, "Flock.Http.Retry.Classification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRetryClassificationTest::RunTest(const FString& Parameters)
{
	// Non-retryable error classes.
	TestFalse(TEXT("auth no retry"), FFlockRetryHandler::ShouldRetry(FFlockError::Make(EFlockErrorType::Auth, TEXT("x"), 401), true));
	TestFalse(TEXT("validation no retry"), FFlockRetryHandler::ShouldRetry(FFlockError::Make(EFlockErrorType::Validation, TEXT("x"), 400), true));
	TestFalse(TEXT("serialization no retry"), FFlockRetryHandler::ShouldRetry(FFlockError::Make(EFlockErrorType::Serialization, TEXT("x")), true));
	TestFalse(TEXT("cancelled no retry"), FFlockRetryHandler::ShouldRetry(FFlockError::Make(EFlockErrorType::Cancelled, TEXT("x")), true));

	// Idempotent network failures.
	TestTrue(TEXT("500 retry"), FFlockRetryHandler::ShouldRetry(Net(500), true));
	TestFalse(TEXT("404 permanent no retry"), FFlockRetryHandler::ShouldRetry(Net(404), true));
	TestTrue(TEXT("408 retry"), FFlockRetryHandler::ShouldRetry(Net(408), true));
	TestTrue(TEXT("429 retry"), FFlockRetryHandler::ShouldRetry(Net(429), true));

	// Non-idempotent: only provably-not-processed (408/429) may retry.
	TestFalse(TEXT("500 non-idempotent no retry"), FFlockRetryHandler::ShouldRetry(Net(500), false));
	TestTrue(TEXT("429 non-idempotent retry"), FFlockRetryHandler::ShouldRetry(Net(429), false));
	TestTrue(TEXT("408 non-idempotent retry"), FFlockRetryHandler::ShouldRetry(Net(408), false));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRetryBackoffTest, "Flock.Http.Retry.Backoff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRetryBackoffTest::RunTest(const FString& Parameters)
{
	FFlockRetryPolicy Policy;
	Policy.BackoffMultiplier = 2.0;
	Policy.MaxDelaySeconds = 30.f;
	Policy.bUseJitter = false;

	TestEqual(TEXT("grow 1 -> 2"), FFlockRetryHandler::GrowDelay(1.f, Policy), 2.f);
	TestEqual(TEXT("grow caps at max"), FFlockRetryHandler::GrowDelay(20.f, Policy), 30.f);

	FFlockError WithHint = Net(429);
	WithHint.bHasRetryAfter = true;
	WithHint.RetryAfterSeconds = 100.f;
	TestEqual(TEXT("retry-after bounded by max"), FFlockRetryHandler::ResolveDelaySeconds(WithHint, 1.f, Policy), 30.f);

	TestEqual(TEXT("no jitter -> base delay"), FFlockRetryHandler::ResolveDelaySeconds(Net(500), 5.f, Policy), 5.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRetryOrchestrationTest, "Flock.Http.Retry.Orchestration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRetryOrchestrationTest::RunTest(const FString& Parameters)
{
	FFlockRetryPolicy Policy;
	Policy.MaxRetries = 3;
	Policy.InitialDelaySeconds = 0.f;
	Policy.bUseJitter = false;

	const TSharedRef<IFlockLogger> Logger = MakeShared<FFlockNullLogger>();
	FFlockRetryHandler Handler(Policy, Logger);
	Handler.SetScheduler([](float, TFunction<void()> Work) { Work(); }); // run retries synchronously

	int32 Attempts = 0;
	auto Operation = [&Attempts](TFunction<void(TFlockResult<int32>)> Callback) -> FFlockRequestHandle
	{
		++Attempts;
		if (Attempts < 3)
		{
			Callback(TFlockResult<int32>::Fail(Net(500)));
		}
		else
		{
			Callback(TFlockResult<int32>::Ok(42));
		}
		return FFlockRequestHandle();
	};

	bool bSuccess = false;
	int32 Value = 0;
	Handler.Execute<int32>(Operation, [&](TFlockResult<int32> Result)
	{
		bSuccess = Result.bSuccess;
		Value = Result.Value;
	});

	TestTrue(TEXT("succeeded after retries"), bSuccess);
	TestEqual(TEXT("value delivered"), Value, 42);
	TestEqual(TEXT("three attempts"), Attempts, 3);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRetryOverrideAndExhaustionTest, "Flock.Http.Retry.OverrideAndExhaustion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRetryOverrideAndExhaustionTest::RunTest(const FString& Parameters)
{
	FFlockRetryPolicy Policy;
	Policy.MaxRetries = 3;
	Policy.InitialDelaySeconds = 0.f;
	Policy.bUseJitter = false;

	const TSharedRef<IFlockLogger> Logger = MakeShared<FFlockNullLogger>();
	FFlockRetryHandler Handler(Policy, Logger);
	Handler.SetScheduler([](float, TFunction<void()> Work) { Work(); });

	// MaxRetriesOverride = 0: one attempt only, even for a retryable failure. The offline layer's
	// "one attempt, then serve cache" path relies on this contract.
	int32 SingleAttempts = 0;
	auto FailingOperation = [&SingleAttempts](TFunction<void(TFlockResult<int32>)> Callback) -> FFlockRequestHandle
	{
		++SingleAttempts;
		Callback(TFlockResult<int32>::Fail(Net(500)));
		return FFlockRequestHandle();
	};

	bool bFailed = false;
	Handler.Execute<int32>(FailingOperation, [&bFailed](TFlockResult<int32> Result) { bFailed = !Result.bSuccess; },
		/*bIdempotent*/ true, /*MaxRetriesOverride*/ 0);
	TestTrue(TEXT("failure delivered"), bFailed);
	TestEqual(TEXT("override zero -> single attempt"), SingleAttempts, 1);

	// Exhausted budget: initial + MaxRetries attempts, and the LAST failure is what propagates.
	int32 Attempts = 0;
	const int32 Statuses[] = { 500, 502, 503, 504 };
	auto DrainingOperation = [&Attempts, &Statuses](TFunction<void(TFlockResult<int32>)> Callback) -> FFlockRequestHandle
	{
		Callback(TFlockResult<int32>::Fail(Net(Statuses[FMath::Min(Attempts++, 3)])));
		return FFlockRequestHandle();
	};

	int32 FinalStatus = 0;
	Handler.Execute<int32>(DrainingOperation, [&FinalStatus](TFlockResult<int32> Result) { FinalStatus = Result.Error.StatusCode; });
	TestEqual(TEXT("initial + MaxRetries attempts"), Attempts, 4);
	TestEqual(TEXT("last failure propagates"), FinalStatus, 504);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRetryCancellationTest, "Flock.Http.Retry.Cancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRetryCancellationTest::RunTest(const FString& Parameters)
{
	FFlockRetryPolicy Policy;
	Policy.MaxRetries = 3;
	Policy.InitialDelaySeconds = 1.f;
	Policy.bUseJitter = false;

	const TSharedRef<IFlockLogger> Logger = MakeShared<FFlockNullLogger>();
	FFlockRetryHandler Handler(Policy, Logger);

	TFunction<void()> Pending;
	Handler.SetScheduler([&Pending](float, TFunction<void()> Work) { Pending = MoveTemp(Work); }); // defer the retry

	int32 Attempts = 0;
	auto Operation = [&Attempts](TFunction<void(TFlockResult<int32>)> Callback) -> FFlockRequestHandle
	{
		++Attempts;
		Callback(TFlockResult<int32>::Fail(Net(500)));
		return FFlockRequestHandle();
	};

	bool bDone = false;
	EFlockErrorType Type = EFlockErrorType::None;
	FFlockRequestHandle Handle = Handler.Execute<int32>(Operation, [&](TFlockResult<int32> Result)
	{
		bDone = true;
		Type = Result.Error.Type;
	});

	// First attempt failed; a retry is queued (deferred) but nothing delivered yet.
	TestEqual(TEXT("one attempt so far"), Attempts, 1);
	TestFalse(TEXT("not completed yet"), bDone);

	Handle.Cancel();
	if (Pending)
	{
		Pending(); // fire the queued retry — it must see the cancel and stop
	}

	TestTrue(TEXT("completed after cancel"), bDone);
	TestEqual(TEXT("cancelled result"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Cancelled));
	TestEqual(TEXT("no further attempts"), Attempts, 1);

	return true;
}

// ── MaxDelay is the longest wait, jitter included ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRetryJitterNeverPassesMaxDelayTest, "Flock.Http.Retry.JitterNeverPassesMaxDelay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRetryJitterNeverPassesMaxDelayTest::RunTest(const FString& Parameters)
{
	FFlockRetryPolicy Policy;
	Policy.MaxDelaySeconds = 30.f;
	Policy.bUseJitter = true;
	// The backoff grows to MaxDelay and jitter is applied after, so a quarter more could be waited without the cap.
	float Longest = 0.f;
	for (int32 Roll = 0; Roll < 200; ++Roll)
	{
		Longest = FMath::Max(Longest, FFlockRetryHandler::ResolveDelaySeconds(Net(500), Policy.MaxDelaySeconds, Policy));
	}
	TestTrue(FString::Printf(TEXT("no wait passes MaxDelay (longest %.2f s)"), Longest), Longest <= Policy.MaxDelaySeconds);
	TestTrue(TEXT("and the jitter still shortens some"), Longest > 0.f);
	return true;
}

namespace
{
	/** Runs one call under a handler with no waits, answering each attempt from Answers in turn. */
	int32 RunWithAnswers(FFlockRetryHandler& Handler, const TArray<FFlockError>& Answers, bool bIdempotent = true)
	{
		Handler.SetScheduler([](float, TFunction<void()> Work) { Work(); });
		int32 Attempts = 0;
		Handler.Execute<int32>([&Attempts, Answers](TFunction<void(TFlockResult<int32>)> Callback) -> FFlockRequestHandle
		{
			const FFlockError& Answer = Answers[FMath::Min(Attempts, Answers.Num() - 1)];
			++Attempts;
			Callback(TFlockResult<int32>::Fail(Answer));
			return FFlockRequestHandle();
		}, [](TFlockResult<int32>) {}, bIdempotent);
		return Attempts;
	}

	FFlockRetryPolicy NoWaits(int32 MaxRetries)
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = MaxRetries;
		Policy.InitialDelaySeconds = 0.f;
		Policy.MaxDelaySeconds = 0.f;
		Policy.bUseJitter = false;
		return Policy;
	}
}

// ── A failure that is never retried is the caller's to report, so the retry handling warns nothing about it ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRetryNeverRetriedFailureIsNotWarnedAboutTest, "Flock.Http.Retry.NeverRetriedFailureIsNotWarnedAbout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRetryNeverRetriedFailureIsNotWarnedAboutTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
	FFlockRetryHandler Handler(NoWaits(0), Log);
	RunWithAnswers(Handler, { Net(404) });
	RunWithAnswers(Handler, { FFlockError::Make(EFlockErrorType::Validation, TEXT("bad request"), 422) });
	RunWithAnswers(Handler, { Net(500) }, /*bIdempotent*/ false);
	TestEqual(TEXT("with retries off nothing is warned"), Log->Warnings.Num(), 0);
	TestEqual(TEXT("and nothing is an error"), Log->Errors.Num(), 0);

	// A refusal landing on the last try is not warned about either; only the retry before it is.
	const TSharedRef<FFlockRecordingLogger> LastTryLog = MakeShared<FFlockRecordingLogger>();
	FFlockRetryHandler LastTryHandler(NoWaits(1), LastTryLog);
	TestEqual(TEXT("two attempts"), RunWithAnswers(LastTryHandler, { Net(500), Net(404) }), 2);
	TestEqual(TEXT("only the retry is warned about"), LastTryLog->Warnings.Num(), 1);
	TestTrue(TEXT("and it is the retry"), LastTryLog->Warnings.Num() == 1 && LastTryLog->Warnings[0].StartsWith(TEXT("Attempt 1 failed")));
	TestEqual(TEXT("and nothing is an error"), LastTryLog->Errors.Num(), 0);

	// The control: a failure it retries and ran out of tries for is warned about once.
	const TSharedRef<FFlockRecordingLogger> OutOfTriesLog = MakeShared<FFlockRecordingLogger>();
	FFlockRetryHandler OutOfTriesHandler(NoWaits(0), OutOfTriesLog);
	RunWithAnswers(OutOfTriesHandler, { FFlockError::Make(EFlockErrorType::Timeout, TEXT("timeout")) });
	TestEqual(TEXT("a retried failure out of tries is warned about once"), OutOfTriesLog->Warnings.Num(), 1);
	TestTrue(TEXT("saying how many tries it had"),
		OutOfTriesLog->Warnings.Num() == 1 && OutOfTriesLog->Warnings[0].StartsWith(TEXT("Operation failed after 1 attempt(s)")));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
