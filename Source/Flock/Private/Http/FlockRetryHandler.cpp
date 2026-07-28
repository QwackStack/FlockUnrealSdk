// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Http/FlockRetryHandler.h"
#include "Containers/Ticker.h"

namespace
{
	/** Applies ±25% jitter to a backoff delay (no-op when jitter is disabled). */
	float CalculateDelay(float BaseDelaySeconds, const FFlockRetryPolicy& Policy)
	{
		if (!Policy.bUseJitter)
		{
			return BaseDelaySeconds;
		}
		const float JitterFactor = 0.75f + FMath::FRand() * 0.5f;
		return BaseDelaySeconds * JitterFactor;
	}
}

FFlockRetryHandler::FFlockRetryHandler(const FFlockRetryPolicy& InPolicy, const TSharedRef<IFlockLogger>& InLogger)
	: Policy(InPolicy)
	, Logger(InLogger)
{
	// Default scheduler: fire once after DelaySeconds on the core ticker (game thread, editor-safe).
	Scheduler = [](float DelaySeconds, TFunction<void()> Work)
	{
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([WorkCopy = MoveTemp(Work)](float) { WorkCopy(); return false; }),
			DelaySeconds);
	};
}

bool FFlockRetryHandler::ShouldRetry(const FFlockError& Error, bool bIdempotent)
{
	switch (Error.Type)
	{
	case EFlockErrorType::None:
	case EFlockErrorType::Cancelled:
	case EFlockErrorType::Auth:
	case EFlockErrorType::Validation:
	case EFlockErrorType::Serialization:
		return false;
	default:
		break; // Network / Timeout / Connection — potentially retryable
	}

	// Permanent 4xx is an authoritative answer; the server won't change its mind.
	if (FFlockError::IsPermanentStatus(Error.StatusCode))
	{
		return false;
	}

	// Non-idempotent callers only retry failures the server provably didn't process (408/429);
	// ambiguous ones (client timeout, dropped connection, 5xx) might have committed, so surface them.
	if (!bIdempotent && !FFlockError::IsNotProcessed(Error.StatusCode))
	{
		return false;
	}

	return true;
}

float FFlockRetryHandler::ResolveDelaySeconds(const FFlockError& Error, float BaseDelaySeconds, const FFlockRetryPolicy& Policy)
{
	// Honor a server Retry-After hint when present (bounded by MaxDelay), else jittered backoff.
	if (Error.bHasRetryAfter)
	{
		return FMath::Min(Error.RetryAfterSeconds, Policy.MaxDelaySeconds);
	}
	return CalculateDelay(BaseDelaySeconds, Policy);
}

float FFlockRetryHandler::GrowDelay(float CurrentDelaySeconds, const FFlockRetryPolicy& Policy)
{
	const float Grown = CurrentDelaySeconds * static_cast<float>(Policy.BackoffMultiplier);
	return FMath::Min(Grown, Policy.MaxDelaySeconds);
}
