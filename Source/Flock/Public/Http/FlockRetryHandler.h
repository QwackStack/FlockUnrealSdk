// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockResult.h"
#include "Http/FlockError.h"
#include "Http/FlockRetryPolicy.h"
#include "Http/FlockHttpAdapter.h"
#include "FlockLogger.h"

/**
 * Retries an async operation with exponential backoff + jitter, honoring Retry-After. It drives an
 * "operation factory" (start one attempt, call back with the result) and, on a retryable failure,
 * schedules the next attempt via FTSTicker (game-thread, works in-editor — no UWorld needed).
 *
 * Classification: never retry Cancelled/Auth/Validation/Serialization; never retry
 * permanent 4xx (except 408/429); non-idempotent callers retry only provably-not-processed failures
 * (408/429); delay = Retry-After hint (bounded by MaxDelay) else jittered backoff; after MaxRetries the
 * last failure propagates.
 */
class FLOCK_API FFlockRetryHandler
{
public:
	FFlockRetryHandler(const FFlockRetryPolicy& InPolicy, const TSharedRef<IFlockLogger>& InLogger);

	/** Starts one attempt and reports its result via the supplied callback; returns a handle to cancel it. */
	template <typename T>
	using FOperation = TFunction<FFlockRequestHandle(TFunction<void(TFlockResult<T>)>)>;

	/**
	 * Runs Operation under the retry policy. Pass bIdempotent=false for non-idempotent mutations so
	 * ambiguous failures surface instead of being re-sent. MaxRetriesOverride < 0 uses the policy value
	 * (pass 0 to disable retries for this call). Returns a handle that cancels the in-flight attempt and
	 * any pending retry.
	 */
	template <typename T>
	FFlockRequestHandle Execute(FOperation<T> Operation, TFunction<void(TFlockResult<T>)> OnComplete,
		bool bIdempotent = true, int32 MaxRetriesOverride = -1,
		TFunction<bool(const FFlockError&)> IsExpectedFailure = nullptr)
	{
		const TSharedRef<TRetryState<T>> State = MakeShared<TRetryState<T>>();
		State->Operation = MoveTemp(Operation);
		State->OnComplete = MoveTemp(OnComplete);
		State->IsExpectedFailure = MoveTemp(IsExpectedFailure);
		State->bIdempotent = bIdempotent;
		State->MaxRetries = MaxRetriesOverride >= 0 ? MaxRetriesOverride : Policy.MaxRetries;
		State->Policy = Policy;
		State->Logger = Logger;
		State->Scheduler = Scheduler;
		State->DelaySeconds = Policy.InitialDelaySeconds;
		State->OuterToken = MakeShared<FFlockCancelToken>();

		TWeakPtr<TRetryState<T>> WeakState = State;
		State->OuterToken->Canceller = [WeakState]()
		{
			if (const TSharedPtr<TRetryState<T>> Pinned = WeakState.Pin())
			{
				Pinned->CurrentAttempt.Cancel();
			}
		};

		RunAttempt(State);
		return FFlockRequestHandle(State->OuterToken);
	}

	/** Overrides the retry scheduler (defaults to FTSTicker). Tests inject a synchronous scheduler. */
	void SetScheduler(TFunction<void(float /*DelaySeconds*/, TFunction<void()> /*Work*/)> InScheduler)
	{
		Scheduler = MoveTemp(InScheduler);
	}

	// ── Pure classification/backoff (exposed for unit tests) ──

	/** Whether this error class is retryable for the given idempotency (ignores attempt budget). */
	static bool ShouldRetry(const FFlockError& Error, bool bIdempotent);

	/** Delay before the next attempt: Retry-After hint (bounded by MaxDelay) else jittered backoff. */
	static float ResolveDelaySeconds(const FFlockError& Error, float BaseDelaySeconds, const FFlockRetryPolicy& Policy);

	/** Grows the backoff base delay by the multiplier, capped at MaxDelay. */
	static float GrowDelay(float CurrentDelaySeconds, const FFlockRetryPolicy& Policy);

private:
	template <typename T>
	struct TRetryState
	{
		FOperation<T> Operation;
		TFunction<void(TFlockResult<T>)> OnComplete;
		bool bIdempotent = true;
		int32 MaxRetries = 3;
		int32 Attempt = 0;
		int32 Generation = 0;
		float DelaySeconds = 1.f;
		FFlockRetryPolicy Policy;
		TSharedPtr<IFlockLogger> Logger;
		TFunction<void(float, TFunction<void()>)> Scheduler;
		FFlockRequestHandle CurrentAttempt;
		TSharedPtr<FFlockCancelToken> OuterToken;
		bool bCompleted = false;
		/** Optional: marks an error the caller treats as a normal outcome, so it is not logged as one. */
		TFunction<bool(const FFlockError&)> IsExpectedFailure;

		void Finish(const TFlockResult<T>& Result)
		{
			if (!bCompleted)
			{
				bCompleted = true;
				if (OnComplete)
				{
					OnComplete(Result);
				}
			}
		}
	};

	// A strong ref to State is captured by each pending continuation (in-flight op callback or scheduled
	// retry), so State lives exactly as long as work is outstanding. Invariant: at most one continuation
	// is outstanding at a time until the terminal Finish, so no attempt ever double-fires.
	template <typename T>
	static void RunAttempt(TSharedRef<TRetryState<T>> State)
	{
		if (State->OuterToken->bCancelled)
		{
			State->Finish(TFlockResult<T>::Fail(FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Request cancelled"))));
			return;
		}

		State->Attempt++;
		if (State->Attempt > 1 && State->Logger.IsValid())
		{
			State->Logger->LogDebug(FString::Printf(TEXT("Attempt %d/%d"), State->Attempt, State->MaxRetries + 1));
		}

		// Guard against an Operation that completes synchronously and recurses into the next attempt
		// before returning: only record this attempt's handle if no newer attempt has since started,
		// so CurrentAttempt always points at the genuinely live request (what cancellation must target).
		const int32 ThisGeneration = ++State->Generation;
		const FFlockRequestHandle Handle = State->Operation([State](TFlockResult<T> Result)
		{
			HandleResult(State, Result);
		});
		if (State->Generation == ThisGeneration)
		{
			State->CurrentAttempt = Handle;
		}
	}

	template <typename T>
	static void HandleResult(TSharedRef<TRetryState<T>> State, const TFlockResult<T>& Result)
	{
		if (State->bCompleted)
		{
			return;
		}

		if (Result.bSuccess)
		{
			State->Finish(Result);
			return;
		}

		if (State->OuterToken->bCancelled || Result.Error.Type == EFlockErrorType::Cancelled)
		{
			State->Finish(TFlockResult<T>::Fail(FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Request cancelled"))));
			return;
		}

		const bool bRetryable = ShouldRetry(Result.Error, State->bIdempotent);
		const bool bHaveBudget = State->Attempt <= State->MaxRetries;
		if (!bRetryable || !bHaveBudget)
		{
			if (State->Logger.IsValid())
			{
				// An outcome the caller declared as expected — "already registered", say, which it
				// turns into a success — is not a failure, and logging it as one trains people to
				// ignore real errors. Still worth a breadcrumb at debug level.
				const FString Line = FString::Printf(TEXT("Operation failed after %d attempt(s): %s"),
					State->Attempt, *Result.Error.Message);
				if (State->IsExpectedFailure && State->IsExpectedFailure(Result.Error))
				{
					State->Logger->LogDebug(Line);
				}
				else
				{
					State->Logger->LogError(Line);
				}
			}
			State->Finish(Result);
			return;
		}

		const float Wait = ResolveDelaySeconds(Result.Error, State->DelaySeconds, State->Policy);
		if (State->Logger.IsValid())
		{
			State->Logger->LogWarning(FString::Printf(TEXT("Attempt %d failed: %s. Retrying in %.1fs..."),
				State->Attempt, *Result.Error.Message, Wait));
		}
		State->DelaySeconds = GrowDelay(State->DelaySeconds, State->Policy);

		State->Scheduler(Wait, [State]()
		{
			if (State->OuterToken->bCancelled)
			{
				State->Finish(TFlockResult<T>::Fail(FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Request cancelled"))));
				return;
			}
			RunAttempt(State);
		});
	}

	FFlockRetryPolicy Policy;
	TSharedRef<IFlockLogger> Logger;
	TFunction<void(float, TFunction<void()>)> Scheduler;
};
