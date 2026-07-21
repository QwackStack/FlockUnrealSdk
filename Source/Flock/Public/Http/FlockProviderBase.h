// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockRetryHandler.h"
#include "Http/FlockRetryPolicy.h"
#include "Http/FlockResult.h"
#include "Http/FlockError.h"
#include "FlockLogger.h"

/**
 * Base for SDK providers: it wraps a client call in the retry handler, offers small request guards,
 * and — when an auth session is attached — silently refreshes the access token and replays once when
 * an authenticated call fails with an Auth error. The remaining deferred concern, snapshot/offline
 * cache, slots in here with its own ticket.
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

	/**
	 * Where the calls being dispatched right now come from — appended to every log line as
	 * "<Context> [<origin>]" so a log shows whether a call came from C++ or a Blueprint graph.
	 * Defaults to "C++"; the Blueprint async nodes set it for the duration of their dispatch via
	 * FFlockCallOriginScope. Safe because dispatch is synchronous and game-thread only: the origin is
	 * folded into the log strings before the call returns, so overlapping async calls can't mix it up.
	 */
	void SetCallOrigin(const FString& InOrigin) { CallOrigin = InOrigin; }
	const FString& GetCallOrigin() const { return CallOrigin; }

protected:
	/** Enables silent refresh-on-auth-failure for this provider's calls. Unset = pass-through. */
	void SetAuthSession(const TSharedPtr<FFlockAuthSession>& InSession) { AuthSession = InSession; }

	/** "Email login" -> "Email login [Blueprint 'bpTest']". Evaluate at dispatch, not in a completion. */
	FString DecorateContext(const FString& Context) const
	{
		return FString::Printf(TEXT("%s [%s]"), *Context, *CallOrigin);
	}

	/**
	 * Runs Operation under the retry handler, logging a failure with Context. Pass bIdempotent=false for
	 * non-idempotent mutations (e.g. currency grants) so ambiguous failures surface instead of re-sending.
	 *
	 * With an auth session set and bAllowAuthRetry, an Auth failure while signed in triggers one silent
	 * token refresh and a single replay of Operation (no fresh retry budget — the pre-refresh attempts
	 * consumed it). Operations must fetch their auth headers inside the operation lambda so the replay
	 * carries the refreshed bearer. Auth calls themselves pass bAllowAuthRetry=false.
	 */
	template <typename T>
	FFlockRequestHandle Execute(FFlockRetryHandler::FOperation<T> Operation, TFunction<void(TFlockResult<T>)> OnComplete,
		const FString& Context, bool bIdempotent = true, int32 MaxRetriesOverride = -1, bool bAllowAuthRetry = true)
	{
		const TSharedRef<IFlockLogger> Log = Logger;
		// Resolved now, at dispatch, so the completion still reports the origin that made the call.
		const FString LoggedContext = DecorateContext(Context);
		TFunction<void(TFlockResult<T>)> Finish = [Log, LoggedContext, OnComplete](TFlockResult<T> Result)
		{
			if (!Result.bSuccess)
			{
				Log->LogError(FString::Printf(TEXT("%s failed: %s"), *LoggedContext, *Result.Error.Message));
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		};

		FFlockRetryHandler::FOperation<T> ReplayOperation = Operation; // copy held for the post-refresh replay
		const TSharedPtr<FFlockAuthSession> Session = AuthSession;
		return RetryHandler.Execute<T>(MoveTemp(Operation),
			[Log, Finish, ReplayOperation, Session, bAllowAuthRetry](TFlockResult<T> Result)
			{
				const bool bTryRefresh = !Result.bSuccess && Result.Error.Type == EFlockErrorType::Auth
					&& bAllowAuthRetry && Session.IsValid() && Session->IsAuthenticated();
				if (!bTryRefresh)
				{
					Finish(Result);
					return;
				}

				Log->LogDebug(TEXT("Access token rejected; attempting silent refresh"));
				Session->RefreshAccessToken([Finish, ReplayOperation, Result](bool bRefreshed)
				{
					if (!bRefreshed)
					{
						// Surface the original error; the session already raised OnAuthExpired if terminal.
						Finish(Result);
						return;
					}
					ReplayOperation([Finish](TFlockResult<T> ReplayResult)
					{
						Finish(ReplayResult);
					});
				});
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

private:
	TSharedPtr<FFlockAuthSession> AuthSession;
	FString CallOrigin = TEXT("C++");
};

/**
 * Tags a provider's calls with their caller for the duration of a synchronous dispatch, then restores
 * the previous origin. The Blueprint async nodes wrap their provider call in one of these so the log
 * names the graph that made the request; anything dispatched outside a scope logs as "C++".
 */
struct FLOCK_API FFlockCallOriginScope
{
	FFlockCallOriginScope(FFlockProviderBase& InProvider, const FString& Origin)
		: Provider(InProvider)
		, Previous(InProvider.GetCallOrigin())
	{
		Provider.SetCallOrigin(Origin);
	}

	~FFlockCallOriginScope()
	{
		Provider.SetCallOrigin(Previous);
	}

	FFlockProviderBase& Provider;
	FString Previous;
};
