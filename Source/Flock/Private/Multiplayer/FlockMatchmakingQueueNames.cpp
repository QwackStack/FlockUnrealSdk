// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockMatchmakingQueueNames.h"

namespace
{
	const TCHAR* const QueueNotFoundCode = TEXT("matchmaking.queue_not_found");
}

FFlockMatchmakingQueueNames::FFlockMatchmakingQueueNames(FReadQueues InReadQueues, const TSharedRef<IFlockLogger>& InLogger)
	: ReadQueues(MoveTemp(InReadQueues))
	, Logger(InLogger)
{
}

FFlockRequestHandle FFlockMatchmakingQueueNames::FindId(const FString& QueueName, TFunction<void(TFlockResult<FString>)> OnComplete)
{
	if (Queues.IsSet())
	{
		const FString Id = FindIn(Queues.GetValue(), QueueName);
		if (!Id.IsEmpty())
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<FString>::Ok(Id));
			}
			return FFlockRequestHandle();
		}
	}

	// Not read yet, or not in the list kept: wait for a read, shared with every caller asking now.
	FWaiter Waiter;
	Waiter.Number = ++NextWaiterNumber;
	Waiter.QueueName = QueueName;
	Waiter.OnComplete = MoveTemp(OnComplete);
	const int32 WaiterNumber = Waiter.Number;
	Waiters.Add(MoveTemp(Waiter));
	StartSharedRead();

	const TSharedPtr<FFlockCancelToken> Token = MakeShared<FFlockCancelToken>();
	const TWeakPtr<FFlockMatchmakingQueueNames> WeakSelf = AsShared();
	Token->Canceller = [WeakSelf, WaiterNumber]()
	{
		if (const TSharedPtr<FFlockMatchmakingQueueNames> Self = WeakSelf.Pin())
		{
			Self->GiveUp(WaiterNumber);
		}
	};
	return FFlockRequestHandle(Token);
}

void FFlockMatchmakingQueueNames::StartSharedRead()
{
	if (bReading)
	{
		return;
	}
	bReading = true;
	++ReadCount;
	// No caller's handle reaches this read: a caller that gives up stops waiting, and the read keeps what it reads.
	const TWeakPtr<FFlockMatchmakingQueueNames> WeakSelf = AsShared();
	ReadQueues([WeakSelf](TFlockResult<TArray<FFlockMatchmakingQueue>> Result)
	{
		if (const TSharedPtr<FFlockMatchmakingQueueNames> Self = WeakSelf.Pin())
		{
			Self->FinishRead(Result);
		}
	});
}

void FFlockMatchmakingQueueNames::FinishRead(const TFlockResult<TArray<FFlockMatchmakingQueue>>& Result)
{
	bReading = false;
	if (Result.bSuccess)
	{
		Queues = Result.Value;
	}
	// A failed read is not kept, so the next lookup reads again.

	TArray<FWaiter> Answering = MoveTemp(Waiters);
	Waiters.Reset();
	for (FWaiter& Waiter : Answering)
	{
		if (!Waiter.OnComplete)
		{
			continue;
		}
		if (!Result.bSuccess)
		{
			Waiter.OnComplete(TFlockResult<FString>::Fail(Result.Error));
			continue;
		}
		const FString Id = FindIn(Result.Value, Waiter.QueueName);
		Waiter.OnComplete(Id.IsEmpty()
			? TFlockResult<FString>::Fail(NotFound(Waiter.QueueName, Result.Value))
			: TFlockResult<FString>::Ok(Id));
	}
}

void FFlockMatchmakingQueueNames::GiveUp(int32 WaiterNumber)
{
	const int32 Index = Waiters.IndexOfByPredicate([WaiterNumber](const FWaiter& Waiter) { return Waiter.Number == WaiterNumber; });
	if (Index == INDEX_NONE)
	{
		return;
	}
	TFunction<void(TFlockResult<FString>)> OnComplete = MoveTemp(Waiters[Index].OnComplete);
	Waiters.RemoveAt(Index);
	if (OnComplete)
	{
		OnComplete(TFlockResult<FString>::Fail(FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Request cancelled"))));
	}
}

void FFlockMatchmakingQueueNames::GiveUpEveryWait()
{
	// A copy, since a caller's handler may ask again from inside its answer.
	TArray<FWaiter> GivingUp = MoveTemp(Waiters);
	Waiters.Reset();
	for (FWaiter& Waiter : GivingUp)
	{
		if (Waiter.OnComplete)
		{
			Waiter.OnComplete(TFlockResult<FString>::Fail(FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Flock shut down"))));
		}
	}
}

FString FFlockMatchmakingQueueNames::FindIn(const TArray<FFlockMatchmakingQueue>& InQueues, const FString& QueueName)
{
	// The list is oldest first, so the first match is the oldest queue of that name.
	const FFlockMatchmakingQueue* Found = nullptr;
	for (const FFlockMatchmakingQueue& Queue : InQueues)
	{
		if (!Queue.Name.Equals(QueueName, ESearchCase::CaseSensitive))
		{
			continue;
		}
		if (Found == nullptr)
		{
			Found = &Queue;
			continue;
		}
		const bool bWarnedBefore = RepeatedNamesWarnedAbout.ContainsByPredicate(
			[&QueueName](const FString& Warned) { return Warned.Equals(QueueName, ESearchCase::CaseSensitive); });
		if (!bWarnedBefore)
		{
			RepeatedNamesWarnedAbout.Add(QueueName);
			Logger->LogWarning(FString::Printf(TEXT("Two matchmaking queues in this game are named \"%s\" (%s and %s). Using the ")
				TEXT("older one, %s. Give each queue its own name in the Flock dashboard."), *QueueName, *Found->Id, *Queue.Id, *Found->Id));
		}
		break;
	}
	return Found != nullptr ? Found->Id : FString();
}

FFlockError FFlockMatchmakingQueueNames::NotFound(const FString& QueueName, const TArray<FFlockMatchmakingQueue>& InQueues)
{
	const FFlockMatchmakingQueue* OtherCase = InQueues.FindByPredicate(
		[&QueueName](const FFlockMatchmakingQueue& Queue) { return Queue.Name.Equals(QueueName, ESearchCase::IgnoreCase); });

	FString Hint;
	if (OtherCase != nullptr)
	{
		Hint = FString::Printf(TEXT("This game has a queue named \"%s\". Queue names match letter case exactly."), *OtherCase->Name);
	}
	else if (InQueues.Num() >= MostQueuesListed)
	{
		Hint = FString::Printf(TEXT("The game's queue list returns only its %d oldest queues, so a newer queue may be missing from it. ")
			TEXT("Delete queues the game no longer uses in the Flock dashboard."), MostQueuesListed);
	}
	else
	{
		Hint = TEXT("Create a queue with this name in the Flock dashboard, or check the name against the game's queues there. ")
			TEXT("Names match exactly, letter case included.");
	}

	FFlockError Error = FFlockError::Make(EFlockErrorType::Validation,
		FString::Printf(TEXT("No matchmaking queue in this game is named \"%s\""), *QueueName), 0, FString(), QueueNotFoundCode);
	Error.Hint = Hint;
	Error.Operation = TEXT("Find matchmaking queue");
	return Error;
}
