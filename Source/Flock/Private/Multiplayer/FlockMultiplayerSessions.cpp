// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockMultiplayerSessions.h"
#include "Http/FlockErrorHints.h"
#include "Multiplayer/Relay/FlockBridgeMessages.h"
#include "UObject/Package.h"

namespace
{
	/** Hands a session call its result; a call made without a completion hears nothing. */
	template <typename T>
	void AnswerSessionCall(const TFunction<void(TFlockResult<T>)>& OnComplete, const TFlockResult<T>& Result)
	{
		if (OnComplete)
		{
			OnComplete(Result);
		}
	}

	FFlockError SessionCallNoLongerCurrent()
	{
		return FFlockError::Make(EFlockErrorType::Cancelled,
			TEXT("The player signed out or changed, or Flock shut down, while the session was on its way"));
	}
}

FFlockMultiplayerSessions::FFlockMultiplayerSessions(const TSharedRef<FFlockMultiplayerSessionRequests>& InRequests,
	const TSharedRef<FFlockRepeatingCalls>& InRepeatingCalls, const TSharedRef<FFlockAuthSession>& InSession,
	const TSharedRef<IFlockLogger>& InLogger)
	: Requests(InRequests)
	, RepeatingCalls(InRepeatingCalls)
	, AuthSession(InSession)
	, Logger(InLogger)
	, DirectAddresses(MakeShared<FFlockDirectAddresses>(InRequests, [Calls = InRepeatingCalls]() { return Calls->Now(); },
		[Session = InSession]() { return Session->GetSignInNumber(); }, InLogger))
{
}

FFlockError FFlockMultiplayerSessions::ShutDownError()
{
	return FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Flock shut down while the session was on its way"));
}

bool FFlockMultiplayerSessions::IsOutOfTheSession(const FFlockError& Failure)
{
	return Failure.ErrorCode == EFlockErrorCode::MultiplayerSessionNotFound || Failure.ErrorCode == EFlockErrorCode::MultiplayerNotAParticipant;
}

bool FFlockMultiplayerSessions::IsGiven(const FString& Value, const TCHAR* Name, FFlockError& OutError)
{
	if (Value.IsEmpty())
	{
		OutError = FFlockError::MakeWithFix(EFlockErrorType::Validation, FString::Printf(TEXT("%s is empty"), Name), FString::Printf(TEXT("Pass a %s; this call cannot run without one."), Name));
		return false;
	}
	return true;
}

template <typename T>
void FFlockMultiplayerSessions::Ask(UFlockMultiplayerSession* Session, const TSharedRef<FFlockRequestsInTurn>& InTurn,
	TFunction<FFlockRequestHandle(TFunction<void(TFlockResult<T>)>)> Send, TFunction<void(TFlockResult<T>)> OnAnswer)
{
	const TWeakObjectPtr<UFlockMultiplayerSession> WeakSession = Session;
	const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
	InTurn->Send([&]()
	{
		return Send([WeakSelf, WeakSession, InTurn, OnAnswer](TFlockResult<T> Answered)
		{
			const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin();
			if (Self.IsValid() && !Answered.bSuccess && IsOutOfTheSession(Answered.Error) && WeakSession.IsValid())
			{
				Self->LearnWhyItEnded(WeakSession.Get(), InTurn, [OnAnswer, Answered]() { OnAnswer(Answered); });
				return;
			}
			OnAnswer(Answered);
		});
	});
}

FFlockRequestHandle FFlockMultiplayerSessions::Host(TOptional<int32> MaxPlayers, const FFlockCommandData& Data, bool bSameVersionOnly,
	FFlockOnMultiplayerSession OnComplete)
{
	int32 SignInNumber = 0;
	FString PlayerId;
	FFlockError Refused;
	if (!RequireSignedIn(SignInNumber, PlayerId, Refused))
	{
		AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Refused));
		return FFlockRequestHandle();
	}
	const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
	return Requests->Host(SignInNumber, MaxPlayers, Data, bSameVersionOnly,
		[WeakSelf, SignInNumber, PlayerId, OnComplete](TFlockResult<FFlockMultiplayerSessionReading> Hosted)
		{
			const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(ShutDownError()));
				return;
			}
			if (!Hosted.bSuccess)
			{
				AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Hosted.Error));
				return;
			}
			if (!Self->IsStillCurrent(SignInNumber))
			{
				AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(SessionCallNoLongerCurrent()));
				return;
			}
			AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Ok(Self->TakeAnswerOfChange(Hosted.Value, SignInNumber, PlayerId)));
		});
}

FFlockRequestHandle FFlockMultiplayerSessions::Join(const FString& JoinCode, FFlockOnMultiplayerSession OnComplete)
{
	int32 SignInNumber = 0;
	FString PlayerId;
	FFlockError Refused;
	if (!IsGiven(JoinCode, TEXT("Join Code"), Refused) || !RequireSignedIn(SignInNumber, PlayerId, Refused))
	{
		AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Refused));
		return FFlockRequestHandle();
	}
	const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
	return Requests->Join(SignInNumber, JoinCode,
		[WeakSelf, SignInNumber, PlayerId, OnComplete](TFlockResult<FFlockMultiplayerSessionReading> Joined)
		{
			const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(ShutDownError()));
				return;
			}
			if (!Joined.bSuccess)
			{
				AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Joined.Error));
				return;
			}
			if (!Self->IsStillCurrent(SignInNumber))
			{
				AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(SessionCallNoLongerCurrent()));
				return;
			}
			AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Ok(Self->TakeAnswerOfChange(Joined.Value, SignInNumber, PlayerId)));
		});
}

FFlockRequestHandle FFlockMultiplayerSessions::Get(const FString& SessionId, FFlockOnMultiplayerSession OnComplete)
{
	int32 SignInNumber = 0;
	FString PlayerId;
	FFlockError Refused;
	if (!IsGiven(SessionId, TEXT("Session ID"), Refused) || !RequireSignedIn(SignInNumber, PlayerId, Refused))
	{
		AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Refused));
		return FFlockRequestHandle();
	}
	const TSharedRef<FFlockRequestsInTurn> InTurn = FFlockRequestsInTurn::Make();
	ReadUntilNewest(SignInNumber, PlayerId, SessionId, InTurn, 1, MoveTemp(OnComplete));
	return InTurn->GetHandle();
}

FFlockRequestHandle FFlockMultiplayerSessions::GetMine(FFlockOnMultiplayerSession OnComplete)
{
	int32 SignInNumber = 0;
	FString PlayerId;
	FFlockError Refused;
	if (!RequireSignedIn(SignInNumber, PlayerId, Refused))
	{
		AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Refused));
		return FFlockRequestHandle();
	}
	const TSharedRef<FFlockRequestsInTurn> InTurn = FFlockRequestsInTurn::Make();
	ReadUntilNewest(SignInNumber, PlayerId, FString(), InTurn, 1, MoveTemp(OnComplete));
	return InTurn->GetHandle();
}

FFlockRequestHandle FFlockMultiplayerSessions::Leave(UFlockMultiplayerSession* Session, FFlockOnMultiplayerSession OnComplete)
{
	FFlockError Refused;
	if (!IsRunning(Session, Refused))
	{
		AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Refused));
		return FFlockRequestHandle();
	}
	const int32 SignInNumber = Session->SignInNumber;
	const FString SessionId = Session->Id;
	const TWeakObjectPtr<UFlockMultiplayerSession> WeakSession = Session;
	const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
	LeavingSessionId = SessionId;
	return Requests->Leave(SignInNumber, SessionId,
		[WeakSelf, WeakSession, SignInNumber, SessionId, OnComplete](TFlockResult<FFlockMultiplayerSessionReading> Answered)
		{
			const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(ShutDownError()));
				return;
			}
			// Out of the session already is what leaving asks for; a retried leave whose first answer was lost lands here too.
			if (!Answered.bSuccess && !IsOutOfTheSession(Answered.Error))
			{
				Self->ForgetLeaveOf(SessionId);
				AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Answered.Error));
				return;
			}
			if (Answered.bSuccess)
			{
				Self->TakeAnswerOfChange(Answered.Value, SignInNumber, FString());
			}
			Self->ForgetLeaveOf(SessionId);
			if (UFlockMultiplayerSession* LeftSession = WeakSession.Get())
			{
				Self->MarkEnded(LeftSession, FlockMultiplayerSessionEndReasons::Left);
			}
			AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Ok(WeakSession.Get()));
		});
}

FFlockRequestHandle FFlockMultiplayerSessions::End(UFlockMultiplayerSession* Session, FFlockOnMultiplayerSession OnComplete)
{
	const TSharedRef<FFlockMultiplayerSessionRequests> RequestsRef = Requests;
	const int32 SignInNumber = Session != nullptr ? Session->SignInNumber : 0;
	const FString SessionId = Session != nullptr ? Session->Id : FString();
	return Change(Session, [RequestsRef, SignInNumber, SessionId](FFlockMultiplayerSessionRequests::FOnSession OnSent)
	{
		return RequestsRef->End(SignInNumber, SessionId, MoveTemp(OnSent));
	}, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerSessions::MakeHost(UFlockMultiplayerSession* Session, const FString& PlayerId,
	FFlockOnMultiplayerSession OnComplete)
{
	FFlockError Refused;
	if (!IsGiven(PlayerId, TEXT("Player ID"), Refused))
	{
		AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Refused));
		return FFlockRequestHandle();
	}
	const TSharedRef<FFlockMultiplayerSessionRequests> RequestsRef = Requests;
	const int32 SignInNumber = Session != nullptr ? Session->SignInNumber : 0;
	const FString SessionId = Session != nullptr ? Session->Id : FString();
	return Change(Session, [RequestsRef, SignInNumber, SessionId, PlayerId](FFlockMultiplayerSessionRequests::FOnSession OnSent)
	{
		return RequestsRef->MakeHost(SignInNumber, SessionId, PlayerId, MoveTemp(OnSent));
	}, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerSessions::PublishConnection(UFlockMultiplayerSession* Session, const FString& Mode,
	const FFlockCommandData& Values, FFlockOnMultiplayerSession OnComplete)
{
	FFlockError Refused;
	if (!IsGiven(Mode, TEXT("Mode"), Refused))
	{
		AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Refused));
		return FFlockRequestHandle();
	}
	// The mode goes with the values; one given among the values is replaced by it.
	FFlockCommandData Connection = Values;
	Connection.Set(TEXT("mode"), Mode);
	const TSharedRef<FFlockMultiplayerSessionRequests> RequestsRef = Requests;
	const int32 SignInNumber = Session != nullptr ? Session->SignInNumber : 0;
	const FString SessionId = Session != nullptr ? Session->Id : FString();
	return Change(Session, [RequestsRef, SignInNumber, SessionId, Connection](FFlockMultiplayerSessionRequests::FOnSession OnSent)
	{
		return RequestsRef->PublishConnection(SignInNumber, SessionId, Connection, MoveTemp(OnSent));
	}, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerSessions::RequestJoinToken(UFlockMultiplayerSession* Session,
	TFunction<void(TFlockResult<FFlockMultiplayerSessionJoinToken>)> OnComplete)
{
	return AskForAJoinToken(Session, MoveTemp(OnComplete), nullptr);
}

FFlockRequestHandle FFlockMultiplayerSessions::RequestJoinTokenToKeepOnHand(UFlockMultiplayerSession* Session,
	TFunction<void(TFlockResult<FFlockMultiplayerSessionJoinToken>)> OnComplete)
{
	return AskForAJoinToken(Session, MoveTemp(OnComplete), &FFlockMultiplayerSessions::IsOutOfTheSession);
}

FFlockRequestHandle FFlockMultiplayerSessions::AskForAJoinToken(UFlockMultiplayerSession* Session,
	TFunction<void(TFlockResult<FFlockMultiplayerSessionJoinToken>)> OnComplete, TFunction<bool(const FFlockError&)> IsExpectedFailure)
{
	FFlockError Refused;
	if (!IsRunning(Session, Refused))
	{
		AnswerSessionCall(OnComplete, TFlockResult<FFlockMultiplayerSessionJoinToken>::Fail(Refused));
		return FFlockRequestHandle();
	}
	const TSharedRef<FFlockMultiplayerSessionRequests> RequestsRef = Requests;
	const int32 SignInNumber = Session->SignInNumber;
	const FString SessionId = Session->Id;
	const TSharedRef<FFlockRequestsInTurn> InTurn = FFlockRequestsInTurn::Make();
	Ask<FFlockMultiplayerSessionJoinToken>(Session, InTurn,
		[RequestsRef, SignInNumber, SessionId, IsExpectedFailure](TFunction<void(TFlockResult<FFlockMultiplayerSessionJoinToken>)> OnSent)
		{
			return RequestsRef->RequestJoinToken(SignInNumber, SessionId, MoveTemp(OnSent), IsExpectedFailure);
		},
		[OnComplete](TFlockResult<FFlockMultiplayerSessionJoinToken> Answered) { AnswerSessionCall(OnComplete, Answered); });
	return InTurn->GetHandle();
}

FFlockRequestHandle FFlockMultiplayerSessions::VerifyJoinToken(UFlockMultiplayerSession* Session, const FString& Token,
	TFunction<void(TFlockResult<FString>)> OnComplete)
{
	FFlockError Refused;
	if (!IsGiven(Token, TEXT("Join Token"), Refused) || !IsRunning(Session, Refused))
	{
		AnswerSessionCall(OnComplete, TFlockResult<FString>::Fail(Refused));
		return FFlockRequestHandle();
	}
	const TSharedRef<FFlockMultiplayerSessionRequests> RequestsRef = Requests;
	const int32 SignInNumber = Session->SignInNumber;
	const FString SessionId = Session->Id;
	const TSharedRef<FFlockRequestsInTurn> InTurn = FFlockRequestsInTurn::Make();
	Ask<FFlockVerifiedJoinToken>(Session, InTurn,
		[RequestsRef, SignInNumber, SessionId, Token](TFunction<void(TFlockResult<FFlockVerifiedJoinToken>)> OnSent)
		{
			return RequestsRef->VerifyJoinToken(SignInNumber, SessionId, Token, MoveTemp(OnSent));
		},
		[OnComplete](TFlockResult<FFlockVerifiedJoinToken> Answered)
		{
			AnswerSessionCall(OnComplete, Answered.bSuccess ? TFlockResult<FString>::Ok(Answered.Value.PlayerId)
				: TFlockResult<FString>::Fail(Answered.Error));
		});
	return InTurn->GetHandle();
}

FFlockRequestHandle FFlockMultiplayerSessions::PublishDirectConnection(UFlockMultiplayerSession* Session, int32 Port,
	FFlockOnMultiplayerSession OnComplete)
{
	FFlockError Refused;
	// The backend checks nothing of a direct connection, so what it holds is checked here, before anything is asked.
	if (Port < 1 || Port > 65535)
	{
		Refused = FFlockError::MakeWithFix(EFlockErrorType::Validation, FString::Printf(TEXT("The port must be from 1 to 65535, not %d"), Port), TEXT("Give the port the game listens on for players."));
	}
	if (!Refused.Message.IsEmpty() || !IsRunning(Session, Refused))
	{
		AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Refused));
		return FFlockRequestHandle();
	}
	const TWeakObjectPtr<UFlockMultiplayerSession> WeakSession = Session;
	const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
	const TSharedRef<FFlockRequestsInTurn> InTurn = FFlockRequestsInTurn::Make();
	InTurn->Send([&]()
	{
		return DirectAddresses->Find(Session->SignInNumber, [WeakSelf, WeakSession, InTurn, Port, OnComplete](TFlockResult<FFlockDeviceAddresses> Found)
		{
			const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(ShutDownError()));
				return;
			}
			if (!Found.bSuccess)
			{
				AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Found.Error));
				return;
			}
			if (Found.Value.Lan.IsEmpty() && Found.Value.Public.IsEmpty())
			{
				AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Network,
					TEXT("This device has no network address to publish"), TEXT("Connect the device to a network, then publish the connection again."))));
				return;
			}
			InTurn->Send([&]()
			{
				return Self->PublishConnection(WeakSession.Get(), FlockMultiplayerConnectionModes::Direct,
					FFlockDirectAddresses::ValuesToPublish(Found.Value, Port), OnComplete);
			});
		});
	});
	return InTurn->GetHandle();
}

FFlockRequestHandle FFlockMultiplayerSessions::PublishRelayConnection(UFlockMultiplayerSession* Session, const TSharedRef<FFlockRelayConnection>& OnTheRelay,
	int32 Port, bool bWithDirectAddresses, FFlockOnMultiplayerSession OnComplete)
{
	FFlockError Refused;
	if (!IsRunning(Session, Refused))
	{
		AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Refused));
		return FFlockRequestHandle();
	}
	const FString RelayAddress = OnTheRelay->GetAddress().ToString();
	const FString RelayServer = OnTheRelay->GetServer();
	const auto RelayValues = [RelayAddress, RelayServer](FFlockCommandData Values)
	{
		Values.Set(FlockRelayConnectionValues::RelayAddress, RelayAddress);
		Values.Set(FlockRelayConnectionValues::RelayServer, RelayServer);
		// This host's bridge keeps the bridges' own messages from the engine, so its players' bridges may send them.
		Values.Set(FlockRelayConnectionValues::BridgeMessages, static_cast<int32>(FFlockBridgeMessages::Version));
		return Values;
	};
	if (!bWithDirectAddresses)
	{
		return PublishConnection(Session, FlockMultiplayerConnectionModes::Relay, RelayValues(FFlockCommandData()), MoveTemp(OnComplete));
	}
	// Players on the host's network skip the relay; an address that cannot be found is left out.
	const TWeakObjectPtr<UFlockMultiplayerSession> WeakSession = Session;
	const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
	const TSharedRef<FFlockRequestsInTurn> InTurn = FFlockRequestsInTurn::Make();
	InTurn->Send([&]()
	{
		return DirectAddresses->Find(Session->SignInNumber, [WeakSelf, WeakSession, InTurn, Port, RelayValues, OnComplete](TFlockResult<FFlockDeviceAddresses> Found)
		{
			const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(ShutDownError()));
				return;
			}
			if (!Found.bSuccess)
			{
				AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Found.Error));
				return;
			}
			InTurn->Send([&]()
			{
				return Self->PublishConnection(WeakSession.Get(), FlockMultiplayerConnectionModes::Relay,
					RelayValues(FFlockDirectAddresses::ValuesToPublish(Found.Value, Port)), OnComplete);
			});
		});
	});
	return InTurn->GetHandle();
}

FFlockRequestHandle FFlockMultiplayerSessions::PublishSeatRelayAddress(UFlockMultiplayerSession* Session, const TSharedRef<FFlockRelayConnection>& OnTheRelay,
	FFlockOnMultiplayerSession OnComplete)
{
	FFlockError Refused;
	if (!IsRunning(Session, Refused))
	{
		AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Refused));
		return FFlockRequestHandle();
	}
	const FString RelayAddress = OnTheRelay->GetAddress().ToString();
	// Skipped when the seat says both already: this relay address, and that this device's bridge reads the bridges' messages.
	const FFlockMultiplayerSessionSeat* Mine = Session->SeatOf(Session->PlayerId);
	if (Mine != nullptr && Mine->RelayAddress.Equals(RelayAddress, ESearchCase::CaseSensitive) && Mine->bReadsBridgeMessages)
	{
		AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Ok(Session));
		return FFlockRequestHandle();
	}
	FFlockCommandData Connection;
	Connection.Set(TEXT("mode"), FlockMultiplayerConnectionModes::Relay);
	Connection.Set(FlockRelayConnectionValues::RelayAddress, RelayAddress);
	Connection.Set(FlockRelayConnectionValues::BridgeMessages, static_cast<int32>(FFlockBridgeMessages::Version));
	const TSharedRef<FFlockMultiplayerSessionRequests> RequestsRef = Requests;
	const int32 SignInNumber = Session->SignInNumber;
	const FString SessionId = Session->Id;
	return Change(Session, [RequestsRef, SignInNumber, SessionId, Connection](FFlockMultiplayerSessionRequests::FOnSession OnSent)
	{
		return RequestsRef->PublishSeatConnection(SignInNumber, SessionId, Connection, MoveTemp(OnSent));
	}, MoveTemp(OnComplete));
}

void FFlockMultiplayerSessions::OpenTheRelayToSeatedPlayers(UFlockMultiplayerSession* Session)
{
	const TSharedPtr<FFlockRelayConnection> RelayNow = Relay.IsValid() ? Relay->Connection : nullptr;
	if (Session == nullptr || Session->HasEnded() || !Session->IsHost() || !RelayNow.IsValid() || !RelayNow->IsOpen()
		|| !Relay->SessionId.Equals(Session->Id, ESearchCase::CaseSensitive))
	{
		return;
	}
	const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
	TSet<FFlockRelayPeer> ReadingBridgeMessages;
	for (const FFlockMultiplayerSessionSeat& Seat : Session->SeatsOnTheRelay)
	{
		FFlockRelayPeer Address;
		if (Seat.PlayerId.Equals(Session->PlayerId, ESearchCase::CaseSensitive) || !FFlockRelayPeer::TryParse(Seat.RelayAddress, Address))
		{
			continue;
		}
		if (Seat.bReadsBridgeMessages)
		{
			ReadingBridgeMessages.Add(Address);
		}
		// An IP open or being opened is the relay's own to answer at once (one request an IP); a refused one is skipped here, so the
		// refusal is told once rather than at every reading. One not answered is asked again at the next.
		if (RelayNow->RefusedToOpenTo(Address))
		{
			continue;
		}
		RelayNow->OpenTo(Address, [WeakSelf](TFlockRelayResult<bool> Opened)
		{
			const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin();
			if (Self.IsValid() && !Opened.bSuccess && Opened.Failure.Reason.Equals(FlockRelayFailureReasons::OpenRefused, ESearchCase::CaseSensitive))
			{
				Self->Logger->LogWarning(FString::Printf(TEXT("A player's relay address cannot be let in, so that player cannot join through Flock's relay: %s"),
					*Opened.Failure.Error.ReasonAndFix()));
			}
		});
	}
	// The host's bridge offers a direct path only to players whose bridges read its messages, as their seats say.
	const FFlockRelayBridge::FPtr Bridge = JoinChecks.IsValid() ? JoinChecks->GetRelayBridge() : nullptr;
	if (Bridge.IsValid() && Bridge->GetRelay() == RelayNow.ToSharedRef())
	{
		Bridge->SetPeersThatReadBridgeMessages(ReadingBridgeMessages);
	}
}

void FFlockMultiplayerSessions::KeepReadingForRelayedPlayers()
{
	UFlockMultiplayerSession* HeldNow = Held.Get();
	const TSharedPtr<FFlockRelayConnection> RelayNow = Relay.IsValid() ? Relay->Connection : nullptr;
	const bool bWanted = HeldNow != nullptr && !bStopped && HeldNow->IsHost() && RelayNow.IsValid() && RelayNow->IsOpen()
		&& Relay->SessionId.Equals(HeldNow->Id, ESearchCase::CaseSensitive) && RelayNow->HandsArrivalsOver()
		&& RelayedPlayersReadRefusedFor.Pin() != RelayNow;
	if (!bWanted || RelayedPlayersReadFor.Pin() != RelayNow)
	{
		StopReadingForRelayedPlayers();
	}
	if (!bWanted || (RelayedPlayersRead.IsValid() && RelayedPlayersRead->IsRunning()))
	{
		return;
	}
	const FString SessionId = HeldNow->Id;
	const int32 SignInNumber = HeldNow->SignInNumber;
	const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
	const TWeakPtr<FFlockRelayConnection> ReadFor = RelayNow;
	RelayedPlayersReadFor = RelayNow;
	RelayedPlayersRead = RepeatingCalls->Start<FFlockNumberedSessionReading>(RelayedPlayersReadCallName, RelayedPlayersReadSeconds,
		[WeakSelf, SignInNumber, SessionId](TFunction<void(TFlockResult<FFlockNumberedSessionReading>)> OnResult) -> FFlockRequestHandle
		{
			if (const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin())
			{
				return Self->ReadNumbered(SignInNumber, SessionId, MoveTemp(OnResult));
			}
			OnResult(TFlockResult<FFlockNumberedSessionReading>::Fail(ShutDownError()));
			return FFlockRequestHandle();
		},
		[WeakSelf](const FFlockNumberedSessionReading& Read)
		{
			if (const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin())
			{
				Self->TakeRepeatedReading(Read);
			}
		},
		[WeakSelf, SessionId, ReadFor](const FFlockError& Failure)
		{
			if (const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin())
			{
				Self->RelayedPlayersReadStopped(SessionId, ReadFor, Failure);
			}
		});
}

void FFlockMultiplayerSessions::StopReadingForRelayedPlayers()
{
	if (RelayedPlayersRead.IsValid())
	{
		RelayedPlayersRead->Stop();
		RelayedPlayersRead.Reset();
	}
	RelayedPlayersReadFor.Reset();
}

void FFlockMultiplayerSessions::RelayedPlayersReadStopped(const FString& SessionId, const TWeakPtr<FFlockRelayConnection>& ReadFor, const FFlockError& Failure)
{
	// Not started again for this relay, or the frames would send it again until the reason landed.
	RelayedPlayersReadRefusedFor = ReadFor;
	UFlockMultiplayerSession* HeldNow = Held.Get();
	if (HeldNow == nullptr || !HeldNow->Id.Equals(SessionId, ESearchCase::CaseSensitive))
	{
		return;
	}
	if (IsOutOfTheSession(Failure))
	{
		LearnWhyItEnded(HeldNow, nullptr, []() {});
		return;
	}
	Logger->LogWarning(FString::Printf(TEXT("The session is no longer read every 2 s for players joining through Flock's relay: %s. The heartbeats "
		"still bring their relay addresses, about every 20 s."), *Failure.ReasonAndFix()));
}

FFlockRequestHandle FFlockMultiplayerSessions::WaitForConnection(UFlockMultiplayerSession* Session, double TimeoutSeconds,
	FFlockOnMultiplayerSession OnComplete)
{
	FFlockError Refused;
	if (!(TimeoutSeconds > 0.0))
	{
		Refused = FFlockError::MakeWithFix(EFlockErrorType::Validation, TEXT("WaitForConnection needs a time to wait that is longer than zero"), TEXT("Pass the number of seconds to wait for the host's connection, such as 30."));
	}
	if (!Refused.Message.IsEmpty() || !IsRunning(Session, Refused))
	{
		AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Refused));
		return FFlockRequestHandle();
	}
	if (Session->HasConnection())
	{
		AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Ok(Session));
		return FFlockRequestHandle();
	}
	const FConnectionWaiterRef Waiter = MakeShared<FConnectionWaiter>();
	Waiter->Session.Reset(Session);
	Waiter->Deadline = RepeatingCalls->Now() + TimeoutSeconds;
	Waiter->OnComplete = MoveTemp(OnComplete);
	ConnectionWaiters.Add(Waiter);
	KeepReadingForConnection();

	const TSharedRef<FFlockCancelToken> Token = MakeShared<FFlockCancelToken>();
	const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
	const TWeakPtr<FConnectionWaiter> WeakWaiter = Waiter;
	Token->Canceller = [WeakSelf, WeakWaiter]()
	{
		const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin();
		const TSharedPtr<FConnectionWaiter> Pinned = WeakWaiter.Pin();
		if (Self.IsValid() && Pinned.IsValid() && Self->ConnectionWaiters.Contains(Pinned.ToSharedRef()))
		{
			Self->AnswerWait(Pinned.ToSharedRef(), TFlockResult<UFlockMultiplayerSession*>::Fail(FFlockError::Make(EFlockErrorType::Cancelled,
				TEXT("Request cancelled"))));
		}
	};
	return FFlockRequestHandle(Token);
}

FFlockRequestHandle FFlockMultiplayerSessions::FindDirectAddress(UFlockMultiplayerSession* Session,
	TFunction<void(TFlockResult<FFlockDirectAddress>)> OnComplete)
{
	return FindDirectAddressForTravel(Session, [OnComplete](TFlockResult<FFlockDirectAddress> Found, bool) { AnswerSessionCall(OnComplete, Found); });
}

FFlockRequestHandle FFlockMultiplayerSessions::FindDirectAddressForTravel(UFlockMultiplayerSession* Session, FOnDirectAddressFound OnComplete)
{
	const auto Answer = [&OnComplete](const TFlockResult<FFlockDirectAddress>& Found, bool bShares)
	{
		if (OnComplete)
		{
			OnComplete(Found, bShares);
		}
	};
	if (Session == nullptr)
	{
		Answer(TFlockResult<FFlockDirectAddress>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation, TEXT("No session was given"), FFlockErrorHints::PassAHeldSession())), false);
		return FFlockRequestHandle();
	}
	FFlockPublishedDirectAddresses Published;
	if (!Session->HasConnection() || !FFlockDirectAddresses::ReadPublished(Session->GetConnection(), Published))
	{
		Answer(TFlockResult<FFlockDirectAddress>::Ok(FFlockDirectAddress()), false);
		return FFlockRequestHandle();
	}
	// A device whose public address is unknown may be anywhere, so only a match of both public addresses picks the LAN one.
	const auto Chosen = [Published]()
	{
		FFlockDirectAddress Address;
		Address.Address = !Published.Address.IsEmpty() ? Published.Address : !Published.Public.IsEmpty() ? Published.Public : Published.Lan;
		Address.Port = Address.Address.IsEmpty() ? 0 : Published.Port;
		Address.bIsLan = !Address.Address.IsEmpty() && Address.Address.Equals(Published.Lan, ESearchCase::CaseSensitive);
		return Address;
	};
	// A host that knew only one of its addresses gives no sign of sharing a network with this device.
	if (Published.Lan.IsEmpty() || Published.Public.IsEmpty())
	{
		Answer(TFlockResult<FFlockDirectAddress>::Ok(Chosen()), false);
		return FFlockRequestHandle();
	}
	return DirectAddresses->Find(Session->SignInNumber, [Published, Chosen, OnComplete](TFlockResult<FFlockDeviceAddresses> Found)
	{
		if (!OnComplete)
		{
			return;
		}
		if (!Found.bSuccess)
		{
			OnComplete(TFlockResult<FFlockDirectAddress>::Fail(Found.Error), false);
			return;
		}
		if (Found.Value.Public.Equals(Published.Public, ESearchCase::CaseSensitive))
		{
			FFlockDirectAddress Lan;
			Lan.Address = Published.Lan;
			Lan.Port = Published.Port;
			Lan.bIsLan = true;
			OnComplete(TFlockResult<FFlockDirectAddress>::Ok(Lan), true);
			return;
		}
		OnComplete(TFlockResult<FFlockDirectAddress>::Ok(Chosen()), false);
	});
}

FFlockRequestHandle FFlockMultiplayerSessions::TravelToSession(UFlockMultiplayerSession* Session, UObject* WorldContextObject,
	const FFlockTravelToSessionOptions& Options, TFunction<void(TFlockResult<FFlockTravelToSessionResult>)> OnComplete)
{
	if (bStopped)
	{
		AnswerSessionCall(OnComplete, TFlockResult<FFlockTravelToSessionResult>::Fail(ShutDownError()));
		return FFlockRequestHandle();
	}
	return GetTravels().Travel(Session, WorldContextObject, Options, MoveTemp(OnComplete));
}

FFlockTravels& FFlockMultiplayerSessions::GetTravels()
{
	if (!Travels.IsValid())
	{
		Travels = MakeShared<FFlockTravels>(AsShared(), Logger, [Calls = RepeatingCalls]() { return Calls->Now(); });
	}
	return *Travels;
}

FFlockRelayBridge::FPtr FFlockMultiplayerSessions::GetRelayBridgeForTesting() const
{
	if (JoinChecks.IsValid() && JoinChecks->GetRelayBridge().IsValid())
	{
		return JoinChecks->GetRelayBridge();
	}
	return Travels.IsValid() ? Travels->GetPlayersBridgeForTesting() : nullptr;
}

FFlockJoinChecksOnHost& FFlockMultiplayerSessions::GetJoinChecks()
{
	if (!JoinChecks.IsValid())
	{
		JoinChecks = MakeShared<FFlockJoinChecksOnHost>(Logger, [Calls = RepeatingCalls]() { return Calls->Now(); });
	}
	return *JoinChecks;
}

void FFlockMultiplayerSessions::MoveConnectingOn()
{
	if (Relay.IsValid() && Relay->Connection.IsValid())
	{
		// Held through its answers, which may close it and start another.
		const TSharedRef<FFlockRelayConnection> Ticking = Relay->Connection.ToSharedRef();
		Ticking->Tick();
		TellARelayLost();
	}
	DirectAddresses->Tick();
	if (Travels.IsValid())
	{
		Travels->Tick();
	}
	if (JoinChecks.IsValid())
	{
		JoinChecks->Tick();
	}
	KeepReadingForRelayedPlayers();
	if (ConnectionWaiters.Num() == 0)
	{
		return;
	}
	const double NowSeconds = RepeatingCalls->Now();
	const TArray<FConnectionWaiterRef> Waiting = ConnectionWaiters;
	for (const FConnectionWaiterRef& Waiter : Waiting)
	{
		// The time ran out: the wait ends with the session as it is, and only the wait ends.
		if (NowSeconds >= Waiter->Deadline && ConnectionWaiters.Contains(Waiter))
		{
			AnswerWait(Waiter, TFlockResult<UFlockMultiplayerSession*>::Ok(Waiter->Session.Get()));
		}
	}
}

void FFlockMultiplayerSessions::SettleWaits()
{
	if (ConnectionWaiters.Num() == 0)
	{
		return;
	}
	const TArray<FConnectionWaiterRef> Waiting = ConnectionWaiters;
	for (const FConnectionWaiterRef& Waiter : Waiting)
	{
		const UFlockMultiplayerSession* Of = Waiter->Session.Get();
		if (ConnectionWaiters.Contains(Waiter) && (Of->HasEnded() || Of->HasConnection()))
		{
			AnswerWait(Waiter, TFlockResult<UFlockMultiplayerSession*>::Ok(Waiter->Session.Get()));
		}
	}
}

void FFlockMultiplayerSessions::AnswerWait(const FConnectionWaiterRef& Waiter, const TFlockResult<UFlockMultiplayerSession*>& Result)
{
	ConnectionWaiters.Remove(Waiter);
	if (ConnectionWaiters.Num() == 0)
	{
		StopReadingForConnection();
	}
	// Kept alive through the completion, which may drop the game's own hold on the session.
	const TStrongObjectPtr<UFlockMultiplayerSession> KeepWhileAnswering = Waiter->Session;
	AnswerSessionCall(Waiter->OnComplete, Result);
}

void FFlockMultiplayerSessions::KeepReadingForConnection()
{
	UFlockMultiplayerSession* HeldNow = Held.Get();
	if (HeldNow == nullptr || bStopped || ConnectionWaiters.Num() == 0 || (ConnectionWait.IsValid() && ConnectionWait->IsRunning()))
	{
		return;
	}
	const FString SessionId = HeldNow->Id;
	const int32 SignInNumber = HeldNow->SignInNumber;
	const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
	ConnectionWait = RepeatingCalls->Start<FFlockNumberedSessionReading>(ConnectionWaitCallName, ConnectionWaitSeconds,
		[WeakSelf, SignInNumber, SessionId](TFunction<void(TFlockResult<FFlockNumberedSessionReading>)> OnResult) -> FFlockRequestHandle
		{
			if (const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin())
			{
				return Self->ReadNumbered(SignInNumber, SessionId, MoveTemp(OnResult));
			}
			OnResult(TFlockResult<FFlockNumberedSessionReading>::Fail(ShutDownError()));
			return FFlockRequestHandle();
		},
		[WeakSelf](const FFlockNumberedSessionReading& Read)
		{
			if (const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin())
			{
				Self->TakeRepeatedReading(Read);
			}
		},
		[WeakSelf, SessionId](const FFlockError& Failure)
		{
			if (const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin())
			{
				Self->ConnectionWaitStopped(SessionId, Failure);
			}
		});
}

void FFlockMultiplayerSessions::StopReadingForConnection()
{
	if (ConnectionWait.IsValid())
	{
		ConnectionWait->Stop();
		ConnectionWait.Reset();
	}
}

void FFlockMultiplayerSessions::ConnectionWaitStopped(const FString& SessionId, const FFlockError& Failure)
{
	UFlockMultiplayerSession* HeldNow = Held.Get();
	if (HeldNow == nullptr || !HeldNow->Id.Equals(SessionId, ESearchCase::CaseSensitive))
	{
		return;
	}
	if (IsOutOfTheSession(Failure))
	{
		LearnWhyItEnded(HeldNow, nullptr, []() {});
		return;
	}
	Logger->LogWarning(FString::Printf(TEXT("The session is no longer read while waiting for the host's address: %s. The heartbeats still "
		"bring it, about every 20 s."), *Failure.Message));
}

void FFlockMultiplayerSessions::EndIfSignInEnded()
{
	UFlockMultiplayerSession* HeldNow = Held.Get();
	if (HeldNow != nullptr && HeldNow->SignInNumber != CurrentSignInNumber())
	{
		MarkEnded(HeldNow, FlockMultiplayerSessionEndReasons::SignedOut);
	}
}

bool FFlockMultiplayerSessions::LeaveForQuit()
{
	UFlockMultiplayerSession* HeldNow = Held.Get();
	if (HeldNow == nullptr || HeldNow->HasEnded())
	{
		return false;
	}
	const int32 SignInNumber = HeldNow->SignInNumber;
	const FString SessionId = HeldNow->Id;
	MarkEnded(HeldNow, FlockMultiplayerSessionEndReasons::Left, /*bRaise*/ false);
	Requests->Leave(SignInNumber, SessionId, [](TFlockResult<FFlockMultiplayerSessionReading>) {});
	return true;
}

void FFlockMultiplayerSessions::StopForShutdown()
{
	bStopped = true;
	if (Travels.IsValid())
	{
		Travels->StopForShutdown();
	}
	if (JoinChecks.IsValid())
	{
		JoinChecks->StopForShutdown();
	}
	// Every wait ends cancelled, as every call does at shutdown, before the held session's end could answer it as ended.
	const TArray<FConnectionWaiterRef> Waiting = ConnectionWaiters;
	for (const FConnectionWaiterRef& Waiter : Waiting)
	{
		AnswerWait(Waiter, TFlockResult<UFlockMultiplayerSession*>::Fail(ShutDownError()));
	}
	StopReadingForConnection();
	StopReadingForRelayedPlayers();
	DirectAddresses->StopForShutdown();
	if (UFlockMultiplayerSession* HeldNow = Held.Get())
	{
		MarkEnded(HeldNow, FlockMultiplayerSessionEndReasons::SignedOut, /*bRaise*/ false);
	}
	StopHeartbeating();
}

FFlockRequestHandle FFlockMultiplayerSessions::Change(UFlockMultiplayerSession* Session,
	TFunction<FFlockRequestHandle(FFlockMultiplayerSessionRequests::FOnSession)> Send, FFlockOnMultiplayerSession OnComplete)
{
	FFlockError Refused;
	if (!IsRunning(Session, Refused))
	{
		AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Refused));
		return FFlockRequestHandle();
	}
	const int32 SignInNumber = Session->SignInNumber;
	const TWeakObjectPtr<UFlockMultiplayerSession> WeakSession = Session;
	const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
	const TSharedRef<FFlockRequestsInTurn> InTurn = FFlockRequestsInTurn::Make();
	Ask<FFlockMultiplayerSessionReading>(Session, InTurn, MoveTemp(Send),
		[WeakSelf, WeakSession, SignInNumber, OnComplete](TFlockResult<FFlockMultiplayerSessionReading> Changed)
		{
			const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(ShutDownError()));
				return;
			}
			if (!Changed.bSuccess)
			{
				AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Changed.Error));
				return;
			}
			Self->TakeAnswerOfChange(Changed.Value, SignInNumber, FString());
			AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Ok(WeakSession.Get()));
		});
	return InTurn->GetHandle();
}

void FFlockMultiplayerSessions::LearnWhyItEnded(UFlockMultiplayerSession* Session, const TSharedPtr<FFlockRequestsInTurn>& InTurn,
	TFunction<void()> Then)
{
	const int32 Number = ++ReadsSent;
	const int32 SignInNumber = Session->SignInNumber;
	const FString SessionId = Session->Id;
	const TWeakObjectPtr<UFlockMultiplayerSession> WeakSession = Session;
	const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
	auto SendRead = [&]()
	{
		return Requests->Read(SignInNumber, SessionId, [WeakSelf, WeakSession, Number, SignInNumber, Then](TFlockResult<FFlockMultiplayerSessionReading> Last)
		{
			if (const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin())
			{
				if (Last.bSuccess)
				{
					if (Number > Self->ReadsSettledThrough)
					{
						Self->ReadsSettledThrough = Number;
						Self->Take(Last.Value, SignInNumber, FString());
					}
				}
				else if (UFlockMultiplayerSession* Gone = WeakSession.Get())
				{
					// Not even readable any more, or the caller gave up: the seat was given up without a word, unless this game left it.
					Self->MarkEnded(Gone, Self->EndingOf(Gone, FlockMultiplayerSessionEndReasons::Dropped));
				}
			}
			Then();
		});
	};
	if (InTurn.IsValid())
	{
		InTurn->Send(SendRead);
	}
	else
	{
		SendRead();
	}
}

void FFlockMultiplayerSessions::ReadUntilNewest(int32 SignInNumber, const FString& PlayerId, const FString& SessionId,
	const TSharedRef<FFlockRequestsInTurn>& InTurn, int32 Attempt, FFlockOnMultiplayerSession OnComplete)
{
	const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
	InTurn->Send([&]()
	{
		return ReadNumbered(SignInNumber, SessionId,
			[WeakSelf, SignInNumber, PlayerId, SessionId, InTurn, Attempt, OnComplete](TFlockResult<FFlockNumberedSessionReading> Read)
			{
				const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin();
				if (!Self.IsValid())
				{
					AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(ShutDownError()));
					return;
				}
				if (!Read.bSuccess)
				{
					AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(Read.Error));
					return;
				}
				if (!Self->IsStillCurrent(SignInNumber))
				{
					AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(SessionCallNoLongerCurrent()));
					return;
				}
				// Reads are numbered for the held session. One of another session the player is out of cannot change what is held,
				// so the held session's heartbeats and changes landing first do not make it read again; it comes back as read.
				const UFlockMultiplayerSession* HeldWhenRead = Self->Held.Get();
				const bool bOfTheHeldSession = HeldWhenRead != nullptr && HeldWhenRead->Id.Equals(Read.Value.Session.Id, ESearchCase::CaseSensitive);
				if (Read.Value.bFound && !SessionId.IsEmpty() && !bOfTheHeldSession && !EndingIn(Read.Value.Session, PlayerId).IsEmpty())
				{
					AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Ok(Self->Take(Read.Value.Session, SignInNumber, PlayerId)));
					return;
				}
				if (Read.Value.Number > Self->ReadsSettledThrough)
				{
					Self->ReadsSettledThrough = Read.Value.Number;
					if (Read.Value.bFound)
					{
						AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Ok(Self->Take(Read.Value.Session, SignInNumber, PlayerId)));
						return;
					}
					// Seated nowhere: a session held until now is over, and a reading of it says why.
					Self->EndIfSignInEnded();
					if (UFlockMultiplayerSession* HeldNow = Self->Held.Get())
					{
						Self->LearnWhyItEnded(HeldNow, InTurn, [OnComplete]()
						{
							AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Ok(nullptr));
						});
						return;
					}
					AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Ok(nullptr));
					return;
				}
				if (Attempt == MostReadsWhileTheSessionChanges)
				{
					AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Network,
						TEXT("The session kept changing while it was being read"), TEXT("Read it again in a moment: it changes while players join and leave and while hosting moves."))));
					return;
				}
				if (InTurn->IsCancelled())
				{
					AnswerSessionCall(OnComplete, TFlockResult<UFlockMultiplayerSession*>::Fail(FFlockError::Make(EFlockErrorType::Cancelled,
						TEXT("Request cancelled"))));
					return;
				}
				Self->ReadUntilNewest(SignInNumber, PlayerId, SessionId, InTurn, Attempt + 1, OnComplete);
			});
	});
}

FFlockRequestHandle FFlockMultiplayerSessions::ReadNumbered(int32 SignInNumber, const FString& SessionId,
	TFunction<void(TFlockResult<FFlockNumberedSessionReading>)> OnRead)
{
	const int32 Number = ++ReadsSent;
	const auto ToRead = [Number, SignInNumber, OnRead](TFlockResult<FFlockMultiplayerSessionReading> Answered, bool bSeatedNowhereIsAnAnswer)
	{
		FFlockNumberedSessionReading Read;
		Read.Number = Number;
		Read.SignInNumber = SignInNumber;
		if (Answered.bSuccess)
		{
			Read.bFound = true;
			Read.Session = Answered.Value;
			OnRead(TFlockResult<FFlockNumberedSessionReading>::Ok(Read));
		}
		else if (bSeatedNowhereIsAnAnswer && Answered.Error.ErrorCode == EFlockErrorCode::MultiplayerSessionNotFound)
		{
			// Seated nowhere is an answer to "my session", not a failure.
			OnRead(TFlockResult<FFlockNumberedSessionReading>::Ok(Read));
		}
		else
		{
			OnRead(TFlockResult<FFlockNumberedSessionReading>::Fail(Answered.Error));
		}
	};
	if (SessionId.IsEmpty())
	{
		return Requests->ReadCurrent(SignInNumber, [ToRead](TFlockResult<FFlockMultiplayerSessionReading> Mine) { ToRead(Mine, true); });
	}
	return Requests->Read(SignInNumber, SessionId, [ToRead](TFlockResult<FFlockMultiplayerSessionReading> One) { ToRead(One, false); });
}

UFlockMultiplayerSession* FFlockMultiplayerSessions::TakeAnswerOfChange(const FFlockMultiplayerSessionReading& Reading, int32 SignInNumber,
	const FString& PlayerId)
{
	// A late answer from a sign-in that ended settles nothing of the next player's.
	if (SignInNumber == CurrentSignInNumber())
	{
		ReadsSettledThrough = ReadsSent;
	}
	return Take(Reading, SignInNumber, PlayerId);
}

UFlockMultiplayerSession* FFlockMultiplayerSessions::Take(const FFlockMultiplayerSessionReading& Reading, int32 SignInNumber,
	const FString& PlayerId)
{
	// A session from a sign-in that ended is over first; that sign-in's late readings are older than any newer one.
	EndIfSignInEnded();
	if (SignInNumber != CurrentSignInNumber())
	{
		return nullptr;
	}

	UFlockMultiplayerSession* HeldNow = Held.Get();
	if (HeldNow != nullptr && HeldNow->Id.Equals(Reading.Id, ESearchCase::CaseSensitive))
	{
		// The server's epochs only grow, so a reading with a lower one was taken before the one held and changes nothing.
		if (Reading.HostEpoch < HeldNow->HostEpoch || Reading.ConnectionEpoch < HeldNow->ConnectionEpoch)
		{
			return HeldNow;
		}
		const FString Ending = EndingIn(Reading, HeldNow->PlayerId);
		if (Ending.IsEmpty())
		{
			// The heartbeat follows the reading before anyone hears of it, so a handler finds the SDK's own state settled too.
			const FFlockMultiplayerSessionChanges Changes = HeldNow->TakeReading(Reading);
			KeepHeartbeating();
			// A host taking players through its relay lets in each seated player's relay address the reading names.
			OpenTheRelayToSeatedPlayers(HeldNow);
			const TStrongObjectPtr<UFlockMultiplayerSession> KeepWhileRaising(HeldNow);
			HeldNow->RaiseChanges(Changes);
			// The game hears the change first, then a wait for the host's address is answered.
			SettleWaits();
		}
		else
		{
			HeldNow->TakeFinalReading(Reading);
			MarkEnded(HeldNow, EndingOf(HeldNow, Ending));
		}
		return HeldNow;
	}
	// A heartbeat or a reading to learn why never takes up another session: only a call the game made does.
	if (PlayerId.IsEmpty())
	{
		return nullptr;
	}

	UFlockMultiplayerSession* Taken = NewObject<UFlockMultiplayerSession>(GetTransientPackage());
	Taken->Begin(AsShared(), AuthSession, SignInNumber, PlayerId, Reading);
	const FString Ended = EndingIn(Reading, PlayerId);
	if (!Ended.IsEmpty())
	{
		// A session the player is not seated in any more: handed back as it ended, never held.
		Taken->MarkEnded(Ended, /*bRaise*/ false);
		return Taken;
	}
	// Hosting or joining gives up any other seat, so the session held until now is over at once.
	if (HeldNow != nullptr)
	{
		MarkEnded(HeldNow, FlockMultiplayerSessionEndReasons::MovedToAnotherSession);
	}
	Held.Reset(Taken);
	KeepHeartbeating();
	return Taken;
}

FString FFlockMultiplayerSessions::EndingIn(const FFlockMultiplayerSessionReading& Reading, const FString& PlayerId)
{
	// A session that is over gives its own reason first: closing one marks every seat left at the same moment, so a seat cannot
	// say who left first.
	if (Reading.Status.Equals(FlockMultiplayerSessionWireValues::SessionEnded, ESearchCase::CaseSensitive))
	{
		return Reading.EndedReason.IsEmpty() ? FString(FlockMultiplayerSessionEndReasons::Left) : Reading.EndedReason;
	}
	if (const FFlockMultiplayerSessionSeat* Seat = Reading.FindSeat(PlayerId))
	{
		if (Seat->Status.Equals(FlockMultiplayerSessionWireValues::SeatJoined, ESearchCase::CaseSensitive))
		{
			return FString();
		}
		return Seat->Status.Equals(FlockMultiplayerSessionWireValues::SeatDropped, ESearchCase::CaseSensitive)
			? FString(FlockMultiplayerSessionEndReasons::Dropped) : FString(FlockMultiplayerSessionEndReasons::Left);
	}
	return FlockMultiplayerSessionEndReasons::Left;
}

void FFlockMultiplayerSessions::ForgetLeaveOf(const FString& SessionId)
{
	if (LeavingSessionId.Equals(SessionId, ESearchCase::CaseSensitive))
	{
		LeavingSessionId.Reset();
	}
}

FString FFlockMultiplayerSessions::EndingOf(const UFlockMultiplayerSession* Session, const FString& Found) const
{
	return Session->Id.Equals(LeavingSessionId, ESearchCase::CaseSensitive) ? FString(FlockMultiplayerSessionEndReasons::Left) : Found;
}

void FFlockMultiplayerSessions::MarkEnded(UFlockMultiplayerSession* Session, const FString& Reason, bool bRaise)
{
	if (!Session->EndReasonSet.IsEmpty())
	{
		return;
	}
	// A session whose sign-in ended reads as signed out from then on, whatever answer lands after.
	const FString Final = Session->SignInNumber != CurrentSignInNumber() ? FString(FlockMultiplayerSessionEndReasons::SignedOut) : Reason;
	// Kept alive through the end's handlers, which may drop the game's own hold on it.
	const TStrongObjectPtr<UFlockMultiplayerSession> KeepWhileEnding(Session);
	if (Held.Get() == Session)
	{
		Held.Reset();
		StopHeartbeating();
	}
	// The session owns its relay: given back before the game hears the session ended.
	CloseRelayOf(Session);
	Session->MarkEnded(Final, bRaise);
	SettleWaits();
}

FFlockRequestHandle FFlockMultiplayerSessions::OpenRelay(UFlockMultiplayerSession* Session, const FString& Server, FOnRelayOpened OnOpened)
{
	FFlockError Refused;
	if (!IsRunning(Session, Refused))
	{
		// Refused as every call on an ended session is, with that refusal's own fix.
		FFlockRelayFailure Failure;
		Failure.Reason = FlockRelayFailureReasons::NotInSession;
		Failure.Error = Refused;
		Failure.Error.Operation = TEXT("Open the relay");
		OnOpened(TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Fail(Failure));
		return FFlockRequestHandle();
	}
	// Only the held session draws logins against the player's seat; a session ending is no longer held while its relay's
	// callers are told, so one of them asking again from its answer opens nothing its end would not close.
	if (Session != Held.Get())
	{
		OnOpened(TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Fail(FFlockRelayFailure::Of(FlockRelayFailureReasons::NotInSession,
			TEXT("This session is ending, or is not the one the player is seated in now"))));
		return FFlockRequestHandle();
	}
	if (Relay.IsValid() && !HasFailed(*Relay) && !IsOn(*Relay, Server))
	{
		if (!Relay->bDone)
		{
			// Opening on another server: it makes way once its opening has ended, and this call starts again then.
			const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
			const TWeakObjectPtr<UFlockMultiplayerSession> WeakSession = Session;
			const TSharedRef<FFlockCancelToken> Token = MakeShared<FFlockCancelToken>();
			const TSharedRef<FFlockRequestHandle> Waiting = MakeShared<FFlockRequestHandle>();
			Token->Canceller = [Waiting]() { Waiting->Cancel(); };
			*Waiting = WaitForRelay(Relay.ToSharedRef(), [WeakSelf, WeakSession, Server, OnOpened, Token, Waiting](TFlockRelayResult<TSharedPtr<FFlockRelayConnection>> Result) mutable
			{
				const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin();
				if (Token->bCancelled || !Self.IsValid() || !WeakSession.IsValid())
				{
					OnOpened(Token->bCancelled ? Result
						: TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Fail(FFlockRelayFailure::Cancelled(TEXT("Flock shut down while the relay opened"))));
					return;
				}
				*Waiting = Self->OpenRelay(WeakSession.Get(), Server, MoveTemp(OnOpened));
			});
			return FFlockRequestHandle(Token);
		}
		// Open on another server: players reach the host only from the server it is on, so this one makes way.
		Relay->Connection->Close();
		Relay.Reset();
	}
	if (!Relay.IsValid() || HasFailed(*Relay))
	{
		StartRelay(Session, Server);
	}
	return WaitForRelay(Relay.ToSharedRef(), MoveTemp(OnOpened));
}

FFlockRequestHandle FFlockMultiplayerSessions::WaitForRelay(const FRelayOpeningRef& Opening, FOnRelayOpened OnOpened)
{
	if (Opening->bDone)
	{
		OnOpened(Opening->Result);
		return FFlockRequestHandle();
	}
	const FRelayWaiterRef Waiter = MakeShared<FRelayWaiter>();
	Waiter->OnOpened = MoveTemp(OnOpened);
	Opening->Waiters.Add(Waiter);
	const TSharedRef<FFlockCancelToken> Token = MakeShared<FFlockCancelToken>();
	const TWeakPtr<FRelayOpening> WeakOpening = Opening;
	const TWeakPtr<FRelayWaiter> WeakWaiter = Waiter;
	// A caller that gives up leaves the opening to the others.
	Token->Canceller = [WeakOpening, WeakWaiter]()
	{
		const TSharedPtr<FRelayOpening> Pinned = WeakOpening.Pin();
		const TSharedPtr<FRelayWaiter> PinnedWaiter = WeakWaiter.Pin();
		if (Pinned.IsValid() && PinnedWaiter.IsValid() && Pinned->Waiters.Remove(PinnedWaiter.ToSharedRef()) > 0 && PinnedWaiter->OnOpened)
		{
			PinnedWaiter->OnOpened(TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Fail(
				FFlockRelayFailure::Cancelled(TEXT("The wait for the session's relay was cancelled"))));
		}
	};
	return FFlockRequestHandle(Token);
}

void FFlockMultiplayerSessions::TellARelayLost()
{
	if (!Relay.IsValid() || Relay->bToldLost || !Relay->bDone || !Relay->Result.bSuccess || !Relay->Connection.IsValid())
	{
		return;
	}
	const FFlockRelayConnection& Connection = *Relay->Connection;
	// A relay the SDK closed (the session's end, a leave, another server) stops with no reason; only a failure is a loss.
	if (!Connection.HasStopped() || Connection.GetFailureReason().IsEmpty())
	{
		return;
	}
	Relay->bToldLost = true;
	// The relay is the held session's: its end, or another session held, closes it and forgets it first (CloseRelayOf). Told on
	// the frame the relay's own warning said why; the next opening (Travel To Session again) reserves another.
	if (UFlockMultiplayerSession* Session = Held.Get())
	{
		Session->RaiseRelayLost(Connection.GetFailureReason());
	}
}

void FFlockMultiplayerSessions::TellTheHostMovedOnTheRelay(UFlockMultiplayerSession* Session)
{
	if (Session == nullptr || Session != Held.Get() || Session->HasEnded())
	{
		return;
	}
	Logger->LogWarning(TEXT("The host's address on Flock's relay changed (its relay was lost and it opened another), so this player's connection to it "
		"through the relay is gone. On Relay Lost is raised: call Travel To Session again, which connects to the new address."));
	Session->RaiseRelayLost(FlockRelayFailureReasons::Lost);
}

void FFlockMultiplayerSessions::StartRelay(UFlockMultiplayerSession* Session, const FString& Server)
{
	if (Relay.IsValid() && Relay->Connection.IsValid())
	{
		Relay->Connection->Close();
	}
	const FRelayOpeningRef Opening = MakeShared<FRelayOpening>();
	Opening->SessionId = Session->Id;
	Opening->SignInNumber = Session->SignInNumber;
	Opening->ServerAsked = Server;
	Relay = Opening;
	const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
	const TWeakPtr<FRelayOpening> WeakOpening = Opening;
	// Kept so the session's end cancels it: a retry waiting then never goes out for the session that ended.
	Opening->LoginsRead = Requests->RelayLogins(Opening->SignInNumber, Opening->SessionId,
		[WeakSelf, WeakOpening](TFlockResult<FFlockRelayLoginList> Answered)
		{
			const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin();
			const TSharedPtr<FRelayOpening> Pinned = WeakOpening.Pin();
			if (Self.IsValid() && Pinned.IsValid())
			{
				Self->RelayLoginsAnswered(Pinned.ToSharedRef(), Answered);
			}
		});
}

void FFlockMultiplayerSessions::RelayLoginsAnswered(const FRelayOpeningRef& Opening, const TFlockResult<FFlockRelayLoginList>& Answered)
{
	if (!Answered.bSuccess)
	{
		FinishRelay(Opening, TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Fail(RelayLoginsRefused(Answered.Error)));
		return;
	}
	if (!IsStillCurrent(Opening->SignInNumber))
	{
		FinishRelay(Opening, TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Fail(
			FFlockRelayFailure::Cancelled(TEXT("The player signed out or another player signed in while the relay opened"))));
		return;
	}
	if (Answered.Value.bRelayPaused)
	{
		FinishRelay(Opening, TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Fail(FFlockRelayFailure::Of(FlockRelayFailureReasons::Paused,
			TEXT("Flock paused this studio's relay until its bill is paid"))));
		return;
	}
	TArray<FFlockRelayLogin> Logins = FFlockRelayLogin::InAnswer(Answered.Value);
	if (Logins.Num() == 0)
	{
		FinishRelay(Opening, TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Fail(FFlockRelayFailure::Of(FlockRelayFailureReasons::NotOffered,
			TEXT("Flock listed no relay for this game: it is switched off for the game in the Flock dashboard, or no relay is set up"))));
		return;
	}
	if (!Opening->ServerAsked.IsEmpty())
	{
		// Players reach the host only from the relay server it is on, since only that server's addresses share the IP it opened to.
		const FString Asked = Opening->ServerAsked;
		Logins.RemoveAll([&Asked](const FFlockRelayLogin& Login) { return !Login.ToString().Equals(Asked, ESearchCase::IgnoreCase); });
		if (Logins.Num() == 0)
		{
			FinishRelay(Opening, TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Fail(FFlockRelayFailure::Of(FlockRelayFailureReasons::ServerNotListed,
				FString::Printf(TEXT("Flock does not list the relay server the host uses (%s) for this player"), *Asked))));
			return;
		}
	}
	Opening->Logins = MoveTemp(Logins);
	// Looked up once, while the relays open: a direct link learns its own address from them, since a relay server may answer
	// few of a device's ports.
	for (const FFlockRelayStunServer& Server : FFlockRelayStunServer::InAnswer(Answered.Value))
	{
		Opening->StunServers.Add({ FFlockNameLookup::Start(Server.Host, RelayTiming.NameLookUpForTesting), Server.Port });
	}
	TryNextRelay(Opening);
}

void FFlockMultiplayerSessions::TryNextRelay(const FRelayOpeningRef& Opening)
{
	if (!Opening->Logins.IsValidIndex(Opening->NextLogin))
	{
		FinishRelay(Opening, TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Fail(Opening->First.Get(FFlockRelayFailure())));
		return;
	}
	const FFlockRelayLogin Login = Opening->Logins[Opening->NextLogin++];
	const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
	const TWeakPtr<FRelayOpening> WeakOpening = Opening;
	Opening->Connection = FFlockRelayConnection::Open(Login, Opening->StunServers, RelayTiming, Logger,
		[WeakSelf, WeakOpening](TFlockRelayResult<TSharedPtr<FFlockRelayConnection>> Result)
		{
			const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin();
			const TSharedPtr<FRelayOpening> Pinned = WeakOpening.Pin();
			if (!Self.IsValid() || !Pinned.IsValid())
			{
				return;
			}
			// Closed by the session's end: answered Cancelled, at once, from inside the close.
			if (Result.bSuccess || Result.Failure.IsCancelled())
			{
				Self->FinishRelay(Pinned.ToSharedRef(), Result);
				return;
			}
			if (!Pinned->First.IsSet())
			{
				Pinned->First = Result.Failure;
			}
			Self->TryNextRelay(Pinned.ToSharedRef());
		});
}

void FFlockMultiplayerSessions::FinishRelay(const FRelayOpeningRef& Opening, const TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>& Result)
{
	if (Opening->bDone)
	{
		return;
	}
	Opening->bDone = true;
	Opening->Result = Result;
	const TArray<FRelayWaiterRef> Answering = MoveTemp(Opening->Waiters);
	Opening->Waiters.Reset();
	for (const FRelayWaiterRef& Waiter : Answering)
	{
		if (Waiter->OnOpened)
		{
			Waiter->OnOpened(Result);
		}
	}
}

void FFlockMultiplayerSessions::CloseRelayOf(const UFlockMultiplayerSession* Session)
{
	if (!Relay.IsValid() || Session == nullptr || !Relay->SessionId.Equals(Session->Id, ESearchCase::CaseSensitive))
	{
		return;
	}
	const FRelayOpeningRef Closing = Relay.ToSharedRef();
	Relay.Reset();
	// Every caller still waiting hears why first; what the cancel and the close answer after it changes nothing.
	FinishRelay(Closing, TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Fail(FFlockRelayFailure::Cancelled(TEXT("The session ended while its relay opened"))));
	Closing->LoginsRead.Cancel();
	if (Closing->Connection.IsValid())
	{
		Closing->Connection->Close();
	}
}

bool FFlockMultiplayerSessions::HasFailed(const FRelayOpening& Opening)
{
	return Opening.bDone && (!Opening.Result.bSuccess || !Opening.Result.Value.IsValid() || Opening.Result.Value->HasStopped());
}

bool FFlockMultiplayerSessions::IsOn(const FRelayOpening& Opening, const FString& Server)
{
	return Server.IsEmpty() || Opening.ServerAsked.Equals(Server, ESearchCase::IgnoreCase)
		|| (Opening.bDone && Opening.Result.bSuccess && Opening.Result.Value.IsValid() && Opening.Result.Value->GetServer().Equals(Server, ESearchCase::IgnoreCase));
}

FFlockRelayFailure FFlockMultiplayerSessions::RelayLoginsRefused(const FFlockError& Failure)
{
	if (Failure.Type == EFlockErrorType::Cancelled)
	{
		return FFlockRelayFailure::Cancelled(Failure.Message);
	}
	// Which allowance ran out, then the server's own words when it gave them.
	auto Cause = [&Failure](const TCHAR* Allowance, const TCHAR* Otherwise)
	{
		return FString::Printf(TEXT("Flock refused relay logins %s: %s"), Allowance, Failure.ServerMessage.IsEmpty() ? Otherwise : *Failure.ServerMessage);
	};
	if (Failure.ErrorCode == EFlockErrorCode::MultiplayerMintRateLimited)
	{
		return FFlockRelayFailure::FromFlock(FlockRelayFailureReasons::TooManyLogins,
			Cause(TEXT("for this player this minute"), TEXT("more than 12 were asked for in a minute")), Failure);
	}
	if (Failure.ErrorCode == EFlockErrorCode::MultiplayerSeatMintLimit)
	{
		return FFlockRelayFailure::FromFlock(FlockRelayFailureReasons::TooManyLogins,
			Cause(TEXT("for this player's seat in the session"), TEXT("the seat has drawn the most one seat may")), Failure);
	}
	if (IsOutOfTheSession(Failure))
	{
		return FFlockRelayFailure::FromFlock(FlockRelayFailureReasons::NotInSession,
			TEXT("Flock refused relay logins: the player has no seat in this session"), Failure);
	}
	if (Failure.StatusCode == 0)
	{
		return FFlockRelayFailure::FromFlock(FlockRelayFailureReasons::FlockUnreachable,
			FString::Printf(TEXT("Flock did not answer the request for relay logins: %s"), *Failure.Message), Failure);
	}
	return FFlockRelayFailure::FromFlock(FlockRelayFailureReasons::LoginsRefused, FString::Printf(TEXT("Flock refused relay logins: %s"), *Failure.Message),
		Failure);
}

void FFlockMultiplayerSessions::KeepHeartbeating()
{
	UFlockMultiplayerSession* HeldNow = Held.Get();
	if (HeldNow == nullptr || bStopped)
	{
		return;
	}
	if (Heartbeat.IsValid() && Heartbeat->IsRunning() && Heartbeat->GetIntervalSeconds() == HeldNow->HeartbeatIntervalSeconds)
	{
		return;
	}
	StopHeartbeating();
	const FString SessionId = HeldNow->Id;
	const int32 SignInNumber = HeldNow->SignInNumber;
	const TWeakPtr<FFlockMultiplayerSessions> WeakSelf = AsShared();
	Heartbeat = RepeatingCalls->Start<FFlockNumberedSessionReading>(HeartbeatCallName, HeldNow->HeartbeatIntervalSeconds,
		[WeakSelf, SignInNumber, SessionId](TFunction<void(TFlockResult<FFlockNumberedSessionReading>)> OnResult) -> FFlockRequestHandle
		{
			if (const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin())
			{
				return Self->HeartbeatNumbered(SignInNumber, SessionId, MoveTemp(OnResult));
			}
			OnResult(TFlockResult<FFlockNumberedSessionReading>::Fail(ShutDownError()));
			return FFlockRequestHandle();
		},
		[WeakSelf](const FFlockNumberedSessionReading& Read)
		{
			if (const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin())
			{
				Self->TakeRepeatedReading(Read);
			}
		},
		[WeakSelf, SessionId](const FFlockError& Failure)
		{
			if (const TSharedPtr<FFlockMultiplayerSessions> Self = WeakSelf.Pin())
			{
				Self->HeartbeatStopped(SessionId, Failure);
			}
		});
}

void FFlockMultiplayerSessions::StopHeartbeating()
{
	if (Heartbeat.IsValid())
	{
		Heartbeat->Stop();
		Heartbeat.Reset();
	}
}

FFlockRequestHandle FFlockMultiplayerSessions::HeartbeatNumbered(int32 SignInNumber, const FString& SessionId,
	TFunction<void(TFlockResult<FFlockNumberedSessionReading>)> OnRead)
{
	const int32 Number = ++ReadsSent;
	return Requests->Heartbeat(SignInNumber, SessionId, [Number, SignInNumber, OnRead](TFlockResult<FFlockMultiplayerSessionReading> Answered)
	{
		if (!Answered.bSuccess)
		{
			OnRead(TFlockResult<FFlockNumberedSessionReading>::Fail(Answered.Error));
			return;
		}
		FFlockNumberedSessionReading Read;
		Read.Number = Number;
		Read.SignInNumber = SignInNumber;
		Read.bFound = true;
		Read.Session = Answered.Value;
		OnRead(TFlockResult<FFlockNumberedSessionReading>::Ok(Read));
	});
}

void FFlockMultiplayerSessions::TakeRepeatedReading(const FFlockNumberedSessionReading& Read)
{
	if (Read.Number <= ReadsSettledThrough)
	{
		return;
	}
	ReadsSettledThrough = Read.Number;
	Take(Read.Session, Read.SignInNumber, FString());
}

void FFlockMultiplayerSessions::HeartbeatStopped(const FString& SessionId, const FFlockError& Failure)
{
	UFlockMultiplayerSession* HeldNow = Held.Get();
	if (HeldNow == nullptr || !HeldNow->Id.Equals(SessionId, ESearchCase::CaseSensitive))
	{
		return;
	}
	if (IsOutOfTheSession(Failure))
	{
		LearnWhyItEnded(HeldNow, nullptr, []() {});
		return;
	}
	Logger->LogWarning(FString::Printf(TEXT("The session is no longer kept seated: %s. The server gives the seat up when it stops hearing from it; "
		"GetMySession or GetSession starts the heartbeats again."), *Failure.Message));
}

bool FFlockMultiplayerSessions::RequireSignedIn(int32& OutSignInNumber, FString& OutPlayerId, FFlockError& OutError) const
{
	if (!AuthSession->IsAuthenticated())
	{
		OutError = FFlockError::MakeWithFix(EFlockErrorType::Auth, TEXT("No player is signed in"), FFlockErrorHints::SignInFirst());
		return false;
	}
	// The player id is what tells a call's reading from a heartbeat's (Take), so a sign-in that names no player sends nothing.
	OutPlayerId = AuthSession->GetPlayerId();
	if (OutPlayerId.IsEmpty())
	{
		OutError = FFlockError::MakeWithFix(EFlockErrorType::Auth, TEXT("The sign-in names no player: its token carries no player id"), TEXT("Call Logout and sign the player in again with one of Flock's sign-in calls."));
		return false;
	}
	OutSignInNumber = CurrentSignInNumber();
	return true;
}

bool FFlockMultiplayerSessions::IsRunning(const UFlockMultiplayerSession* Session, FFlockError& OutError)
{
	if (Session == nullptr)
	{
		OutError = FFlockError::MakeWithFix(EFlockErrorType::Validation, TEXT("No session was given"), FFlockErrorHints::PassAHeldSession());
		return false;
	}
	if (Session->HasEnded())
	{
		OutError = FFlockError::MakeWithFix(EFlockErrorType::Validation, FString::Printf(TEXT("This session has ended (%s)"), *Session->GetEndReason()), FFlockErrorHints::FindAnotherSession());
		return false;
	}
	return true;
}
