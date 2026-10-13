// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockLogger.h"
#include "Http/FlockError.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockResult.h"
#include "Models/FlockMultiplayerModels.h"

/**
 * Finds a matchmaking queue's id by its name. The game's queue list is read once a launch, shared by every caller asking at
 * once, and read again only when a name is not in it. Game-thread only.
 */
class FLOCK_API FFlockMatchmakingQueueNames : public TSharedFromThis<FFlockMatchmakingQueueNames>
{
public:
	/** The most queues one read of the list returns; the backend has no further pages. */
	static constexpr int32 MostQueuesListed = 200;

	using FReadQueues = TFunction<FFlockRequestHandle(TFunction<void(TFlockResult<TArray<FFlockMatchmakingQueue>>)>)>;

	FFlockMatchmakingQueueNames(FReadQueues InReadQueues, const TSharedRef<IFlockLogger>& InLogger);

	/**
	 * The id of the queue named exactly QueueName. Fails Validation coded matchmaking.queue_not_found, with a hint for
	 * that name, when the game has none. Cancelling the returned handle ends this caller's wait at once (Cancelled); the
	 * read goes on for whoever else waits and keeps what it reads.
	 */
	FFlockRequestHandle FindId(const FString& QueueName, TFunction<void(TFlockResult<FString>)> OnComplete);

	/** Flock is shutting down: every caller still waiting for a read is answered Cancelled at once. */
	void GiveUpEveryWait();

	/** How many times the list has been read; for tests. */
	int32 GetReadCountForTesting() const { return ReadCount; }

private:
	struct FWaiter
	{
		int32 Number = 0;
		FString QueueName;
		TFunction<void(TFlockResult<FString>)> OnComplete;
	};

	void StartSharedRead();
	void FinishRead(const TFlockResult<TArray<FFlockMatchmakingQueue>>& Result);
	void GiveUp(int32 WaiterNumber);
	/** The oldest queue of that name, warning once a launch when the game has two. */
	FString FindIn(const TArray<FFlockMatchmakingQueue>& Queues, const FString& QueueName);
	static FFlockError NotFound(const FString& QueueName, const TArray<FFlockMatchmakingQueue>& Queues);

	FReadQueues ReadQueues;
	TSharedRef<IFlockLogger> Logger;
	TOptional<TArray<FFlockMatchmakingQueue>> Queues;
	TArray<FWaiter> Waiters;
	/** Compared letter for letter, so "Duo" and "duo" are warned about apart (a TSet<FString> would merge them). */
	TArray<FString> RepeatedNamesWarnedAbout;
	bool bReading = false;
	int32 NextWaiterNumber = 0;
	int32 ReadCount = 0;
};
