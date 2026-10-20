// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Models/FlockMultiplayerModels.h"

/** The reasons themselves (FlockRelayFailureReasons) are public: a travel's result names them. */
namespace FlockRelayFailureReasons
{
	/** How to fix a relay failure of this reason: the one place each reason's fix is written. */
	FString FixFor(const FString& Reason);
}

/** Why the relay could not be used: one of FlockRelayFailureReasons (empty when the call was cancelled), and the failure to show, whose fix is the reason's. */
struct FFlockRelayFailure
{
	FString Reason;
	FFlockError Error;

	bool IsCancelled() const { return Error.Type == EFlockErrorType::Cancelled; }

	/** A failure of this reason; what is wrong in Message, and the reason's fix. */
	static FFlockRelayFailure Of(const FString& Reason, const FString& Message);

	/** Flock refused, or did not answer, the request for relay logins: the reason's fix, with Flock's own code, status and words kept. */
	static FFlockRelayFailure FromFlock(const FString& Reason, const FString& Message, const FFlockError& FlockFailure);

	/** The caller stopped waiting, or the relay was closed while the call waited. */
	static FFlockRelayFailure Cancelled(const FString& Message);
};

/** A relay call's answer: the value, or why the relay could not be used. */
template <typename T>
struct TFlockRelayResult
{
	bool bSuccess = false;
	T Value{};
	FFlockRelayFailure Failure;

	static TFlockRelayResult Ok(T InValue)
	{
		TFlockRelayResult Result;
		Result.bSuccess = true;
		Result.Value = MoveTemp(InValue);
		return Result;
	}

	static TFlockRelayResult Fail(const FFlockRelayFailure& InFailure)
	{
		TFlockRelayResult Result;
		Result.Failure = InFailure;
		return Result;
	}
};
