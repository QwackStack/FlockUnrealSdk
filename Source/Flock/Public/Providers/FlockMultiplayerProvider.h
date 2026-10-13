// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Containers/Ticker.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockMultiplayerModels.h"
#include "Multiplayer/FlockMatchmakingQueueNames.h"
#include "Multiplayer/FlockParty.h"
#include "Multiplayer/FlockRepeatingCalls.h"

class FFlockParties;
class FFlockPartyRequests;

/**
 * The entry point for multiplayer: the player's party, and the lookup of matchmaking queues by name. Owns every call the
 * SDK repeats for it, sent from the core ticker. Built at Flock's initialization and torn down at its shutdown; make its
 * calls from the game thread.
 */
class FLOCK_API FFlockMultiplayerProvider
	: public FFlockProviderBase
	, public TSharedFromThis<FFlockMultiplayerProvider>
{
public:
	/** @param InPartyRefreshSeconds How often a held party is read again (Party Refresh Seconds); 0 or less turns that off. */
	FFlockMultiplayerProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
		const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession, const FString& InVersionedApiUrl,
		double InPartyRefreshSeconds);
	virtual ~FFlockMultiplayerProvider() override;

	/**
	 * Makes a party with the signed-in player as its leader and only member. Leave MaxSize unset for the server's default (4);
	 * it takes 2 to 64 players. Settings are the game's own data on the party, such as a mode, which its players read; empty
	 * sends none. A player can be in one party at a time.
	 */
	FFlockRequestHandle CreateParty(TOptional<int32> MaxSize, const FFlockCommandData& Settings, FFlockOnParty OnComplete);

	/** Joins the party with this invite code; letter case does not matter. Joining the party the player is already in returns it. */
	FFlockRequestHandle JoinParty(const FString& InviteCode, FFlockOnParty OnComplete);

	/**
	 * The signed-in player's party, read now, or null when the player is in none. A party lasts until the player leaves it,
	 * across launches, so call this after signing in. Returns the same object each time while it lasts.
	 */
	FFlockRequestHandle GetMyParty(FFlockOnParty OnComplete);

	/** Starts sending repeating calls from the core ticker. Called once, right after construction. */
	void Start();

	/**
	 * Stops every repeating call, cancels their requests, answers every queue lookup still waiting (Cancelled) and leaves
	 * the core ticker; runs at Flock's shutdown.
	 */
	void Shutdown();

	/**
	 * The id of the matchmaking queue named exactly QueueName (letter case included), found without starting a search.
	 * A missing name fails Validation coded matchmaking.queue_not_found. Cancelling the handle ends this wait at once.
	 */
	FFlockRequestHandle FindQueueId(const FString& QueueName, TFunction<void(TFlockResult<FString>)> OnComplete);

	/** The calls the SDK repeats for multiplayer; the SDK's own features start them, a game does not. */
	FFlockRepeatingCalls& GetRepeatingCalls() { return *RepeatingCalls; }

	/** The owner of the player's party; for tests. */
	FFlockParties& GetPartiesForTesting() { return *Parties; }

	/** Runs one frame's work (parties of an ended sign-in, then the calls due); for tests that drive frames themselves. */
	void TickForTesting() { Tick(0.f); }

private:
	bool Tick(float DeltaSeconds);
	FFlockRequestHandle ReadQueues(TFunction<void(TFlockResult<TArray<FFlockMatchmakingQueue>>)> OnComplete);

	TSharedRef<FFlockAuthSession> Session;
	FString VersionedApiUrl;
	TSharedRef<FFlockRepeatingCalls> RepeatingCalls;
	TSharedPtr<FFlockMatchmakingQueueNames> QueueNames;
	TSharedRef<FFlockParties> Parties;
	FTSTicker::FDelegateHandle TickerHandle;
};
