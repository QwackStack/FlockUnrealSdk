// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockMultiplayerSession.h"
#include "Auth/FlockAuthSession.h"
#include "Multiplayer/FlockMultiplayerSessions.h"

namespace
{
	/** A third of the shorter of the server's default timeouts (60 s host, 90 s seat), for an answer that names no interval. */
	constexpr double SessionHeartbeatWhenNoneIsGiven = 20.0;
}

FString UFlockMultiplayerSession::GetEndReason() const
{
	if (!EndReasonSet.IsEmpty())
	{
		return EndReasonSet;
	}
	// A sign-in that ended reads as signed out at once; the session's end is raised on the next frame.
	const TSharedPtr<FFlockAuthSession> Pinned = AuthSession.Pin();
	if (!Pinned.IsValid() || Pinned->GetSignInNumber() != SignInNumber)
	{
		return FlockMultiplayerSessionEndReasons::SignedOut;
	}
	return FString();
}

FFlockRequestHandle UFlockMultiplayerSession::Leave(FFlockOnMultiplayerSession OnComplete)
{
	if (const TSharedPtr<FFlockMultiplayerSessions> Pinned = Owner.Pin())
	{
		return Pinned->Leave(this, MoveTemp(OnComplete));
	}
	if (OnComplete)
	{
		OnComplete(TFlockResult<UFlockMultiplayerSession*>::Fail(RefusedWithoutOwner()));
	}
	return FFlockRequestHandle();
}

FFlockRequestHandle UFlockMultiplayerSession::End(FFlockOnMultiplayerSession OnComplete)
{
	if (const TSharedPtr<FFlockMultiplayerSessions> Pinned = Owner.Pin())
	{
		return Pinned->End(this, MoveTemp(OnComplete));
	}
	if (OnComplete)
	{
		OnComplete(TFlockResult<UFlockMultiplayerSession*>::Fail(RefusedWithoutOwner()));
	}
	return FFlockRequestHandle();
}

FFlockRequestHandle UFlockMultiplayerSession::MakeHost(const FString& NewHostPlayerId, FFlockOnMultiplayerSession OnComplete)
{
	if (const TSharedPtr<FFlockMultiplayerSessions> Pinned = Owner.Pin())
	{
		return Pinned->MakeHost(this, NewHostPlayerId, MoveTemp(OnComplete));
	}
	if (OnComplete)
	{
		OnComplete(TFlockResult<UFlockMultiplayerSession*>::Fail(RefusedWithoutOwner()));
	}
	return FFlockRequestHandle();
}

FFlockRequestHandle UFlockMultiplayerSession::PublishConnection(const FString& Mode, const FFlockCommandData& Values,
	FFlockOnMultiplayerSession OnComplete)
{
	if (const TSharedPtr<FFlockMultiplayerSessions> Pinned = Owner.Pin())
	{
		return Pinned->PublishConnection(this, Mode, Values, MoveTemp(OnComplete));
	}
	if (OnComplete)
	{
		OnComplete(TFlockResult<UFlockMultiplayerSession*>::Fail(RefusedWithoutOwner()));
	}
	return FFlockRequestHandle();
}

FFlockRequestHandle UFlockMultiplayerSession::RequestJoinToken(TFunction<void(TFlockResult<FFlockMultiplayerSessionJoinToken>)> OnComplete)
{
	if (const TSharedPtr<FFlockMultiplayerSessions> Pinned = Owner.Pin())
	{
		return Pinned->RequestJoinToken(this, MoveTemp(OnComplete));
	}
	if (OnComplete)
	{
		OnComplete(TFlockResult<FFlockMultiplayerSessionJoinToken>::Fail(RefusedWithoutOwner()));
	}
	return FFlockRequestHandle();
}

FFlockRequestHandle UFlockMultiplayerSession::VerifyJoinToken(const FString& Token, TFunction<void(TFlockResult<FString>)> OnComplete)
{
	if (const TSharedPtr<FFlockMultiplayerSessions> Pinned = Owner.Pin())
	{
		return Pinned->VerifyJoinToken(this, Token, MoveTemp(OnComplete));
	}
	if (OnComplete)
	{
		OnComplete(TFlockResult<FString>::Fail(RefusedWithoutOwner()));
	}
	return FFlockRequestHandle();
}

void UFlockMultiplayerSession::Begin(const TWeakPtr<FFlockMultiplayerSessions>& InOwner, const TSharedRef<FFlockAuthSession>& InSession,
	int32 InSignInNumber, const FString& InPlayerId, const FFlockMultiplayerSessionReading& Reading)
{
	Owner = InOwner;
	AuthSession = InSession;
	SignInNumber = InSignInNumber;
	PlayerId = InPlayerId;
	Id = Reading.Id;
	CopyFrom(Reading);
}

FFlockMultiplayerSessionChanges UFlockMultiplayerSession::TakeReading(const FFlockMultiplayerSessionReading& Reading)
{
	const TArray<FString> Seated = Reading.SeatedPlayerIds();
	bool bPlayersChanged = Players.Num() != Seated.Num();
	for (int32 Index = 0; !bPlayersChanged && Index < Seated.Num(); ++Index)
	{
		bPlayersChanged = !Players[Index].PlayerId.Equals(Seated[Index], ESearchCase::CaseSensitive);
	}
	const bool bHostChanged = Reading.HostEpoch != HostEpoch || !Reading.HostPlayerId.Equals(HostPlayerId, ESearchCase::CaseSensitive);
	const bool bConnectionEpochMoved = Reading.ConnectionEpoch != ConnectionEpoch;
	const bool bHadConnection = bHasConnection;
	CopyFrom(Reading);
	// A publish moves the epoch; hosting moving on clears the address without moving it.
	const bool bConnectionChanged = bConnectionEpochMoved || bHadConnection != bHasConnection;

	FFlockMultiplayerSessionChanges Changes;
	Changes.bPlayers = bPlayersChanged;
	Changes.bHost = bHostChanged;
	Changes.bConnection = bConnectionChanged;
	return Changes;
}

void UFlockMultiplayerSession::RaiseChanges(const FFlockMultiplayerSessionChanges& Changes)
{
	if (Changes.bPlayers)
	{
		OnPlayersChanged.Broadcast();
	}
	if (Changes.bHost)
	{
		OnHostChanged.Broadcast();
	}
	if (Changes.bConnection)
	{
		OnConnectionChanged.Broadcast();
	}
}

void UFlockMultiplayerSession::MarkEnded(const FString& Reason, bool bRaise)
{
	EndReasonSet = Reason;
	if (bRaise)
	{
		OnEnded.Broadcast(Reason);
	}
}

void UFlockMultiplayerSession::CopyFrom(const FFlockMultiplayerSessionReading& Reading)
{
	JoinCode = Reading.JoinCode;
	HostPlayerId = Reading.HostPlayerId;
	HostEpoch = Reading.HostEpoch;
	MaxPlayers = Reading.MaxPlayers;
	Data = Reading.Data;
	Players.Reset();
	for (const FString& SeatedPlayerId : Reading.SeatedPlayerIds())
	{
		FFlockMultiplayerSessionPlayer Player;
		Player.PlayerId = SeatedPlayerId;
		Players.Add(Player);
	}
	ConnectionEpoch = Reading.ConnectionEpoch;
	HeartbeatIntervalSeconds = Reading.HeartbeatIntervalSeconds > 0 ? static_cast<double>(Reading.HeartbeatIntervalSeconds)
		: SessionHeartbeatWhenNoneIsGiven;
	// The server blanks the address for a player without a seat, and sends none before the host first publishes.
	bHasConnection = Reading.bHasConnection;
	Connection = Reading.bHasConnection ? Reading.Connection : FFlockMultiplayerSessionConnection();
}

FFlockError UFlockMultiplayerSession::RefusedWithoutOwner()
{
	return FFlockError::Make(EFlockErrorType::Validation, TEXT("This session has ended (signed_out): Flock has shut down."));
}
