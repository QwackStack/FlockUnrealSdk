// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Containers/Ticker.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockMultiplayerModels.h"
#include "Multiplayer/FlockMatchmakingQueueNames.h"
#include "Multiplayer/FlockMultiplayerSession.h"
#include "Multiplayer/FlockParty.h"
#include "Multiplayer/FlockRepeatingCalls.h"

class FFlockMultiplayerSessions;
class FFlockParties;
class FFlockPartyRequests;

/**
 * The entry point for multiplayer: the player's party and session, and the lookup of matchmaking queues by name. Owns every
 * call the SDK repeats for them, sent from the core ticker. Built at Flock's initialization and torn down at its shutdown;
 * make its calls from the game thread.
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

	/**
	 * Hosts a session with the signed-in player as its host and only player. Leave MaxPlayers unset for the game's own player
	 * count from the dashboard (8 unless changed there); it takes 2 to 16. Data is the game's own data on the session, such as a map, which its players read; empty sends none.
	 * With bSameVersionOnly (the server's default) only players on the host's Game Version can join. A player holds one seat
	 * per game, so a session held until now ends as moved_to_another_session.
	 */
	FFlockRequestHandle HostSession(TOptional<int32> MaxPlayers, const FFlockCommandData& Data, bool bSameVersionOnly,
		FFlockOnMultiplayerSession OnComplete);

	/** Takes a seat in the session with this join code; letter case does not matter. Joining the session the player is seated in returns it. */
	FFlockRequestHandle JoinSession(const FString& JoinCode, FFlockOnMultiplayerSession OnComplete);

	/**
	 * A session the player holds or once held a seat in, read now. The held session comes back as the same object; one the
	 * player is no longer seated in comes back ended and is not held.
	 */
	FFlockRequestHandle GetSession(const FString& SessionId, FFlockOnMultiplayerSession OnComplete);

	/**
	 * The session the signed-in player is seated in, found without its id, or null when seated nowhere: after a restart, or as
	 * a party member whose leader made the session. Returns the same object each time while it lasts.
	 */
	FFlockRequestHandle GetMySession(FFlockOnMultiplayerSession OnComplete);

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

	/** The owner of the player's session; for tests. */
	FFlockMultiplayerSessions& GetSessionsForTesting() { return *Sessions; }

	/** Runs one frame's work (a party or session of an ended sign-in, then the calls due); for tests that drive frames themselves. */
	void TickForTesting() { Tick(0.f); }

private:
	friend class UFlockSubsystem;

	/**
	 * The game is ending: gives the held session's seat up without waiting for the answer, so the other players see it at once
	 * rather than when the server stops hearing from the player. The session ends as left, raising nothing. True when a leave
	 * was sent. Flock calls it when its game instance shuts down, before it waits for requests on their way; a game leaves with
	 * the session's own Leave.
	 */
	bool LeaveSessionForQuit();

	bool Tick(float DeltaSeconds);
	FFlockRequestHandle ReadQueues(TFunction<void(TFlockResult<TArray<FFlockMatchmakingQueue>>)> OnComplete);

	TSharedRef<FFlockAuthSession> Session;
	FString VersionedApiUrl;
	TSharedRef<FFlockRepeatingCalls> RepeatingCalls;
	TSharedPtr<FFlockMatchmakingQueueNames> QueueNames;
	TSharedRef<FFlockParties> Parties;
	TSharedRef<FFlockMultiplayerSessions> Sessions;
	FTSTicker::FDelegateHandle TickerHandle;
};
