// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockRelayFailures.h"

namespace
{
	const TCHAR* const OpenTheRelay = TEXT("Open the relay");

	/** The reasons are the SDK's own spellings: told apart by their exact letters. */
	bool Is(const FString& Reason, const TCHAR* Spelling)
	{
		return Reason.Equals(Spelling, ESearchCase::CaseSensitive);
	}

	EFlockErrorType TypeFor(const FString& Reason)
	{
		return Is(Reason, FlockRelayFailureReasons::Unreachable) || Is(Reason, FlockRelayFailureReasons::FlockUnreachable)
			? EFlockErrorType::Connection
			: EFlockErrorType::Network;
	}
}

FString FlockRelayFailureReasons::FixFor(const FString& Reason)
{
	if (Is(Reason, NotOffered))
	{
		return TEXT("Turn the relay on for the game in the Flock dashboard, or connect players directly.");
	}
	if (Is(Reason, NotOfferedByTheHost))
	{
		return TEXT("Set Relay the same on every player of the game: a host whose Relay is Never, or whose relay could not be opened (its log "
			"says why), takes nobody through the relay, so a player whose Relay is Always cannot reach it.");
	}
	if (Is(Reason, ServerNotListed))
	{
		return TEXT("The host and this player were given different relay servers; have the host connect again so both use one Flock lists for each.");
	}
	if (Is(Reason, Paused))
	{
		return TEXT("Settle the studio's bill in the Flock dashboard; meanwhile players connect directly.");
	}
	if (Is(Reason, TooManyLogins))
	{
		return TEXT("Open the relay once per session rather than again and again. A seat that drew the most logins it may (10) gets no more; "
			"another session gives a seat of its own.");
	}
	if (Is(Reason, NotInSession))
	{
		return TEXT("Join the session again before connecting.");
	}
	if (Is(Reason, FlockUnreachable))
	{
		return TEXT("Check the device is online, then connect again.");
	}
	if (Is(Reason, LoginsRefused))
	{
		return TEXT("Fix what Flock's message above names, then connect again; if Flock keeps refusing, connect players directly.");
	}
	if (Is(Reason, Unreachable))
	{
		return TEXT("Check the device is online and that a firewall lets the game send UDP to the relay; otherwise players connect directly.");
	}
	if (Is(Reason, WrongLogin))
	{
		return TEXT("Connect again so Flock gives fresh relay logins.");
	}
	if (Is(Reason, Full))
	{
		return TEXT("The relay has no room for another address now; try again shortly, or connect players directly.");
	}
	if (Is(Reason, OpenRefused))
	{
		return TEXT("That address cannot come in through this relay: the player connects directly, or joins the session again.");
	}
	if (Is(Reason, Lost))
	{
		return TEXT("Connect again to reopen the relay.");
	}
	if (Is(Reason, Closed))
	{
		return TEXT("The session ended and closed its relay: host or join another session.");
	}
	if (Is(Reason, NoThread))
	{
		return TEXT("Launch the game without -nothreading so the relay can run on a thread of its own, or connect players directly.");
	}
	if (Is(Reason, Refused))
	{
		return TEXT("The relay's own code is in the reason above. Connect again so Flock gives fresh relay logins; if the relay keeps refusing, "
			"connect players directly.");
	}
	return TEXT("Connect again; if the relay keeps refusing, connect players directly.");
}

FFlockRelayFailure FFlockRelayFailure::Of(const FString& Reason, const FString& Message)
{
	FFlockRelayFailure Failure;
	Failure.Reason = Reason;
	Failure.Error = FFlockError::MakeWithFix(TypeFor(Reason), Message, FlockRelayFailureReasons::FixFor(Reason));
	Failure.Error.Operation = OpenTheRelay;
	return Failure;
}

FFlockRelayFailure FFlockRelayFailure::FromFlock(const FString& Reason, const FString& Message, const FFlockError& FlockFailure)
{
	FFlockRelayFailure Failure;
	Failure.Reason = Reason;
	Failure.Error = FlockFailure;
	Failure.Error.Type = TypeFor(Reason);
	Failure.Error.Message = Message;
	Failure.Error.Hint = FlockRelayFailureReasons::FixFor(Reason);
	Failure.Error.Operation = OpenTheRelay;
	return Failure;
}

FFlockRelayFailure FFlockRelayFailure::Cancelled(const FString& Message)
{
	FFlockRelayFailure Failure;
	Failure.Error = FFlockError::Make(EFlockErrorType::Cancelled, Message);
	Failure.Error.Operation = OpenTheRelay;
	return Failure;
}
