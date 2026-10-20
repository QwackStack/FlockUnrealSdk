// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockRepeatingCalls.h"
#include "HAL/PlatformTime.h"

FFlockRepeatingCalls::FFlockRepeatingCalls(TFunction<int32()> InCurrentSignInNumber, const TSharedRef<IFlockLogger>& InLogger,
	TFunction<double()> InNextRoll, TFunction<double()> InNow)
	: Shared(MakeShared<FFlockRepeatingCallsShared>())
{
	Shared->CurrentSignInNumber = InCurrentSignInNumber ? MoveTemp(InCurrentSignInNumber) : TFunction<int32()>([]() { return 0; });
	Shared->Now = InNow ? MoveTemp(InNow) : TFunction<double()>([]() { return FPlatformTime::Seconds(); });
	Shared->NextRoll = InNextRoll ? MoveTemp(InNextRoll) : TFunction<double()>([]() { return static_cast<double>(FMath::FRand()); });
	Shared->Logger = InLogger;
}

bool FFlockRepeatingCalls::CanStart(const FString& Name, double IntervalSeconds, bool bHasSend) const
{
	if (Name.IsEmpty() || !(IntervalSeconds > 0.0) || !bHasSend)
	{
		Shared->Logger->LogError(FString::Printf(TEXT("A repeating call needs a name, an interval above zero and a request to send (got '%s', %.3f s)"),
			*Name, IntervalSeconds));
		return false;
	}
	for (const TSharedRef<FFlockRepeatingCall>& Running : Calls)
	{
		if (Running->GetName().Equals(Name, ESearchCase::CaseSensitive) && Running->IsRunning())
		{
			Shared->Logger->LogError(FString::Printf(TEXT("The repeating call '%s' is already running"), *Name));
			return false;
		}
	}
	return true;
}

void FFlockRepeatingCalls::SendThoseDue()
{
	if (Calls.Num() == 0)
	{
		return;
	}

	const double Now = Shared->Now();
	for (const TSharedRef<FFlockRepeatingCall>& Call : Calls)
	{
		if (!Call->bWaitingForAnswer && Now >= Call->DueAt)
		{
			Due.Add(Call);
		}
	}

	// Decided as each one goes, so a call stopped (or a sign-out) by a handler of one sent just before it sends nothing.
	for (const TSharedRef<FFlockRepeatingCall>& Call : Due)
	{
		if (!Call->IsRunning())
		{
			Calls.Remove(Call);
			continue;
		}
		Call->SendOnce();
	}
	Due.Reset();
}

void FFlockRepeatingCalls::StopAll()
{
	// A copy, since a cancelled request's handler may run inside Stop().
	const TArray<TSharedRef<FFlockRepeatingCall>> Stopping = MoveTemp(Calls);
	Calls.Reset();
	for (const TSharedRef<FFlockRepeatingCall>& Call : Stopping)
	{
		Call->Stop();
	}
}

bool FFlockRepeatingCalls::IsWorthAnotherTry(const FFlockError& Failure)
{
	switch (Failure.Type)
	{
	case EFlockErrorType::Connection:
	case EFlockErrorType::Timeout:
	case EFlockErrorType::Serialization:
		return true;
	case EFlockErrorType::Network:
		return !FFlockError::IsPermanentStatus(Failure.StatusCode);
	default:
		return false;
	}
}
