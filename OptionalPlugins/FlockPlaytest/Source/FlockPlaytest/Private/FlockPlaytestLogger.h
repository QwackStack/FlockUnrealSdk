// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockLogger.h"
#include "FlockPlaytestLog.h"

/**
 * Sends the Flock SDK's per-request messages for playtest calls to LogFlockPlaytest at Verbose. The playtest
 * subsystem reports each outcome once, at the level its meaning deserves, so the request trace underneath it
 * stays out of the way unless someone raises the category.
 */
class FFlockPlaytestLogger : public IFlockLogger
{
public:
	virtual void LogDebug(const FString& Message) override
	{
		UE_LOG(LogFlockPlaytest, Verbose, TEXT("%s"), *Message);
	}

	virtual void LogInfo(const FString& Message) override
	{
		UE_LOG(LogFlockPlaytest, Verbose, TEXT("%s"), *Message);
	}

	virtual void LogWarning(const FString& Message) override
	{
		UE_LOG(LogFlockPlaytest, Verbose, TEXT("%s"), *Message);
	}

	virtual void LogError(const FString& Message) override
	{
		UE_LOG(LogFlockPlaytest, Verbose, TEXT("%s"), *Message);
	}
};
