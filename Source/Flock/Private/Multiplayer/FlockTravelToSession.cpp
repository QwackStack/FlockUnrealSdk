// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockTravelToSession.h"
#include "Multiplayer/FlockMultiplayerSession.h"
#include "Multiplayer/FlockMultiplayerSessions.h"

namespace
{
	FFlockError TravelRefused(const FString& Message)
	{
		return FFlockError::Make(EFlockErrorType::Validation, Message);
	}
}

FFlockTravels::FFlockTravels(const TWeakPtr<FFlockMultiplayerSessions>& InSessions, const TSharedRef<IFlockLogger>& InLogger)
	: Sessions(InSessions)
	, Logger(InLogger)
	, MakeGameTravel(FlockEngineTravel())
{
}

bool FFlockTravels::IsOnItsOwn(const FFlockGameNetState& State)
{
	return State.bHasWorld && !State.bChangingLevel && State.NetMode == NM_Standalone;
}

FFlockRequestHandle FFlockTravels::Travel(UFlockMultiplayerSession* Session, UObject* WorldContextObject,
	const FFlockTravelToSessionOptions& Options, FOnTravel OnComplete)
{
	const auto Refuse = [&OnComplete](const FString& Message)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockTravelToSessionResult>::Fail(TravelRefused(Message)));
		}
		return FFlockRequestHandle();
	};
	if (Session == nullptr)
	{
		return Refuse(TEXT("No session was given"));
	}
	if (Session->HasEnded())
	{
		return Refuse(FString::Printf(TEXT("This session has ended (%s). Host or join another, or get the player's current one with GetMySession."),
			*Session->GetEndReason()));
	}
	if (Current.IsValid())
	{
		return Refuse(TEXT("A travel to the session is already under way. Wait for it, or cancel it, before starting another."));
	}
	if (!Session->IsHost() && !(Options.WaitForHostSeconds > 0.f))
	{
		return Refuse(TEXT("Give Wait For Host Seconds a time to wait that is longer than zero."));
	}
	FString Refusal;
	const TSharedPtr<IFlockGameTravel> Game = MakeGameTravel ? MakeGameTravel(WorldContextObject, Refusal) : nullptr;
	if (!Game.IsValid())
	{
		return Refuse(Refusal);
	}
	if (!IsOnItsOwn(Game->Read()))
	{
		return Refuse(TEXT("This game is already connected to a host, hosting, or opening a level. Travel to a level of its own, not listening "
			"for players, before traveling to a session."));
	}

	const FTravelRef Travel = MakeShared<FTravel>();
	Travel->Session.Reset(Session);
	Travel->Game = Game;
	Travel->Options = Options;
	Travel->OnComplete = MoveTemp(OnComplete);
	Travel->Result.bIsHost = Session->IsHost();
	Current = Travel;

	const TWeakPtr<FTravel> WeakTravel = Travel;
	Game->WatchFailures([WeakTravel](const FString& Why)
	{
		const TSharedPtr<FTravel> Pinned = WeakTravel.Pin();
		if (Pinned.IsValid() && Pinned->Failure.IsEmpty())
		{
			Pinned->Failure = Why;
		}
	});
	const TSharedRef<FFlockCancelToken> Token = MakeShared<FFlockCancelToken>();
	const TWeakPtr<FFlockTravels> WeakSelf = AsShared();
	Token->Canceller = [WeakSelf, WeakTravel]()
	{
		const TSharedPtr<FFlockTravels> Self = WeakSelf.Pin();
		const TSharedPtr<FTravel> Pinned = WeakTravel.Pin();
		if (Self.IsValid() && Pinned.IsValid())
		{
			Self->Cancel(Pinned.ToSharedRef());
		}
	};
	if (Travel->Result.bIsHost)
	{
		StartHosting(Travel);
	}
	else
	{
		StartWaiting(Travel);
	}
	return FFlockRequestHandle(Token);
}

void FFlockTravels::StartHosting(const FTravelRef& Travel)
{
	Travel->Level = Travel->Options.HostLevel.IsEmpty() ? Travel->Game->CurrentLevel() : Travel->Options.HostLevel;
	Travel->Step = EStep::OpeningLevel;
	Travel->Failure.Reset();
	Travel->Game->MarkWorld();
	Travel->Game->OpenListening(Travel->Level);
}

void FFlockTravels::StartWaiting(const FTravelRef& Travel)
{
	const TSharedPtr<FFlockMultiplayerSessions> Owner = Sessions.Pin();
	if (!Owner.IsValid())
	{
		Fail(Travel, FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Flock shut down while traveling to the session")));
		return;
	}
	Travel->Step = EStep::WaitingForHost;
	const TWeakPtr<FFlockTravels> WeakSelf = AsShared();
	const TWeakPtr<FTravel> WeakTravel = Travel;
	const FFlockRequestHandle Waiting = Owner->WaitForConnection(Travel->Session.Get(), Travel->Options.WaitForHostSeconds,
		[WeakSelf, WeakTravel](TFlockResult<UFlockMultiplayerSession*> Waited)
		{
			const TSharedPtr<FFlockTravels> Self = WeakSelf.Pin();
			const TSharedPtr<FTravel> Pinned = WeakTravel.Pin();
			if (!Self.IsValid() || !Pinned.IsValid() || Pinned->Step != EStep::WaitingForHost)
			{
				return;
			}
			if (!Waited.bSuccess)
			{
				Self->Fail(Pinned.ToSharedRef(), Waited.Error);
				return;
			}
			Self->TookConnection(Pinned.ToSharedRef(), Waited.Value);
		});
	// A wait answered at once has moved the travel on already; the handle kept is the one on its way now.
	if (Travel->Step == EStep::WaitingForHost)
	{
		Travel->OnItsWay = Waiting;
	}
}

void FFlockTravels::TookConnection(const FTravelRef& Travel, UFlockMultiplayerSession* Session)
{
	if (Session != nullptr && Session->HasConnection())
	{
		Travel->Result.Connection = Session->GetConnection();
	}
	if (Session == nullptr || Session->HasEnded())
	{
		Complete(Travel, FlockTravelToSessionOutcomes::SessionEnded);
		return;
	}
	if (!Session->HasConnection())
	{
		Complete(Travel, FlockTravelToSessionOutcomes::HostNeverPublished);
		return;
	}
	if (!Session->GetConnection().Mode.Equals(FlockMultiplayerConnectionModes::Direct, ESearchCase::CaseSensitive))
	{
		Complete(Travel, FlockTravelToSessionOutcomes::NotDirect);
		return;
	}
	const TSharedPtr<FFlockMultiplayerSessions> Owner = Sessions.Pin();
	if (!Owner.IsValid())
	{
		Fail(Travel, FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Flock shut down while traveling to the session")));
		return;
	}
	Travel->Step = EStep::FindingAddress;
	const TWeakPtr<FFlockTravels> WeakSelf = AsShared();
	const TWeakPtr<FTravel> WeakTravel = Travel;
	const FFlockRequestHandle Finding = Owner->FindDirectAddress(Session, [WeakSelf, WeakTravel](TFlockResult<FFlockDirectAddress> Found)
	{
		const TSharedPtr<FFlockTravels> Self = WeakSelf.Pin();
		const TSharedPtr<FTravel> Pinned = WeakTravel.Pin();
		if (!Self.IsValid() || !Pinned.IsValid() || Pinned->Step != EStep::FindingAddress)
		{
			return;
		}
		if (!Found.bSuccess)
		{
			Self->Fail(Pinned.ToSharedRef(), Found.Error);
			return;
		}
		if (!Found.Value.IsFound())
		{
			Self->Logger->LogWarning(TEXT("The host published a direct connection with no usable IPv4 address and port, so this player cannot connect to it."));
			Self->Complete(Pinned.ToSharedRef(), FlockTravelToSessionOutcomes::CouldNotConnect);
			return;
		}
		Self->StartConnecting(Pinned.ToSharedRef(), Found.Value);
	});
	if (Travel->Step == EStep::FindingAddress)
	{
		Travel->OnItsWay = Finding;
	}
}

void FFlockTravels::StartConnecting(const FTravelRef& Travel, const FFlockDirectAddress& Address)
{
	if (!IsOnItsOwn(Travel->Game->Read()))
	{
		Fail(Travel, TravelRefused(TEXT("This game started hosting, connecting or opening a level while it waited for the host, so it does not travel "
			"to the session.")));
		return;
	}
	Travel->Result.Address = Address.Address;
	Travel->Result.Port = Address.Port;
	Travel->Step = EStep::Connecting;
	Travel->OnItsWay = FFlockRequestHandle();
	Travel->Failure.Reset();
	Travel->Game->MarkWorld();
	Travel->Game->TravelTo(Address.Address, Address.Port);
}

void FFlockTravels::Tick()
{
	if (!Current.IsValid())
	{
		return;
	}
	const FTravelRef Travel = Current.ToSharedRef();
	if (Travel->Step == EStep::OpeningLevel || Travel->Step == EStep::StoppingAfterCancel)
	{
		MoveOpeningOn(Travel, Travel->Game->Read());
	}
	else if (Travel->Step == EStep::Connecting)
	{
		MoveConnectingOn(Travel, Travel->Game->Read());
	}
}

void FFlockTravels::MoveOpeningOn(const FTravelRef& Travel, const FFlockGameNetState& State)
{
	// The engine settles first: a level it failed to open may still send the game back to its default level.
	if (State.bHasWorld && State.bChangingLevel)
	{
		return;
	}
	const bool bCancelled = Travel->Step == EStep::StoppingAfterCancel;
	if (!bCancelled && !Travel->Failure.IsEmpty())
	{
		Logger->LogWarning(FString::Printf(TEXT("The level %s did not open listening for players (%s), so nobody can join this game."),
			*Travel->Level, *Travel->Failure));
		Complete(Travel, FlockTravelToSessionOutcomes::CouldNotStart);
		return;
	}
	if (bCancelled)
	{
		// Nobody waits on a host the game gave up on: its level stays, on its own.
		if (State.ListeningPort > 0)
		{
			Travel->Game->StopListening();
		}
		Travel->Step = EStep::Done;
		Current.Reset();
		return;
	}
	if (!State.bInAnotherWorld || State.NetMode != NM_ListenServer || State.ListeningPort <= 0)
	{
		Logger->LogWarning(FString::Printf(TEXT("The level %s did not open listening for players on an IP address, so nobody can join this game "
			"directly. A game started with -nonetdriver does this, and so does one whose net driver uses Steam's sockets (OnlineSubsystemSteam's "
			"bUseSteamNetworking, on unless set false): such a game publishes its own connection with PublishConnection instead."), *Travel->Level));
		Complete(Travel, FlockTravelToSessionOutcomes::CouldNotStart);
		return;
	}
	const TSharedPtr<FFlockMultiplayerSessions> Owner = Sessions.Pin();
	if (!Owner.IsValid())
	{
		Fail(Travel, FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Flock shut down while traveling to the session")));
		return;
	}
	// The host listens before it publishes, on the port its net driver bound.
	Travel->Result.Port = State.ListeningPort;
	Travel->Step = EStep::Publishing;
	const TWeakPtr<FFlockTravels> WeakSelf = AsShared();
	const TWeakPtr<FTravel> WeakTravel = Travel;
	const FFlockRequestHandle Publishing = Owner->PublishDirectConnection(Travel->Session.Get(), State.ListeningPort,
		[WeakSelf, WeakTravel](TFlockResult<UFlockMultiplayerSession*> Published)
		{
			const TSharedPtr<FFlockTravels> Self = WeakSelf.Pin();
			const TSharedPtr<FTravel> Pinned = WeakTravel.Pin();
			if (!Self.IsValid() || !Pinned.IsValid() || Pinned->Step != EStep::Publishing)
			{
				return;
			}
			if (!Published.bSuccess)
			{
				// Nobody can reach a host that did not say where it is.
				Pinned->Game->StopListening();
				Self->Fail(Pinned.ToSharedRef(), Published.Error);
				return;
			}
			const UFlockMultiplayerSession* Session = Pinned->Session.Get();
			if (Session != nullptr && Session->HasConnection())
			{
				Pinned->Result.Connection = Session->GetConnection();
				Pinned->Result.Connection.Values.TryGetString(FlockDirectConnectionValues::Address, Pinned->Result.Address);
			}
			Self->Complete(Pinned.ToSharedRef(), FlockTravelToSessionOutcomes::StartedAsHost);
		});
	if (Travel->Step == EStep::Publishing)
	{
		Travel->OnItsWay = Publishing;
	}
}

void FFlockTravels::MoveConnectingOn(const FTravelRef& Travel, const FFlockGameNetState& State)
{
	// Whatever happened, the engine settles first: a connection it gave up is still being dropped when it says so (measured), and
	// it then loads the game's default level, so the completion runs only once the game is in the level it stays in.
	if (State.bHasWorld && State.bChangingLevel)
	{
		return;
	}
	if (Travel->Failure.IsEmpty() && State.bInAnotherWorld && State.NetMode == NM_Client)
	{
		// In the host's level; done once the host has let this player in.
		if (State.bLetInByHost)
		{
			Complete(Travel, FlockTravelToSessionOutcomes::Connected);
		}
		return;
	}
	// Back in a level of its own: the engine gave the connection up.
	const FString Why = Travel->Failure.IsEmpty() ? FString(TEXT("no reason given")) : Travel->Failure;
	Logger->LogWarning(FString::Printf(TEXT("Could not connect to the host at %s:%d (%s). Outside a LAN the host's port must be forwarded to it."),
		*Travel->Result.Address, Travel->Result.Port, *Why));
	Complete(Travel, FlockTravelToSessionOutcomes::CouldNotConnect);
}

void FFlockTravels::Cancel(const FTravelRef& Travel)
{
	if (Current != Travel || Travel->Step == EStep::Done || Travel->Step == EStep::StoppingAfterCancel)
	{
		return;
	}
	const EStep Was = Travel->Step;
	const FFlockError Cancelled = FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Request cancelled"));
	if (Was == EStep::OpeningLevel)
	{
		// The level opens anyway; once it has, it stops listening. The caller hears now, and no other travel starts meanwhile.
		Travel->Step = EStep::StoppingAfterCancel;
		FOnTravel OnComplete = MoveTemp(Travel->OnComplete);
		Travel->OnComplete = nullptr;
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockTravelToSessionResult>::Fail(Cancelled));
		}
		return;
	}
	Travel->Step = EStep::Done;
	Travel->OnItsWay.Cancel();
	if (Was == EStep::Publishing)
	{
		Travel->Game->StopListening();
	}
	else if (Was == EStep::Connecting)
	{
		const FFlockGameNetState State = Travel->Game->Read();
		// Still being made: given up, and the game stays in its level. Once in the host's level the game leaves by traveling on.
		if (State.bChangingLevel && !State.bInAnotherWorld)
		{
			Travel->Game->StopConnecting();
		}
	}
	Finish(Travel, TFlockResult<FFlockTravelToSessionResult>::Fail(Cancelled));
}

void FFlockTravels::StopForShutdown()
{
	if (!Current.IsValid())
	{
		return;
	}
	const FTravelRef Travel = Current.ToSharedRef();
	Travel->Step = EStep::Done;
	Travel->OnItsWay.Cancel();
	Finish(Travel, TFlockResult<FFlockTravelToSessionResult>::Fail(FFlockError::Make(EFlockErrorType::Cancelled,
		TEXT("Flock shut down while traveling to the session"))));
}

void FFlockTravels::Complete(const FTravelRef& Travel, const FString& Outcome)
{
	Travel->Result.Outcome = Outcome;
	Finish(Travel, TFlockResult<FFlockTravelToSessionResult>::Ok(Travel->Result));
}

void FFlockTravels::Fail(const FTravelRef& Travel, const FFlockError& Error)
{
	Finish(Travel, TFlockResult<FFlockTravelToSessionResult>::Fail(Error));
}

void FFlockTravels::Finish(const FTravelRef& Travel, const TFlockResult<FFlockTravelToSessionResult>& Result)
{
	Travel->Step = EStep::Done;
	if (Current == Travel)
	{
		Current.Reset();
	}
	// The engine's failure handlers go with it.
	Travel->Game.Reset();
	FOnTravel OnComplete = MoveTemp(Travel->OnComplete);
	Travel->OnComplete = nullptr;
	if (OnComplete)
	{
		OnComplete(Result);
	}
}
