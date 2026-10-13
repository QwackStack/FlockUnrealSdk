// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Containers/Ticker.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockMultiplayerModels.h"
#include "Multiplayer/FlockMatchmakingQueueNames.h"
#include "Multiplayer/FlockRepeatingCalls.h"

/**
 * The entry point for multiplayer. Owns every call the SDK repeats for it, sent from the core ticker, and the lookup of
 * matchmaking queues by name. Built at Flock's initialization and torn down at its shutdown.
 */
class FLOCK_API FFlockMultiplayerProvider
	: public FFlockProviderBase
	, public TSharedFromThis<FFlockMultiplayerProvider>
{
public:
	FFlockMultiplayerProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
		const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession, const FString& InVersionedApiUrl);
	virtual ~FFlockMultiplayerProvider() override;

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
	FFlockRepeatingCalls& GetRepeatingCalls() { return RepeatingCalls; }

private:
	bool Tick(float DeltaSeconds);
	FFlockRequestHandle ReadQueues(TFunction<void(TFlockResult<TArray<FFlockMatchmakingQueue>>)> OnComplete);

	TSharedRef<FFlockAuthSession> Session;
	FString VersionedApiUrl;
	FFlockRepeatingCalls RepeatingCalls;
	TSharedPtr<FFlockMatchmakingQueueNames> QueueNames;
	FTSTicker::FDelegateHandle TickerHandle;
};
