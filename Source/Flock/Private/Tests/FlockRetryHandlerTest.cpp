// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockRetryHandler.h"
#include "Http/FlockResult.h"
#include "Http/FlockError.h"
#include "FlockLogger.h"

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

#endif // WITH_AUTOMATION_TESTS
