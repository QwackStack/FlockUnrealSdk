// Copyright 2022, Qwacks. All Rights Reserved.

#include "FlockLogger.h"
#include "Flock.h"

namespace
{
	/** Defaults to C++ so a call made before any Blueprint scope is still attributed, not left blank. */
	FString GCallOrigin = TEXT("C++");
}

const FString& FFlockCallOrigin::Get()
{
	return GCallOrigin;
}

void FFlockCallOrigin::Set(const FString& Origin)
{
	GCallOrigin = Origin;
}

void FFlockUnrealLogger::LogDebug(const FString& Message)
{
	if (bVerbose)
	{
		UE_LOG(LogFlock, Verbose, TEXT("[Flock SDK] %s"), *Message);
	}
}

void FFlockUnrealLogger::LogInfo(const FString& Message)
{
	if (bVerbose)
	{
		UE_LOG(LogFlock, Log, TEXT("[Flock SDK] %s"), *Message);
	}
}

void FFlockUnrealLogger::LogWarning(const FString& Message)
{
	UE_LOG(LogFlock, Warning, TEXT("[Flock SDK] %s"), *Message);
}

void FFlockUnrealLogger::LogError(const FString& Message)
{
	UE_LOG(LogFlock, Error, TEXT("[Flock SDK] %s"), *Message);
}
