// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockRetryHandler.h"
#include "Http/FlockRetryPolicy.h"
#include "Http/FlockResult.h"
#include "Http/FlockError.h"
#include "FlockLogger.h"

/**
 * Base for SDK providers: it wraps a client call in the retry handler and offers small request guards.
 * Two further concerns — auth-token-refresh-on-401 and snapshot/offline cache — are deferred to their
 * own tickets (no token source or snapshot store exists yet), and this base is shaped so they slot in
 * without reshaping it.
 *
 * No concrete providers exist yet; this ships as the base future providers inherit.
 */
class FLOCK_API FFlockProviderBase
{
public:
	FFlockProviderBase(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
		const TSharedRef<IFlockLogger>& InLogger)
		: Client(InClient)
		, Logger(InLogger)
		, RetryHandler(InPolicy, InLogger)
	{
	}

	virtual ~FFlockProviderBase() = default;

protected:
	/**
	 * Runs Operation under the retry handler, logging a failure with Context. Pass bIdempotent=false for
	 * non-idempotent mutations (e.g. currency grants) so ambiguous failures surface instead of re-sending.
	 */
	template <typename T>
	FFlockRequestHandle Execute(FFlockRetryHandler::FOperation<T> Operation, TFunction<void(TFlockResult<T>)> OnComplete,
		const FString& Context, bool bIdempotent = true, int32 MaxRetriesOverride = -1)
	{
		const TSharedRef<IFlockLogger> Log = Logger;
		return RetryHandler.Execute<T>(MoveTemp(Operation),
			[Log, Context, OnComplete](TFlockResult<T> Result)
			{
				if (!Result.bSuccess)
				{
					Log->LogError(FString::Printf(TEXT("%s failed: %s"), *Context, *Result.Error.Message));
				}
				if (OnComplete)
				{
					OnComplete(Result);
				}
			},
			bIdempotent, MaxRetriesOverride);
	}

	/** Reports a Validation failure and returns false when a required argument is empty. */
	template <typename T>
	bool RequireNotEmpty(const FString& Value, const FString& Name, const TFunction<void(TFlockResult<T>)>& OnComplete) const
	{
		if (Value.IsEmpty())
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<T>::Fail(FFlockError::Make(EFlockErrorType::Validation,
					FString::Printf(TEXT("%s cannot be null or empty"), *Name))));
			}
			return false;
		}
		return true;
	}

	TSharedRef<FFlockHttpClient> Client;
	TSharedRef<IFlockLogger> Logger;
	FFlockRetryHandler RetryHandler;
};
