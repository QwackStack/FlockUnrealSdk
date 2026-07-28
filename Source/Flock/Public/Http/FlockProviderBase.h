// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockRetryHandler.h"
#include "Http/FlockRetryPolicy.h"
#include "Http/FlockResult.h"
#include "Http/FlockError.h"
#include "Http/FlockJsonUtils.h"
#include "Http/FlockSnapshotStore.h"
#include "FlockLogger.h"

/**
 * Base for SDK providers: it wraps a client call in the retry handler, offers small request guards,
 * and — when an auth session is attached — silently refreshes the access token and replays once when
 * an authenticated call fails with an Auth error.
 *
 * It also owns the read-side offline snapshot: FetchWithSnapshot(List)/FetchAtScope run an operation and,
 * with a store attached, cache the success and serve the last-known value when the network is down. The
 * store is optional — without one (auth/analytics never attach it, offline cache disabled) every fetch is
 * a plain Execute.
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
	 *
	 * Also published ambiently (FFlockCallOrigin) so the HTTP layer underneath can stamp the same origin
	 * onto its request/response trace — otherwise a provider line names the caller and the network lines
	 * below it do not, which is the half that matters when a call fails. FFlockCallOriginScope restores
	 * both on the way out, because it restores through this setter.
	 */
	void SetCallOrigin(const FString& InOrigin)
	{
		CallOrigin = InOrigin;
		FFlockCallOrigin::Set(InOrigin);
	}
	const FString& GetCallOrigin() const { return CallOrigin; }

	/**
	 * Attaches the offline snapshot store and the game version that scopes its entries. Without this,
	 * FetchWithSnapshot(List)/FetchAtScope degrade to a plain Execute. Set once at construction.
	 */
	void SetSnapshotStore(const TSharedPtr<FFlockSnapshotStore>& InStore, const FString& InGameVersionId)
	{
		SnapshotStore = InStore;
		GameVersionId = InGameVersionId;
	}

	/**
	 * Overrides the "is the server reachable" probe (defaults to always-reachable). UE has no dependable
	 * cross-platform reachability signal, so the default always attempts the network and lets the failure
	 * path serve cache; the seam lets tests force the offline branch deterministically.
	 */
	void SetReachabilityProbe(TFunction<bool()> InProbe) { ReachabilityProbe = MoveTemp(InProbe); }

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
		const FString& Context, bool bIdempotent = true, int32 MaxRetriesOverride = -1, bool bAllowAuthRetry = true,
		TFunction<bool(const FFlockError&)> IsExpectedFailure = nullptr)
	{
		const TSharedRef<IFlockLogger> Log = Logger;
		// Resolved now, at dispatch, so the completion still reports the origin that made the call.
		const FString LoggedContext = DecorateContext(Context);
		TFunction<void(TFlockResult<T>)> Finish =
			[Log, LoggedContext, OnComplete, IsExpectedFailure](TFlockResult<T> Result)
		{
			if (!Result.bSuccess)
			{
				// Some failures are a normal outcome the caller converts into a success (registering
				// an identity that already exists). Logging those as errors is noise that devalues
				// the real ones, so the caller can declare them and they drop to debug.
				const FString Line = FString::Printf(TEXT("%s failed: %s"), *LoggedContext, *Result.Error.Message);
				if (IsExpectedFailure && IsExpectedFailure(Result.Error))
				{
					Log->LogDebug(Line);
				}
				else
				{
					Log->LogError(Line);
				}
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
			bIdempotent, MaxRetriesOverride, IsExpectedFailure);
	}

	// ── Snapshot-backed fetches ──

	/** Scope for a category's snapshots: "<GameVersionId>/<category>", so a version switch parks stale trees. */
	FString GetSnapshotScope(const FString& Category) const
	{
		return FString::Printf(TEXT("%s/%s"), *GameVersionId, *Category);
	}

	/**
	 * The snapshot store itself, for the one provider that needs to write something other than a fetch
	 * result: the command provider persists its offline queue through it. May be null (cache disabled) —
	 * callers must handle that, which for the queue means "offline writes are not durable this run".
	 */
	const TSharedPtr<FFlockSnapshotStore>& GetSnapshotStore() const { return SnapshotStore; }

	/** Drops a whole category's snapshots (e.g. on a provider ClearCache). No-op without a store. */
	void DeleteSnapshotCategory(const FString& Category)
	{
		if (SnapshotStore.IsValid())
		{
			SnapshotStore->DeleteScope(GetSnapshotScope(Category));
		}
	}

	/**
	 * Runs Operation and snapshots its success under "<version>/<Category>"/Key, serving the cached value
	 * when the network is down. Three branches: cache + unreachable -> serve cache, no call; cache present
	 * -> one attempt then cache on a non-permanent failure; no cache -> full retry budget, failure
	 * propagates. A permanent failure (a 4xx that is an authoritative answer, e.g. a deleted config)
	 * propagates even with a cache. Without a store this is a plain Execute.
	 */
	template <typename T>
	FFlockRequestHandle FetchWithSnapshot(const FString& Category, const FString& Key,
		FFlockRetryHandler::FOperation<T> Operation, const FString& Context, TFunction<void(TFlockResult<T>)> OnComplete)
	{
		return FetchAtScope<T>(GetSnapshotScope(Category), Key, MoveTemp(Operation), Context, MoveTemp(OnComplete));
	}

	/** FetchWithSnapshot for a list result (TArray<T>). Same policy; array (de)serialization for the cache. */
	template <typename T>
	FFlockRequestHandle FetchListWithSnapshot(const FString& Category, const FString& Key,
		FFlockRetryHandler::FOperation<TArray<T>> Operation, const FString& Context, TFunction<void(TFlockResult<TArray<T>>)> OnComplete)
	{
		return FetchSnapshotCore<TArray<T>>(GetSnapshotScope(Category), Key, MoveTemp(Operation), Context,
			[](const TArray<T>& Value, FString& Out) { return FFlockJsonUtils::ArrayToPlainJson(Value, Out); },
			[](const FString& In, TArray<T>& Out) { return FFlockJsonUtils::ArrayFromPlainJson(In, Out); },
			MoveTemp(OnComplete));
	}

	/**
	 * FetchWithSnapshot at an explicit scope rather than a category — the escape hatch for a lookup that
	 * runs before the version id is known (a by-name version resolve, kept on BootstrapScope).
	 */
	template <typename T>
	FFlockRequestHandle FetchAtScope(const FString& Scope, const FString& Key,
		FFlockRetryHandler::FOperation<T> Operation, const FString& Context, TFunction<void(TFlockResult<T>)> OnComplete)
	{
		return FetchSnapshotCore<T>(Scope, Key, MoveTemp(Operation), Context,
			[](const T& Value, FString& Out) { return FFlockJsonUtils::StructToPlainJson(Value, Out); },
			[](const FString& In, T& Out) { return FFlockJsonUtils::PlainJsonToStruct(In, Out); },
			MoveTemp(OnComplete));
	}

	/** True when the server is considered reachable (default: always). */
	bool IsServerReachable() const { return !ReachabilityProbe || ReachabilityProbe(); }

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
	/**
	 * The one place the three-branch snapshot policy lives; the public fetches are thin type wrappers that
	 * supply the (de)serialization for a single struct or a list. Serialize/Deserialize round-trip the
	 * value through the snapshot's plain (PascalCase, no-transform) format.
	 */
	template <typename TValue>
	FFlockRequestHandle FetchSnapshotCore(const FString& Scope, const FString& Key,
		FFlockRetryHandler::FOperation<TValue> Operation, const FString& Context,
		TFunction<bool(const TValue&, FString&)> Serialize, TFunction<bool(const FString&, TValue&)> Deserialize,
		TFunction<void(TFlockResult<TValue>)> OnComplete)
	{
		const TSharedPtr<FFlockSnapshotStore> Store = SnapshotStore;
		if (!Store.IsValid())
		{
			return Execute<TValue>(MoveTemp(Operation), MoveTemp(OnComplete), Context);
		}

		FString Cached;
		const bool bHasCache = Store->TryRead(Scope, Key, Cached);

		// Branch 1: a cache in hand and no connectivity — skip the network entirely.
		if (bHasCache && !IsServerReachable())
		{
			TValue Value;
			if (Deserialize(Cached, Value))
			{
				Logger->LogWarning(FString::Printf(TEXT("%s: serving cached snapshot (no connectivity)"), *Context));
				if (OnComplete)
				{
					OnComplete(TFlockResult<TValue>::Ok(Value));
				}
				return FFlockRequestHandle();
			}
			// Unreadable cache: fall through and try the network after all.
		}

		// Branch 2/3: one attempt when a cache can catch a failure, full budget otherwise.
		const int32 RetryBudget = bHasCache ? 0 : -1;
		const TSharedRef<IFlockLogger> Log = Logger;
		return Execute<TValue>(MoveTemp(Operation),
			[OnComplete, Store, Scope, Key, Serialize, Deserialize, Cached, bHasCache, Context, Log](TFlockResult<TValue> Result)
			{
				if (Result.bSuccess)
				{
					FString Payload;
					if (Serialize(Result.Value, Payload))
					{
						Store->Write(Scope, Key, Payload);
					}
					if (OnComplete)
					{
						OnComplete(Result);
					}
					return;
				}

				// A non-permanent failure with a cache to fall back on serves the cache; a permanent 4xx
				// (an authoritative answer, e.g. the config was deleted) propagates regardless.
				if (bHasCache && !FFlockError::IsPermanentStatus(Result.Error.StatusCode))
				{
					TValue Value;
					if (Deserialize(Cached, Value))
					{
						Log->LogWarning(FString::Printf(TEXT("%s: serving cached snapshot (couldn't reach server)"), *Context));
						if (OnComplete)
						{
							OnComplete(TFlockResult<TValue>::Ok(Value));
						}
						return;
					}
				}
				if (OnComplete)
				{
					OnComplete(Result);
				}
			},
			Context, /*bIdempotent*/ true, RetryBudget);
	}

	TSharedPtr<FFlockAuthSession> AuthSession;
	TSharedPtr<FFlockSnapshotStore> SnapshotStore;
	FString GameVersionId;
	TFunction<bool()> ReachabilityProbe;
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
