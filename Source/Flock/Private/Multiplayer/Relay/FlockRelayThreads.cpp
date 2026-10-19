// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockRelayThreads.h"
#include "Containers/Ticker.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"

namespace
{
	class FFlockRelayThreadList
	{
	public:
		static FFlockRelayThreadList& Get()
		{
			static FFlockRelayThreadList Threads;
			return Threads;
		}

		bool Start(const TSharedRef<IFlockRelayThreadWork, ESPMode::ThreadSafe>& Work, const TCHAR* Name, uint32 StackBytes)
		{
			// Made and listed under the lock, so a thread asked for while every thread is being ended (a relay's thread meeting a new
			// peer as Flock's module goes away) is refused rather than left running past it.
			FScopeLock Locked(&Lock);
			if (bEndingAll)
			{
				return false;
			}
			FRunnableThread* Thread = FRunnableThread::Create(&Work.Get(), Name, StackBytes, TPri_Normal);
			if (Thread == nullptr)
			{
				return false;
			}
			Running.Add({ Thread, Work });
			if (!CleanUpHandle.IsValid())
			{
				// Ticks while any thread is listed: it takes itself off once the last one is cleaned up.
				CleanUpHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
				{
					return FFlockRelayThreadList::Get().CleanUpEnded(/*bFromTheTicker*/ true);
				}), 1.0f);
			}
			return true;
		}

		/**
		 * Deletes the threads that ended; deleting one waits for it, which is at once. From the ticker, true while threads are left
		 * to clean up, and the ticker is let go when none is.
		 */
		bool CleanUpEnded(bool bFromTheTicker)
		{
			TArray<FEntry> Ended;
			bool bKeepTicking = true;
			{
				FScopeLock Locked(&Lock);
				for (int32 Index = Running.Num() - 1; Index >= 0; --Index)
				{
					if (Running[Index].Work->HasEnded())
					{
						Ended.Add(Running[Index]);
						Running.RemoveAt(Index);
					}
				}
				if (bFromTheTicker && Running.Num() == 0)
				{
					CleanUpHandle.Reset();
					bKeepTicking = false;
				}
			}
			for (FEntry& Entry : Ended)
			{
				delete Entry.Thread;
			}
			return bKeepTicking;
		}

		void EndAll()
		{
			TArray<FEntry> All;
			{
				FScopeLock Locked(&Lock);
				bEndingAll = true;
				All = MoveTemp(Running);
				Running.Reset();
				if (CleanUpHandle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(CleanUpHandle);
					CleanUpHandle.Reset();
				}
			}
			// Every thread is told first, so they end side by side rather than one wait after another.
			for (FEntry& Entry : All)
			{
				Entry.Work->EndNow();
			}
			for (FEntry& Entry : All)
			{
				// Waits for the thread, which sees the end within one wait.
				delete Entry.Thread;
			}
			FScopeLock Locked(&Lock);
			bEndingAll = false;
		}

		int32 Count()
		{
			CleanUpEnded(/*bFromTheTicker*/ false);
			FScopeLock Locked(&Lock);
			return Running.Num();
		}

		bool IsTickingForTesting()
		{
			FScopeLock Locked(&Lock);
			return CleanUpHandle.IsValid();
		}

	private:
		struct FEntry
		{
			FRunnableThread* Thread = nullptr;
			TSharedPtr<IFlockRelayThreadWork, ESPMode::ThreadSafe> Work;
		};

		FCriticalSection Lock;
		TArray<FEntry> Running;
		FTSTicker::FDelegateHandle CleanUpHandle;
		/** Every thread is being ended (Flock's module is going away): none starts meanwhile. */
		bool bEndingAll = false;
	};
}

bool FFlockRelayThreads::bRefusedForTesting = false;

bool FFlockRelayThreads::Start(const TSharedRef<IFlockRelayThreadWork, ESPMode::ThreadSafe>& Work, const TCHAR* Name, uint32 StackBytes)
{
	return !bRefusedForTesting && FFlockRelayThreadList::Get().Start(Work, Name, StackBytes);
}

void FFlockRelayThreads::EndEveryThread()
{
	FFlockRelayThreadList::Get().EndAll();
}

int32 FFlockRelayThreads::CountForTesting()
{
	return FFlockRelayThreadList::Get().Count();
}

bool FFlockRelayThreads::IsCleaningUpForTesting()
{
	return FFlockRelayThreadList::Get().IsTickingForTesting();
}
