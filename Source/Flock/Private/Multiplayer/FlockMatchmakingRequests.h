// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockMultiplayerModels.h"

/**
 * The matchmaking ticket routes on the wire, each sent as the sign-in it was asked for or not at all: every attempt, retries
 * included, fails Cancelled without sending once that sign-in has ended. Every route is enveloped; a search is read and
 * cancelled by its own player only.
 */
class FFlockMatchmakingRequests : public FFlockProviderBase
{
public:
	using FOnTicket = TFunction<void(TFlockResult<FFlockMatchmakingTicketReading>)>;
	using FOnTicketsMade = TFunction<void(TFlockResult<FFlockMatchmakingTicketsMade>)>;

	FFlockMatchmakingRequests(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
		const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession, const FString& InVersionedApiUrl);

	/**
	 * Makes the search: one ticket per player in it (the party's members, or the player alone). An empty PartyId searches alone;
	 * empty Attributes send none. Retried: a retry after a lost answer is refused as already queued, which the owner answers as
	 * a search left behind, so that refusal is not logged as a failure.
	 */
	FFlockRequestHandle Create(int32 SignInNumber, const FString& QueueId, const FString& PartyId, const FFlockCommandData& Attributes,
		FOnTicketsMade OnComplete);

	/** The ticket, with its match once there is one. */
	FFlockRequestHandle Read(int32 SignInNumber, const FString& TicketId, FOnTicket OnComplete);

	/**
	 * The player's search without its id: the one queued, else the latest, with its match once there is one. Refused
	 * ticket_not_found for a player who never searched, which the owner reads as an answer, so it is not logged as a failure.
	 * With bTryOnce, a failure is not tried again: a reading its caller repeats anyway, or one a party call waits for.
	 */
	FFlockRequestHandle ReadCurrent(int32 SignInNumber, FOnTicket OnComplete, bool bTryOnce = false);

	/** The game's queues, which need only the API key: the list is the game's, so a 401 is a refused key, never a lapsed sign-in. */
	FFlockRequestHandle ReadQueues(TFunction<void(TFlockResult<TArray<FFlockMatchmakingQueue>>)> OnComplete);

	/**
	 * Cancels the search; any member's cancel stops the whole party's. Retried: a retry after a lost answer is refused as not
	 * cancelable, which the owner answers by reading how the search ended, so that refusal is not logged as a failure.
	 */
	FFlockRequestHandle Cancel(int32 SignInNumber, const FString& TicketId, FOnTicket OnComplete);

private:
	FString Url(const FString& Path) const { return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Path); }

	/** Sends one enveloped matchmaking request as the given sign-in, under the retry policy. */
	template <typename T>
	FFlockRequestHandle SendAs(int32 SignInNumber, const FString& Context,
		TFunction<FFlockRequestHandle(const TMap<FString, FString>& Headers, TFunction<void(TFlockResult<T>)> OnResult)> Send,
		TFunction<void(TFlockResult<T>)> OnComplete, TFunction<bool(const FFlockError&)> IsExpectedFailure = nullptr,
		int32 MaxRetriesOverride = -1);

	static FString ToJson(const TSharedRef<FJsonObject>& Object);

	TSharedRef<FFlockAuthSession> Session;
	FString VersionedApiUrl;
};
