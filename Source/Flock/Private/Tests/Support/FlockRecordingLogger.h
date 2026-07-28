// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockLogger.h"

#if WITH_AUTOMATION_TESTS

/**
 * Captures log lines per level so a test can assert not just what the SDK did, but what it *said*
 * about it. Needed because "this outcome must not be reported as an error" is a real requirement —
 * an expected result logged as an error trains people to ignore the log.
 */
class FFlockRecordingLogger : public IFlockLogger
{
public:
	TArray<FString> Debugs;
	TArray<FString> Infos;
	TArray<FString> Warnings;
	TArray<FString> Errors;

	virtual void LogDebug(const FString& Message) override { Debugs.Add(Message); }
	virtual void LogInfo(const FString& Message) override { Infos.Add(Message); }
	virtual void LogWarning(const FString& Message) override { Warnings.Add(Message); }
	virtual void LogError(const FString& Message) override { Errors.Add(Message); }

	static bool AnyContains(const TArray<FString>& Lines, const FString& Needle)
	{
		for (const FString& Line : Lines)
		{
			if (Line.Contains(Needle))
			{
				return true;
			}
		}
		return false;
	}
};

#endif // WITH_AUTOMATION_TESTS
