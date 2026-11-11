// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockLogger.h"
#include "Misc/ScopeLock.h"

/**
 * True on this thread while the SDK hands one of its own lines to the game's logger. The exception capture
 * (FFlockLogSink) skips what the engine log receives meanwhile: a game logger that writes an SDK error under its own
 * category would otherwise have it captured and sent as an exception, whose failed send logs another error, and so on.
 */
inline thread_local bool GFlockPassingOnAnSdkLine = false;

/**
 * The one logger every part of the SDK is handed: it passes each line on to the logger in use now. The parts keep this
 * object from their creation, so a logger the game sets later (UFlockSubsystem::SetLogger) reaches all of them, the
 * calls' lines and the network trace included, without anyone rebuilding them.
 *
 * Lines can come from any thread (a file upload, a download); the logger in use is read under a lock and called outside it.
 */
class FFlockPassOnLogger final : public IFlockLogger
{
public:
	explicit FFlockPassOnLogger(const TSharedRef<IFlockLogger>& InInUse)
		: InUse(InInUse)
	{
	}

	/** Every line from now on goes to Logger. The one it replaces is let go outside the lock, in case its end waits on a thread that logs. */
	void PassOnTo(const TSharedRef<IFlockLogger>& Logger)
	{
		TSharedRef<IFlockLogger> Replaced = Logger;
		{
			FScopeLock Lock(&Guard);
			Swap(InUse, Replaced);
		}
	}

	virtual void LogDebug(const FString& Message) override { FPassingOn Mark; Current()->LogDebug(Message); }
	virtual void LogInfo(const FString& Message) override { FPassingOn Mark; Current()->LogInfo(Message); }
	virtual void LogWarning(const FString& Message) override { FPassingOn Mark; Current()->LogWarning(Message); }
	virtual void LogError(const FString& Message) override { FPassingOn Mark; Current()->LogError(Message); }

private:
	/** Marks this thread for the length of one line; a line logged from inside another keeps the mark. */
	struct FPassingOn
	{
		bool bWas = GFlockPassingOnAnSdkLine;
		FPassingOn() { GFlockPassingOnAnSdkLine = true; }
		~FPassingOn() { GFlockPassingOnAnSdkLine = bWas; }
	};

	TSharedRef<IFlockLogger> Current()
	{
		FScopeLock Lock(&Guard);
		return InUse;
	}

	FCriticalSection Guard;
	TSharedRef<IFlockLogger> InUse;
};
