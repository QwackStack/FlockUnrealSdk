// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockLogger.h"
#include "Http/FlockError.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockResult.h"
#include "Http/FlockRetryHandler.h"

/** What every repeating call shares with its owner: the current sign-in, the clock, the jitter and the log. */
struct FFlockRepeatingCallsShared
{
	TFunction<int32()> CurrentSignInNumber;
	TFunction<double()> Now;
	TFunction<double()> NextRoll;
	TSharedPtr<IFlockLogger> Logger;
};

/** One call the SDK repeats. Stop() ends it, and nothing it was waiting for is used afterwards. */
class FLOCK_API FFlockRepeatingCall
{
public:
	FFlockRepeatingCall(const TSharedRef<FFlockRepeatingCallsShared>& InShared, const FString& InName, int32 InSignInNumber,
		double InIntervalSeconds)
		: Shared(InShared)
		, Name(InName)
		, SignInNumber(InSignInNumber)
		, IntervalSeconds(InIntervalSeconds)
	{
	}

	const FString& GetName() const { return Name; }
	int32 GetSignInNumber() const { return SignInNumber; }
	double GetIntervalSeconds() const { return IntervalSeconds; }
	bool IsWaitingForAnswer() const { return bWaitingForAnswer; }

	/** False once stopped, or once the sign-in it started under has ended. */
	bool IsRunning() const { return !bStopped && SignInNumber == Shared->CurrentSignInNumber(); }

	/** Stops it and cancels the request on its way, if any. */
	void Stop()
	{
		if (bStopped)
		{
			return;
		}
		bStopped = true;
		InFlight.Cancel();
	}

private:
	friend class FFlockRepeatingCalls;

	TSharedRef<FFlockRepeatingCallsShared> Shared;
	FString Name;
	int32 SignInNumber = 0;
	double IntervalSeconds = 0.0;
	double DueAt = 0.0;
	bool bWaitingForAnswer = false;
	bool bStopped = false;
	FFlockRequestHandle InFlight;
	TFunction<void()> SendOnce;
};

/**
 * Every multiplayer call the SDK repeats (search checks, heartbeats, party refreshes): one request at a time each, at a
 * jittered interval, for the sign-in it started under. SendThoseDue() runs once a frame from the core ticker, which keeps
 * ticking while a desktop game is minimized or covered. Game-thread only.
 */
class FLOCK_API FFlockRepeatingCalls
{
public:
	/**
	 * @param InCurrentSignInNumber The sign-in now current; a call stops being sent once it moves.
	 * @param InNextRoll A number from 0 to 1 for each wait's jitter; null draws a random one.
	 * @param InNow Seconds on a steady clock; null reads the platform's.
	 */
	FFlockRepeatingCalls(TFunction<int32()> InCurrentSignInNumber, const TSharedRef<IFlockLogger>& InLogger,
		TFunction<double()> InNextRoll = nullptr, TFunction<double()> InNow = nullptr);

	/**
	 * Starts repeating Send one jittered interval from now, under the current sign-in. Refused (null, with an error
	 * logged) while a call of that name runs, or with no name, no interval above zero or no Send.
	 * @param UseAnswer Gets each answer, never one that arrives after the call stopped or its sign-in ended.
	 * @param StoppedByFailure Gets the failure that ended the call: anything but no answer, a timeout, a 5xx, 408, 429 or an
	 * unreadable answer, which keep it going.
	 */
	template <typename T>
	TSharedPtr<FFlockRepeatingCall> Start(const FString& Name, double IntervalSeconds, FFlockRetryHandler::FOperation<T> Send,
		TFunction<void(const T&)> UseAnswer, TFunction<void(const FFlockError&)> StoppedByFailure)
	{
		if (!CanStart(Name, IntervalSeconds, static_cast<bool>(Send)))
		{
			return nullptr;
		}
		const TSharedRef<FFlockRepeatingCall> Call = MakeShared<FFlockRepeatingCall>(Shared, Name, Shared->CurrentSignInNumber(),
			IntervalSeconds);
		const TWeakPtr<FFlockRepeatingCall> WeakCall = Call;
		const TSharedRef<FFlockRepeatingCallsShared> SharedState = Shared;
		Call->SendOnce = [WeakCall, SharedState, Send, UseAnswer, StoppedByFailure]()
		{
			const TSharedPtr<FFlockRepeatingCall> Sending = WeakCall.Pin();
			if (!Sending.IsValid())
			{
				return;
			}
			Sending->bWaitingForAnswer = true;
			const double Wait = JitteredWait(Sending->IntervalSeconds, SharedState->NextRoll());
			const TSharedRef<FFlockRepeatingCall> Answering = Sending.ToSharedRef();
			Sending->InFlight = Send([Answering, SharedState, Wait, UseAnswer, StoppedByFailure](TFlockResult<T> Result)
			{
				double NextWait = Wait;
				// Stopped, or its sign-in ended, while the answer was on its way: nothing is used.
				if (Answering->IsRunning())
				{
					if (Result.bSuccess)
					{
						if (UseAnswer)
						{
							UseAnswer(Result.Value);
						}
					}
					else if (IsWorthAnotherTry(Result.Error))
					{
						if (Result.Error.bHasRetryAfter && Result.Error.RetryAfterSeconds > NextWait)
						{
							NextWait = Result.Error.RetryAfterSeconds;
						}
						SharedState->Logger->LogDebug(FString::Printf(TEXT("%s failed, trying again in %.1f s: %s"),
							*Answering->Name, NextWait, *Result.Error.Message));
					}
					else
					{
						Answering->Stop();
						if (StoppedByFailure)
						{
							StoppedByFailure(Result.Error);
						}
					}
				}
				Answering->bWaitingForAnswer = false;
				Answering->DueAt = SharedState->Now() + NextWait;
			});
		};
		Call->DueAt = Shared->Now() + JitteredWait(IntervalSeconds, Shared->NextRoll());
		Calls.Add(Call);
		return Call;
	}

	/** Sends every call that is due and not waiting for an answer; run once a frame. */
	void SendThoseDue();

	/** Stops every call and cancels the requests on their way: Flock shutting down. */
	void StopAll();

	/** The calls this holds, stopped ones included until their next due time; for tests. */
	int32 GetCallCountForTesting() const { return Calls.Num(); }

	/** The wait before a call's next request: its interval, from three quarters to one and a quarter of it. */
	static double JitteredWait(double IntervalSeconds, double Roll) { return IntervalSeconds * (0.75 + Roll * 0.5); }

	/** No answer, a timeout, a server error, a limit or an unreadable answer can pass; anything else is the server's word on this call. */
	static bool IsWorthAnotherTry(const FFlockError& Failure);

private:
	bool CanStart(const FString& Name, double IntervalSeconds, bool bHasSend) const;

	TSharedRef<FFlockRepeatingCallsShared> Shared;
	TArray<TSharedRef<FFlockRepeatingCall>> Calls;
	/** Reused every frame, so sending what is due allocates nothing while nothing is. */
	TArray<TSharedRef<FFlockRepeatingCall>> Due;
};
