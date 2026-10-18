// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"

/** Why Flock's relay could not be used. An open set: compare with these and keep a default. */
namespace FlockRelayFailureReasons
{
	/** Flock listed no relay: switched off for this game in the Flock dashboard, or none set up. */
	inline constexpr const TCHAR* NotOffered = TEXT("relay_not_offered");
	/** Flock does not list, for this player, the relay server the host uses. */
	inline constexpr const TCHAR* ServerNotListed = TEXT("relay_server_not_listed");
	/** The studio's relay is paused until its bill is paid. */
	inline constexpr const TCHAR* Paused = TEXT("relay_paused");
	/** The player asked Flock for relay logins more than 12 times in a minute, or its seat drew the most logins one seat may (10). */
	inline constexpr const TCHAR* TooManyLogins = TEXT("relay_too_many_logins");
	/** Flock says the player has no seat in the session. */
	inline constexpr const TCHAR* NotInSession = TEXT("relay_not_in_session");
	/** Flock did not answer the request for relay logins. */
	inline constexpr const TCHAR* FlockUnreachable = TEXT("flock_unreachable");
	/** Flock refused the request for relay logins for another reason. */
	inline constexpr const TCHAR* LoginsRefused = TEXT("relay_logins_refused");
	/** The relay server's name could not be looked up, or the server did not answer. */
	inline constexpr const TCHAR* Unreachable = TEXT("relay_unreachable");
	/** The relay refused the login Flock minted. */
	inline constexpr const TCHAR* WrongLogin = TEXT("relay_wrong_login");
	/** The relay has no room for another address. */
	inline constexpr const TCHAR* Full = TEXT("relay_full");
	/** The relay refused to let packets in from the host's relay address, or from a player's. */
	inline constexpr const TCHAR* OpenRefused = TEXT("relay_open_refused");
	/** The relay refused a request for another reason. */
	inline constexpr const TCHAR* Refused = TEXT("relay_refused");
	/** The relay let the address go: a renewal was refused, or none was answered before it lapsed. */
	inline constexpr const TCHAR* Lost = TEXT("relay_lost");
	/** The relay connection was closed by its owner (the session's end) before the call. */
	inline constexpr const TCHAR* Closed = TEXT("relay_closed");
	/** The relay needs a thread of its own, and this launch of the game runs without threads. */
	inline constexpr const TCHAR* NoThread = TEXT("relay_no_thread");

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
