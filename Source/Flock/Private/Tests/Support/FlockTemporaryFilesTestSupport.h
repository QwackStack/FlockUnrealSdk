// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"

/**
 * How long a save that gives up at once may take in a test. A move that retries takes five seconds; one that gives up at
 * once finishes well inside this even when the disk holds a single write for a second or two, which Windows Defender does
 * on this machine.
 */
constexpr double FlockTestSecondsWithoutARetry = 2.5;

/** Moves a file's time back, ten minutes unless told otherwise, so a sweep takes it for a crash's leftover rather than a write in progress. */
inline void FlockMoveTestFileTimeBack(const FString& Path, double Minutes = 10.0)
{
	IFileManager::Get().SetTimeStamp(*Path, FDateTime::UtcNow() - FTimespan::FromMinutes(Minutes));
}

/**
 * Counts the warnings and errors the engine's file manager logs while it lives. A move that fails without giving up at
 * once retries for seconds and logs an Error, which Flock's exception capture reports, so a test that expects a quiet
 * failure counts these as well as timing it.
 */
class FFlockFileManagerComplaintCounter : public FOutputDevice
{
public:
	FFlockFileManagerComplaintCounter()
	{
		GLog->AddOutputDevice(this);
	}

	virtual ~FFlockFileManagerComplaintCounter() override
	{
		GLog->RemoveOutputDevice(this);
	}

	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		if (Category == FileManagerCategory && (Verbosity & ELogVerbosity::VerbosityMask) <= ELogVerbosity::Warning)
		{
			Complaints.Increment();
		}
	}

	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	int32 Count()
	{
		GLog->Flush();
		return Complaints.GetValue();
	}

private:
	const FName FileManagerCategory = TEXT("LogFileManager");
	FThreadSafeCounter Complaints;
};

#endif // WITH_AUTOMATION_TESTS
