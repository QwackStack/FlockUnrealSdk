// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockTravelToSession.h"
#include "Dom/JsonObject.h"
#include "Http/FlockErrorHints.h"
#include "Multiplayer/FlockJoinChecksOnHost.h"
#include "Multiplayer/FlockMultiplayerSession.h"
#include "Multiplayer/FlockMultiplayerSessions.h"
#include "Multiplayer/FlockSockets.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	FFlockError TravelRefused(const FString& Message, const FString& Fix)
	{
		return FFlockError::MakeWithFix(EFlockErrorType::Validation, Message, Fix);
	}

	FFlockError ShutDownWhileTraveling()
	{
		return FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Flock shut down while traveling to the session"));
	}
}

FString FFlockTravels::HostsNetworkAddressForTesting;

FFlockTravels::FFlockTravels(const TWeakPtr<FFlockMultiplayerSessions>& InSessions, const TSharedRef<IFlockLogger>& InLogger, TFunction<double()> InNow)
	: Sessions(InSessions)
	, Logger(InLogger)
	, Now(MoveTemp(InNow))
	, MakeGameTravel(FlockEngineTravel())
{
}

bool FFlockTravels::IsOnItsOwn(const FFlockGameNetState& State)
{
	return State.bHasWorld && !State.bChangingLevel && State.NetMode == NM_Standalone;
}

bool FFlockTravels::IsInTheHostsLevel(const FTravel& Travel, const FFlockGameNetState& State)
{
	// A client of the host it played the session with, not of a server of the game's own.
	return Travel.bFromTheSession && State.bHasWorld && State.NetMode == NM_Client && !Travel.FromDestination.IsEmpty()
		&& State.Destination.Equals(Travel.FromDestination, ESearchCase::IgnoreCase);
}

bool FFlockTravels::IsFreeToTravel(const FTravel& Travel, const FFlockGameNetState& State)
{
	// The engine closes a client's connection to its host as it sets off for another (UEngine::Browse), so a player of the session
	// goes on from its old host's level.
	return IsOnItsOwn(State) || (!State.bChangingLevel && IsInTheHostsLevel(Travel, State));
}

bool FFlockTravels::HostMovedOnTheRelay(const FWithTheHost& With, const UFlockMultiplayerSession& Session)
{
	// The same host on another relay address; a new host's address is On Host Changed's to tell.
	if (!With.Bridge.IsValid() || !Session.HasConnection() || !Session.GetHostPlayerId().Equals(With.HostPlayerId, ESearchCase::CaseSensitive)
		|| !Session.GetConnection().Mode.Equals(FlockMultiplayerConnectionModes::Relay, ESearchCase::CaseSensitive))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Values;
	FFlockRelayPeer Published;
	FString Server;
	return FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Session.GetConnection().Values.ToJsonString()), Values) && Values.IsValid()
		&& FFlockRelayPeer::TryReadPublished(*Values, Published, Server) && !(Published == With.Bridge->GetHost());
}

bool FFlockTravels::IsInTheHostsGame(const UFlockMultiplayerSession& Session, const FFlockGameNetState& State) const
{
	const FWithTheHost* With = WithTheHost.Get();
	return With != nullptr && With->Session.Get() == &Session && With->HostPlayerId.Equals(Session.GetHostPlayerId(), ESearchCase::CaseSensitive)
		&& State.bHasWorld && !State.bChangingLevel && State.NetMode == NM_Client && State.bLetInByHost
		&& State.Destination.Equals(With->Destination, ESearchCase::IgnoreCase) && !HostMovedOnTheRelay(*With, Session);
}

FString FFlockTravels::WhereOf(const FTravel& Travel)
{
	return Travel.Result.bThroughRelay
		? FString::Printf(TEXT("through Flock's relay at %s:%d"), *Travel.Result.Address, Travel.Result.Port)
		: FString::Printf(TEXT("at %s:%d"), *Travel.Result.Address, Travel.Result.Port);
}

FFlockRelayBridge::FPtr FFlockTravels::GetPlayersBridgeForTesting() const
{
	if (Current.IsValid() && Current->Bridge.IsValid())
	{
		return Current->Bridge;
	}
	return WithTheHost.IsValid() ? WithTheHost->Bridge : nullptr;
}

FFlockRequestHandle FFlockTravels::Travel(UFlockMultiplayerSession* Session, UObject* WorldContextObject,
	const FFlockTravelToSessionOptions& Options, FOnTravel OnComplete)
{
	const auto Refuse = [&OnComplete](const FString& Message, const FString& Fix)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockTravelToSessionResult>::Fail(TravelRefused(Message, Fix)));
		}
		return FFlockRequestHandle();
	};
	if (Session == nullptr)
	{
		return Refuse(TEXT("No session was given"), FFlockErrorHints::PassAHeldSession());
	}
	if (Session->HasEnded())
	{
		return Refuse(FString::Printf(TEXT("This session has ended (%s)"),
			*Session->GetEndReason()), FFlockErrorHints::FindAnotherSession());
	}
	if (Options.Relay != EFlockRelayUse::WhenNeeded && Options.Relay != EFlockRelayUse::Always && Options.Relay != EFlockRelayUse::Never)
	{
		return Refuse(FString::Printf(TEXT("Relay is %d, which is not a Flock relay use"), static_cast<int32>(Options.Relay)),
			TEXT("Set Relay to When Needed, Always or Never."));
	}
	FString Refusal;
	const TSharedPtr<IFlockGameTravel> Game = MakeGameTravel ? MakeGameTravel(WorldContextObject, Refusal) : nullptr;
	if (!Game.IsValid())
	{
		return Refuse(Refusal, TEXT("Pass an object in the game's world as the World Context, such as an actor or the game instance."));
	}
	// The check of joining players lasts as long as the host's level listens, past this travel: an engine side of its own, made now
	// since whether this device hosts is the session's when the travel begins (a host change may come meanwhile). A host that cannot
	// check its players does not open its level to them.
	const TSharedPtr<IFlockGameTravel> HostSide = MakeGameTravel(WorldContextObject, Refusal);
	if (!HostSide.IsValid())
	{
		return Refuse(Refusal, TEXT("Pass an object in the game's world as the World Context, such as an actor or the game instance."));
	}
	// In this session's networking, and where: with its host, brought there by the travel this one replaces, or still in the level of
	// the host it last played the session with (its relay lost, the SDK lets that host go at once).
	FString FromDestination;
	bool bFromTheSession = false;
	if (WithTheHost.IsValid() && WithTheHost->Session.Get() == Session)
	{
		bFromTheSession = true;
		FromDestination = WithTheHost->Destination;
	}
	else if (Current.IsValid() && Current->Session.Get() == Session)
	{
		bFromTheSession = true;
		FromDestination = Current->ConnectAddress.IsEmpty() ? FString() : FString::Printf(TEXT("%s:%d"), *Current->ConnectAddress, Current->ConnectPort);
	}
	else if (Session->GetId().Equals(LevelPlayedSessionId, ESearchCase::CaseSensitive))
	{
		bFromTheSession = true;
		FromDestination = LastHostDestination;
	}
	// A game with another session's host leaves it now. One with this session's host stays with it until this travel connects or
	// hosts, so its connection there ends the engine's own way.
	if (!(WithTheHost.IsValid() && WithTheHost->Session.Get() == Session))
	{
		LeaveTheHost();
	}

	const FTravelRef Travel = MakeShared<FTravel>();
	Travel->Step = EStep::Beginning;
	Travel->Session.Reset(Session);
	Travel->Game = Game;
	Travel->HostSide = HostSide;
	Travel->Options = Options;
	Travel->OnComplete = MoveTemp(OnComplete);
	Travel->Result.bIsHost = Session->IsHost();
	Travel->bFromTheSession = bFromTheSession;
	Travel->FromDestination = FromDestination;

	const TWeakPtr<FTravel> WeakTravel = Travel;
	Game->WatchFailures([WeakTravel](const FString& Kind, const FString& Words)
	{
		const TSharedPtr<FTravel> Pinned = WeakTravel.Pin();
		if (!Pinned.IsValid() || !Pinned->Failure.IsEmpty())
		{
			return;
		}
		Pinned->Failure = FString::Printf(TEXT("%s: %s"), *Kind, *Words);
		// A host turning a waiting player away fails the connection being made with the host's words; a host closing its
		// connections fails it the same way with the engine's own words, so only a refusal of a host's check counts (measured).
		Pinned->bRefused = Kind.Equals(TEXT("PendingConnectionFailure"), ESearchCase::CaseSensitive) && FlockHostRefusals::TryRead(Words, Pinned->RefusedReason);
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
	// One travel at a time, the newest taking over. It is in place before the others hear, so a travel started from their answer
	// replaces it in turn.
	const TSharedPtr<FTravel> Waiting = Next;
	const TSharedPtr<FTravel> Replaced = Current;
	Next = Travel;
	if (Waiting.IsValid())
	{
		// It had not begun: nothing of the engine's to leave.
		Complete(Waiting.ToSharedRef(), FlockTravelToSessionOutcomes::Restarted);
	}
	if (Replaced.IsValid())
	{
		Replaced->bRestarted = true;
		Replaced->Result.Outcome = FlockTravelToSessionOutcomes::Restarted;
		EndEarly(Replaced.ToSharedRef(), TFlockResult<FFlockTravelToSessionResult>::Ok(Replaced->Result));
	}
	BeginTheNext();
	return FFlockRequestHandle(Token);
}

void FFlockTravels::BeginTheNext()
{
	if (!Next.IsValid())
	{
		return;
	}
	const FTravelRef Travel = Next.ToSharedRef();
	Next.Reset();
	Current = Travel;
	const TWeakPtr<FFlockTravels> WeakSelf = AsShared();
	WhenSettled(Travel, [WeakSelf](const FTravelRef& Settled)
	{
		if (const TSharedPtr<FFlockTravels> Self = WeakSelf.Pin())
		{
			Self->Begin(Settled);
		}
	});
}

void FFlockTravels::WhenSettled(const FTravelRef& Travel, TFunction<void(const FTravelRef&)> Then)
{
	const FFlockGameNetState State = Travel->Game->Read();
	if (!(State.bHasWorld && State.bChangingLevel))
	{
		Then(Travel);
		return;
	}
	Travel->Step = EStep::Settling;
	Travel->SettleBy = Now() + SettleWithinSeconds;
	Travel->OnceSettled = MoveTemp(Then);
}

void FFlockTravels::MoveSettlingOn(const FTravelRef& Travel, const FFlockGameNetState& State)
{
	if (State.bHasWorld && State.bChangingLevel)
	{
		if (Now() >= Travel->SettleBy)
		{
			Travel->OnceSettled = nullptr;
			Fail(Travel, TravelRefused(FString::Printf(TEXT("This game was still opening a level or connecting to a host %g s on, so it does not travel "
				"to the session"), SettleWithinSeconds), TEXT("Let the level change the game started finish, then travel again.")));
		}
		return;
	}
	TFunction<void(const FTravelRef&)> Then = MoveTemp(Travel->OnceSettled);
	Travel->OnceSettled = nullptr;
	if (Then)
	{
		Then(Travel);
	}
}

void FFlockTravels::Begin(const FTravelRef& Travel)
{
	UFlockMultiplayerSession* Session = Travel->Session.Get();
	if (Session == nullptr || Session->HasEnded())
	{
		Complete(Travel, FlockTravelToSessionOutcomes::SessionEnded);
		return;
	}
	const TSharedPtr<FFlockMultiplayerSessions> Owner = Sessions.Pin();
	if (!Owner.IsValid())
	{
		Fail(Travel, ShutDownWhileTraveling());
		return;
	}
	// The part is the session's now: a host change read while the engine settled is taken.
	Travel->Result.bIsHost = Session->IsHost();
	const FFlockGameNetState State = Travel->Game->Read();
	const bool bHostingIt = Owner->GetJoinChecks().IsHosting(Session) && State.NetMode == NM_ListenServer && State.ListeningPort > 0;
	const bool bInTheHostsLevel = IsInTheHostsLevel(*Travel, State);
	if (Travel->Result.bIsHost)
	{
		// A player's bridge on the session's relay makes way for the host's.
		LeaveTheHost();
		if (bHostingIt)
		{
			KeepHosting(Travel, State.ListeningPort);
			return;
		}
		// A player that has become the host opens its level from the old host's (its connection goes with it).
		if (IsOnItsOwn(State) || bInTheHostsLevel)
		{
			StartHosting(Travel);
			return;
		}
	}
	else
	{
		if (!(Travel->Options.WaitForHostSeconds > 0.f))
		{
			Fail(Travel, TravelRefused(TEXT("Wait For Host Seconds must be longer than zero"),
				TEXT("Give it a time to wait, such as 30, so a player who is not the host waits for the host's address.")));
			return;
		}
		// Already in the game of the host the session names, on the address it published: called again (a level's Begin Play
		// run on every arriving player, say), it answers at once rather than reconnect.
		if (IsInTheHostsGame(*Session, State))
		{
			Travel->bAlreadyIn = true;
			Travel->Result = WithTheHost->Result;
			Complete(Travel, FlockTravelToSessionOutcomes::Connected);
			return;
		}
		if (bHostingIt)
		{
			// Hosting moved to another player: nobody joins this game any more (its check of joining players ends with the
			// listening), and it waits for the new host in its level, on its own.
			Travel->Game->StopListening();
			StartWaiting(Travel);
			return;
		}
		if (IsOnItsOwn(State) || bInTheHostsLevel)
		{
			StartWaiting(Travel);
			return;
		}
	}
	Fail(Travel, TravelRefused(TEXT("This game is already connected to a host, hosting, or opening a level"),
		TEXT("Travel to a level of its own, not listening for players, before traveling to a session.")));
}

// ---- the host ----

void FFlockTravels::StartHosting(const FTravelRef& Travel)
{
	// A player of the session that became its host plays on the level it played with the old host, though the engine may have
	// taken it back to its default level when that host went.
	const UFlockMultiplayerSession* Session = Travel->Session.Get();
	const bool bPlayedIt = Session != nullptr && !LevelPlayed.IsEmpty() && Session->GetId().Equals(LevelPlayedSessionId, ESearchCase::CaseSensitive)
		&& IsOnItsOwn(Travel->Game->Read());
	Travel->Level = !Travel->Options.HostLevel.IsEmpty() ? Travel->Options.HostLevel : bPlayedIt ? LevelPlayed : Travel->Game->CurrentLevel();
	Travel->Step = EStep::OpeningLevel;
	Travel->Failure.Reset();
	// Checking from before the level listens, so no player reaches it unchecked; it ends once the game no longer listens. With
	// Relay Always, from then on only players through the relay get in.
	const TSharedPtr<FFlockMultiplayerSessions> Owner = Sessions.Pin();
	if (Owner.IsValid() && Travel->HostSide.IsValid())
	{
		Owner->GetJoinChecks().StartHosting(Travel->Session.Get(), Travel->HostSide.ToSharedRef(), Travel->Options.Relay == EFlockRelayUse::Always);
	}
	Travel->Game->MarkWorld();
	Travel->Game->OpenListening(Travel->Level);
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
		// Nobody waits on a host the game gave up on: its level stays, on its own. (One a newer travel replaced is that one's.)
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
	OpenToPlayers(Travel, State.ListeningPort);
}

void FFlockTravels::OpenToPlayers(const FTravelRef& Travel, int32 Port)
{
	// The host listens before it publishes, on the port its net driver bound.
	Travel->Result.Port = Port;
	if (Travel->Options.Relay == EFlockRelayUse::Never)
	{
		PublishForPlayers(Travel, nullptr);
		return;
	}
	OpenRelayForPlayers(Travel);
}

void FFlockTravels::KeepHosting(const FTravelRef& Travel, int32 Port)
{
	Travel->bKeptHosting = true;
	Travel->Level = Travel->Game->CurrentLevel();
	// The players already in stay let in, by the check that let them in.
	OpenToPlayers(Travel, Port);
}

void FFlockTravels::OpenRelayForPlayers(const FTravelRef& Travel)
{
	const TSharedPtr<FFlockMultiplayerSessions> Owner = Sessions.Pin();
	if (!Owner.IsValid())
	{
		Fail(Travel, ShutDownWhileTraveling());
		return;
	}
	Travel->Step = EStep::OpeningRelay;
	// The relay is the session's: cancelling its opening, or an opening to an IP, answers only that caller and the request goes
	// on, so nothing of it is kept to cancel. A cancelled travel's step drops the answer.
	Travel->OnItsWay = FFlockRequestHandle();
	const TWeakPtr<FFlockTravels> WeakSelf = AsShared();
	const TWeakPtr<FTravel> WeakTravel = Travel;
	Owner->OpenRelay(Travel->Session.Get(), FString(),
		[WeakSelf, WeakTravel](TFlockRelayResult<TSharedPtr<FFlockRelayConnection>> Opened)
		{
			const TSharedPtr<FFlockTravels> Self = WeakSelf.Pin();
			const TSharedPtr<FTravel> Pinned = WeakTravel.Pin();
			if (!Self.IsValid() || !Pinned.IsValid() || Pinned->Step != EStep::OpeningRelay)
			{
				return;
			}
			if (!Opened.bSuccess || !Opened.Value.IsValid())
			{
				Self->RelayCouldNotOpen(Pinned.ToSharedRef(), Opened.Failure);
				return;
			}
			// Opened to the relay's own IP, which a relay that gives every address one IP shares with every player; a relay that
			// gives each address an IP of its own is opened to each seated player's as its seat names it.
			const TSharedRef<FFlockRelayConnection> Relay = Opened.Value.ToSharedRef();
			Pinned->Relay = Relay;
			Relay->OpenTo(Relay->GetAddress(), [WeakSelf, WeakTravel, Relay](TFlockRelayResult<bool> Open)
			{
				const TSharedPtr<FFlockTravels> Self = WeakSelf.Pin();
				const TSharedPtr<FTravel> Pinned = WeakTravel.Pin();
				if (!Self.IsValid() || !Pinned.IsValid() || Pinned->Step != EStep::OpeningRelay)
				{
					return;
				}
				if (!Open.bSuccess)
				{
					// A relay address no player can reach is given back rather than renewed for the whole session; the next travel opens another.
					Relay->Close();
					Self->RelayCouldNotOpen(Pinned.ToSharedRef(), Open.Failure);
					return;
				}
				const TSharedPtr<FFlockMultiplayerSessions> Owner = Self->Sessions.Pin();
				if (!Owner.IsValid())
				{
					Self->Fail(Pinned.ToSharedRef(), ShutDownWhileTraveling());
					return;
				}
				// Direct paths for its relayed players with When Needed alone: Always keeps every address hidden.
				const bool bGoesDirect = Pinned->Options.Relay == EFlockRelayUse::WhenNeeded;
				// A host that kept hosting keeps the bridge its relayed players came through, while that relay works.
				const FFlockRelayBridge::FPtr InUse = Owner->GetJoinChecks().GetRelayBridge();
				if (InUse.IsValid() && InUse->GetRelay() == Relay && Owner->GetJoinChecks().IsHosting(Pinned->Session.Get()))
				{
					InUse->SetGoesDirect(bGoesDirect);
					Self->PublishForPlayers(Pinned.ToSharedRef(), Relay);
					return;
				}
				const TFlockRelayResult<FFlockRelayBridge::FPtr> Made = FFlockRelayBridge::ForTheHost(Relay, Pinned->Result.Port, Self->Logger, bGoesDirect);
				if (!Made.bSuccess)
				{
					if (Made.Failure.Reason.IsEmpty())
					{
						// A host that kept its level keeps its players on it.
						if (!Pinned->bKeptHosting)
						{
							Pinned->Game->StopListening();
						}
						Self->Fail(Pinned.ToSharedRef(), Made.Failure.Error);
						return;
					}
					Self->RelayCouldNotOpen(Pinned.ToSharedRef(), Made.Failure);
					return;
				}
				// The hosting owns the bridge from here: it closes with the hosting, with the relay, and at shutdown.
				Owner->GetJoinChecks().UseRelayBridge(Pinned->Session.Get(), Made.Value.ToSharedRef());
				Self->PublishForPlayers(Pinned.ToSharedRef(), Relay);
			});
		});
}

void FFlockTravels::RelayCouldNotOpen(const FTravelRef& Travel, const FFlockRelayFailure& Failure)
{
	if (Failure.IsCancelled())
	{
		// The session ended while its relay opened, or Flock shut down: nobody can join a host that is not published.
		Travel->Game->StopListening();
		Fail(Travel, Failure.Error);
		return;
	}
	Travel->Result.RelayFailureReason = Failure.Reason;
	if (Travel->Options.Relay == EFlockRelayUse::Always)
	{
		Logger->LogWarning(FString::Printf(TEXT("Stopped hosting: every player comes through Flock's relay, and it could not be opened (%s)"),
			*Failure.Error.ReasonAndFix()));
		Travel->Game->StopListening();
		Complete(Travel, FlockTravelToSessionOutcomes::RelayFailed);
		return;
	}
	// A relay switched off for the game is the studio's choice; any other reason is worth saying.
	if (!Failure.Reason.Equals(FlockRelayFailureReasons::NotOffered, ESearchCase::CaseSensitive))
	{
		Logger->LogWarning(FString::Printf(TEXT("Players outside this device's network cannot join through Flock's relay (%s); those on its network still can."),
			*Failure.Error.ReasonAndFix()));
	}
	PublishForPlayers(Travel, nullptr);
}

void FFlockTravels::PublishForPlayers(const FTravelRef& Travel, const TSharedPtr<FFlockRelayConnection>& OnTheRelay)
{
	// The game may have stopped its level while the relay opened; a host that is gone is not published.
	const FFlockGameNetState State = Travel->Game->Read();
	if (!State.bInAnotherWorld || State.NetMode != NM_ListenServer || State.ListeningPort != Travel->Result.Port)
	{
		Logger->LogWarning(FString::Printf(TEXT("The level %s stopped listening for players while Flock's relay opened, so nothing was published."), *Travel->Level));
		Complete(Travel, FlockTravelToSessionOutcomes::CouldNotStart);
		return;
	}
	const TSharedPtr<FFlockMultiplayerSessions> Owner = Sessions.Pin();
	if (!Owner.IsValid())
	{
		Fail(Travel, ShutDownWhileTraveling());
		return;
	}
	Travel->Step = EStep::Publishing;
	const TWeakPtr<FFlockTravels> WeakSelf = AsShared();
	const TWeakPtr<FTravel> WeakTravel = Travel;
	const bool bThroughRelay = OnTheRelay.IsValid();
	const FFlockOnMultiplayerSession Published = [WeakSelf, WeakTravel, bThroughRelay](TFlockResult<UFlockMultiplayerSession*> Answer)
	{
		const TSharedPtr<FFlockTravels> Self = WeakSelf.Pin();
		const TSharedPtr<FTravel> Pinned = WeakTravel.Pin();
		if (!Self.IsValid() || !Pinned.IsValid() || Pinned->Step != EStep::Publishing)
		{
			return;
		}
		if (!Answer.bSuccess)
		{
			// Nobody can reach a host that did not say where it is; its level stops listening, which closes its relay bridge too.
			// A host that kept its level keeps its players on it (what it published before still stands).
			if (!Pinned->bKeptHosting)
			{
				Pinned->Game->StopListening();
			}
			Self->Fail(Pinned.ToSharedRef(), Answer.Error);
			return;
		}
		const UFlockMultiplayerSession* Session = Pinned->Session.Get();
		if (Session != nullptr && Session->HasConnection())
		{
			Pinned->Result.Connection = Session->GetConnection();
			Pinned->Result.Connection.Values.TryGetString(FlockDirectConnectionValues::Address, Pinned->Result.Address);
		}
		Pinned->Result.bThroughRelay = bThroughRelay;
		if (Session != nullptr)
		{
			Self->LevelPlayedSessionId = Session->GetId();
			Self->LevelPlayed = Pinned->Level;
		}
		Self->Complete(Pinned.ToSharedRef(), FlockTravelToSessionOutcomes::StartedAsHost);
	};
	const FFlockRequestHandle Publishing = bThroughRelay
		? Owner->PublishRelayConnection(Travel->Session.Get(), OnTheRelay.ToSharedRef(), Travel->Result.Port,
			Travel->Options.Relay == EFlockRelayUse::WhenNeeded, Published)
		: Owner->PublishDirectConnection(Travel->Session.Get(), Travel->Result.Port, Published);
	if (Travel->Step == EStep::Publishing)
	{
		Travel->OnItsWay = Publishing;
	}
}

// ---- a player ----

void FFlockTravels::StartWaiting(const FTravelRef& Travel)
{
	const TSharedPtr<FFlockMultiplayerSessions> Owner = Sessions.Pin();
	if (!Owner.IsValid())
	{
		Fail(Travel, ShutDownWhileTraveling());
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
	const FString& Mode = Session->GetConnection().Mode;
	const bool bHostIsDirect = Mode.Equals(FlockMultiplayerConnectionModes::Direct, ESearchCase::CaseSensitive);
	const bool bHostIsRelayed = Mode.Equals(FlockMultiplayerConnectionModes::Relay, ESearchCase::CaseSensitive);
	if (!bHostIsDirect && !bHostIsRelayed)
	{
		Complete(Travel, FlockTravelToSessionOutcomes::NotDirect);
		return;
	}
	if (bHostIsRelayed)
	{
		TSharedPtr<FJsonObject> Values;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Session->GetConnection().Values.ToJsonString()), Values) || !Values.IsValid()
			|| !FFlockRelayPeer::TryReadPublished(*Values, Travel->HostOnTheRelay, Travel->RelayServer) || Travel->RelayServer.IsEmpty())
		{
			Logger->LogWarning(TEXT("The host published a relay connection with no usable relay address, so this player cannot connect to it."));
			Complete(Travel, FlockTravelToSessionOutcomes::CouldNotConnect);
			return;
		}
		// A host that names direct addresses beside its relay one (When Needed) offers its relayed players a direct path; its bridge
		// is sent the bridges' own messages only when it says it reads them.
		FFlockPublishedDirectAddresses Published;
		Travel->bHostReadsBridgeMessages = FFlockRelayPeer::ReadsBridgeMessages(*Values);
		Travel->bHostOffersDirectPaths = FFlockDirectAddresses::ReadPublished(Session->GetConnection(), Published)
			&& !(Published.Address.IsEmpty() && Published.Lan.IsEmpty() && Published.Public.IsEmpty());
		if (Travel->Options.Relay == EFlockRelayUse::Always)
		{
			StartThroughTheRelay(Travel);
			return;
		}
		if (Travel->Options.Relay == EFlockRelayUse::WhenNeeded)
		{
			// Sharing the host's public address suggests its network, but a network may keep its devices apart (or many share one
			// address), so the relay follows a try that is not answered.
			FindHostsAddress(Travel, /*bThenTheRelay*/ true);
			return;
		}
		FindHostsAddress(Travel, /*bThenTheRelay*/ false);
		return;
	}
	if (Travel->Options.Relay == EFlockRelayUse::Always)
	{
		Logger->LogWarning(TEXT("The host takes no players through Flock's relay, and this player's Relay is Always, so it cannot connect."));
		Travel->Result.RelayFailureReason = FlockRelayFailureReasons::NotOfferedByTheHost;
		Complete(Travel, FlockTravelToSessionOutcomes::RelayFailed);
		return;
	}
	FindHostsAddress(Travel, /*bThenTheRelay*/ false);
}

void FFlockTravels::FindHostsAddress(const FTravelRef& Travel, bool bThenTheRelay)
{
	const TSharedPtr<FFlockMultiplayerSessions> Owner = Sessions.Pin();
	if (!Owner.IsValid())
	{
		Fail(Travel, ShutDownWhileTraveling());
		return;
	}
	Travel->Step = EStep::FindingAddress;
	const TWeakPtr<FFlockTravels> WeakSelf = AsShared();
	const TWeakPtr<FTravel> WeakTravel = Travel;
	const FFlockRequestHandle Finding = Owner->FindDirectAddressForTravel(Travel->Session.Get(),
		[WeakSelf, WeakTravel, bThenTheRelay](TFlockResult<FFlockDirectAddress> Found, bool bSharesTheHostsPublicAddress)
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
			if (bThenTheRelay)
			{
				// Only a player sharing the host's public address tries its network; everyone else goes through the relay at once.
				if (Found.Value.IsFound() && bSharesTheHostsPublicAddress)
				{
					Pinned->bTryingTheHostsNetwork = true;
					Self->StartConnecting(Pinned.ToSharedRef(), HostsNetworkAddressForTesting.IsEmpty() ? Found.Value.Address : HostsNetworkAddressForTesting,
						Found.Value.Port);
					return;
				}
				Self->StartThroughTheRelay(Pinned.ToSharedRef());
				return;
			}
			if (!Found.Value.IsFound())
			{
				const bool bRelayOnly = Pinned->Result.Connection.Mode.Equals(FlockMultiplayerConnectionModes::Relay, ESearchCase::CaseSensitive);
				Self->Logger->LogWarning(bRelayOnly
					? TEXT("The host takes players through Flock's relay only, and this player's Relay is Never, so it cannot connect.")
					: TEXT("The host published a direct connection with no usable IPv4 address and port, so this player cannot connect to it."));
				Self->Complete(Pinned.ToSharedRef(), FlockTravelToSessionOutcomes::CouldNotConnect);
				return;
			}
			Self->StartConnecting(Pinned.ToSharedRef(), Found.Value.Address, Found.Value.Port);
		});
	if (Travel->Step == EStep::FindingAddress)
	{
		Travel->OnItsWay = Finding;
	}
}

void FFlockTravels::StartThroughTheRelay(const FTravelRef& Travel)
{
	const TSharedPtr<FFlockMultiplayerSessions> Owner = Sessions.Pin();
	if (!Owner.IsValid())
	{
		Fail(Travel, ShutDownWhileTraveling());
		return;
	}
	Travel->Step = EStep::OpeningRelayToTheHost;
	Travel->bTryingTheHostsNetwork = false;
	Travel->Result.bThroughRelay = true;
	Travel->Result.Address = Travel->HostOnTheRelay.Ip();
	Travel->Result.Port = Travel->HostOnTheRelay.Port;
	// As for the host: nothing of the session's relay opening is kept to cancel.
	Travel->OnItsWay = FFlockRequestHandle();
	const TWeakPtr<FFlockTravels> WeakSelf = AsShared();
	const TWeakPtr<FTravel> WeakTravel = Travel;
	// Players reach the host only from the relay server it is on, since only that server's addresses share the IP it opened to.
	Owner->OpenRelay(Travel->Session.Get(), Travel->RelayServer,
		[WeakSelf, WeakTravel](TFlockRelayResult<TSharedPtr<FFlockRelayConnection>> Opened)
		{
			const TSharedPtr<FFlockTravels> Self = WeakSelf.Pin();
			const TSharedPtr<FTravel> Pinned = WeakTravel.Pin();
			if (!Self.IsValid() || !Pinned.IsValid() || Pinned->Step != EStep::OpeningRelayToTheHost)
			{
				return;
			}
			if (!Opened.bSuccess || !Opened.Value.IsValid())
			{
				Self->RelayFailedFor(Pinned.ToSharedRef(), Opened.Failure);
				return;
			}
			const TSharedRef<FFlockRelayConnection> Relay = Opened.Value.ToSharedRef();
			Pinned->Relay = Relay;
			Relay->OpenTo(Pinned->HostOnTheRelay, [WeakSelf, WeakTravel](TFlockRelayResult<bool> Open)
			{
				const TSharedPtr<FFlockTravels> Self = WeakSelf.Pin();
				const TSharedPtr<FTravel> Pinned = WeakTravel.Pin();
				if (!Self.IsValid() || !Pinned.IsValid() || Pinned->Step != EStep::OpeningRelayToTheHost)
				{
					return;
				}
				if (!Open.bSuccess)
				{
					Self->RelayFailedFor(Pinned.ToSharedRef(), Open.Failure);
					return;
				}
				Self->PublishSeatAddress(Pinned.ToSharedRef());
			});
		});
}

void FFlockTravels::RelayFailedFor(const FTravelRef& Travel, const FFlockRelayFailure& Failure)
{
	const UFlockMultiplayerSession* Session = Travel->Session.Get();
	if (Failure.IsCancelled())
	{
		// The session's end closes its relay, and answers those waiting for it as cancelled.
		if (Session == nullptr || Session->HasEnded())
		{
			Complete(Travel, FlockTravelToSessionOutcomes::SessionEnded);
			return;
		}
		Fail(Travel, Failure.Error);
		return;
	}
	Logger->LogWarning(FString::Printf(TEXT("Could not reach the host through Flock's relay (%s)"), *Failure.Error.ReasonAndFix()));
	Travel->Result.RelayFailureReason = Failure.Reason;
	Complete(Travel, FlockTravelToSessionOutcomes::RelayFailed);
}

void FFlockTravels::PublishSeatAddress(const FTravelRef& Travel)
{
	const TSharedPtr<FFlockMultiplayerSessions> Owner = Sessions.Pin();
	if (!Owner.IsValid() || !Travel->Relay.IsValid())
	{
		Fail(Travel, ShutDownWhileTraveling());
		return;
	}
	Travel->Step = EStep::PublishingSeatAddress;
	const TWeakPtr<FFlockTravels> WeakSelf = AsShared();
	const TWeakPtr<FTravel> WeakTravel = Travel;
	// The host's relay lets this player in once it knows the player's relay address, since a relay may give each address an IP of
	// its own. Flock not taking it (none reachable now, or a Flock from before the route) leaves the player to a relay that gives
	// every address one IP, so it is said and the connect goes on.
	const FFlockRequestHandle Publishing = Owner->PublishSeatRelayAddress(Travel->Session.Get(), Travel->Relay.ToSharedRef(),
		[WeakSelf, WeakTravel](TFlockResult<UFlockMultiplayerSession*> Published)
		{
			const TSharedPtr<FFlockTravels> Self = WeakSelf.Pin();
			const TSharedPtr<FTravel> Pinned = WeakTravel.Pin();
			if (!Self.IsValid() || !Pinned.IsValid() || Pinned->Step != EStep::PublishingSeatAddress)
			{
				return;
			}
			const UFlockMultiplayerSession* Session = Pinned->Session.Get();
			if (Session == nullptr || Session->HasEnded())
			{
				Self->Complete(Pinned.ToSharedRef(), FlockTravelToSessionOutcomes::SessionEnded);
				return;
			}
			if (!Published.bSuccess)
			{
				Self->Logger->LogWarning(FString::Printf(TEXT("This player's relay address could not be given to the host (%s); a relay that gives each "
					"address an IP of its own will not let it in."), *Published.Error.ReasonAndFix()));
			}
			Self->ConnectThroughTheBridge(Pinned.ToSharedRef());
		});
	if (Travel->Step == EStep::PublishingSeatAddress)
	{
		Travel->OnItsWay = Publishing;
	}
}

void FFlockTravels::ConnectThroughTheBridge(const FTravelRef& Travel)
{
	// The bridge to the host the game was with makes way: one bridge a relay.
	LeaveTheHost();
	FFlockRelayBridge::FToTheHostOptions Bridging;
	Bridging.bHostReadsBridgeMessages = Travel->bHostReadsBridgeMessages;
	// Always keeps this device's addresses from the host; When Needed moves to a direct path once one proves itself.
	Bridging.bGoesDirect = Travel->Options.Relay == EFlockRelayUse::WhenNeeded && Travel->bHostOffersDirectPaths;
	const TFlockRelayResult<FFlockRelayBridge::FPtr> Made = FFlockRelayBridge::ToTheHost(Travel->Relay.ToSharedRef(), Travel->HostOnTheRelay, Logger, Bridging);
	if (!Made.bSuccess)
	{
		if (Made.Failure.Reason.IsEmpty())
		{
			Fail(Travel, Made.Failure.Error);
			return;
		}
		RelayFailedFor(Travel, Made.Failure);
		return;
	}
	Travel->Bridge = Made.Value;
	StartConnecting(Travel, FFlockSockets::LoopbackIpv4, Made.Value->GetLocalPort());
}

void FFlockTravels::StartConnecting(const FTravelRef& Travel, const FString& Address, int32 Port)
{
	// The engine's own way out of a lost connection (back to the default level) may be under way: it takes a frame.
	if (Travel->Game->Read().bChangingLevel)
	{
		WhenSettled(Travel, [WeakSelf = TWeakPtr<FFlockTravels>(AsShared()), Address, Port](const FTravelRef& Settled)
		{
			if (const TSharedPtr<FFlockTravels> Self = WeakSelf.Pin())
			{
				Self->StartConnecting(Settled, Address, Port);
			}
		});
		return;
	}
	if (!IsFreeToTravel(*Travel, Travel->Game->Read()))
	{
		Fail(Travel, TravelRefused(TEXT("This game started hosting, connecting or opening a level while it waited for the host, so it does not travel "
			"to the session"), TEXT("Leave the game in a level of its own while Travel To Session waits for the host, then travel again.")));
		return;
	}
	Travel->ConnectAddress = Address;
	Travel->ConnectPort = Port;
	if (!Travel->Result.bThroughRelay)
	{
		Travel->Result.Address = Address;
		Travel->Result.Port = Port;
	}
	const TSharedPtr<FFlockMultiplayerSessions> Owner = Sessions.Pin();
	if (!Owner.IsValid())
	{
		Fail(Travel, ShutDownWhileTraveling());
		return;
	}
	// A token lasts 60 s, so it is asked for once the host is known; the host lets in only a player whose token Flock verifies.
	Travel->Step = EStep::AskingForJoinToken;
	const TWeakPtr<FFlockTravels> WeakSelf = AsShared();
	const TWeakPtr<FTravel> WeakTravel = Travel;
	const double AskedAt = Now();
	const FFlockRequestHandle Asking = Owner->RequestJoinToken(Travel->Session.Get(),
		[WeakSelf, WeakTravel, AskedAt](TFlockResult<FFlockMultiplayerSessionJoinToken> Token)
		{
			const TSharedPtr<FFlockTravels> Self = WeakSelf.Pin();
			const TSharedPtr<FTravel> Pinned = WeakTravel.Pin();
			if (!Self.IsValid() || !Pinned.IsValid() || Pinned->Step != EStep::AskingForJoinToken)
			{
				return;
			}
			if (!Token.bSuccess)
			{
				// Nothing travels without a token: the host would turn the player away.
				Self->Fail(Pinned.ToSharedRef(), Token.Error);
				return;
			}
			// Kept for the player's stay with the host, counted from when it was asked for.
			Pinned->JoinToken = Token.Value.Token;
			Pinned->JoinTokenGoodUntil = AskedAt + (Token.Value.ExpiresInSeconds > 0 ? Token.Value.ExpiresInSeconds : 60);
			Self->TravelWithToken(Pinned.ToSharedRef(), Token.Value.Token);
		});
	if (Travel->Step == EStep::AskingForJoinToken)
	{
		Travel->OnItsWay = Asking;
	}
}

void FFlockTravels::TravelWithToken(const FTravelRef& Travel, const FString& JoinToken)
{
	if (Travel->Game->Read().bChangingLevel)
	{
		WhenSettled(Travel, [WeakSelf = TWeakPtr<FFlockTravels>(AsShared()), JoinToken](const FTravelRef& Settled)
		{
			if (const TSharedPtr<FFlockTravels> Self = WeakSelf.Pin())
			{
				Self->TravelWithToken(Settled, JoinToken);
			}
		});
		return;
	}
	if (!IsFreeToTravel(*Travel, Travel->Game->Read()))
	{
		Fail(Travel, TravelRefused(TEXT("This game started hosting, connecting or opening a level while it asked for its join token, so it does not travel "
			"to the session"), TEXT("Leave the game in a level of its own while Travel To Session runs, then travel again.")));
		return;
	}
	Travel->Step = EStep::Connecting;
	Travel->OnItsWay = FFlockRequestHandle();
	// A failure told before the connection began, a refusal's words among them, says nothing of this connection.
	Travel->Failure.Reset();
	Travel->bRefused = false;
	Travel->RefusedReason.Reset();
	Travel->bHostAnswered = false;
	Travel->TryUntil = Travel->bTryingTheHostsNetwork ? Now() + TryOnTheHostsNetworkSeconds : 0.0;
	// The connection this travel makes is its own from here: the host the game was with lets go.
	LeaveTheHost();
	Travel->Game->MarkWorld();
	// The engine hands the travel URL's options to the host's PreLoginAsync, ahead of its own (measured: ?FlockJoinToken=...?Name=...).
	Travel->Game->TravelTo(FString::Printf(TEXT("%s:%d?%s=%s"), *Travel->ConnectAddress, Travel->ConnectPort, FlockJoinChecks::JoinTokenOption, *JoinToken));
}

void FFlockTravels::Tick()
{
	if (Current.IsValid())
	{
		const FTravelRef Travel = Current.ToSharedRef();
		if (Travel->Step == EStep::OpeningLevel || Travel->Step == EStep::StoppingAfterCancel)
		{
			MoveOpeningOn(Travel, Travel->Game->Read());
		}
		else if (Travel->Step == EStep::Settling)
		{
			MoveSettlingOn(Travel, Travel->Game->Read());
		}
		else if (Travel->Step == EStep::Connecting)
		{
			MoveConnectingOn(Travel, Travel->Game->Read());
		}
		else if (Travel->Step == EStep::StoppingTheTry)
		{
			MoveTheStoppedTryOn(Travel, Travel->Game->Read());
		}
	}
	KeepWithTheHost();
}

void FFlockTravels::MoveConnectingOn(const FTravelRef& Travel, const FFlockGameNetState& State)
{
	// The host's engine answering comes before its check of the player, so a slow check is never taken for an address that does
	// not answer.
	Travel->bHostAnswered = Travel->bHostAnswered || State.bHostAnswered;
	// Whatever happened, the engine settles first: a connection it gave up is still being dropped when it says so (measured), and
	// it then loads the game's default level, so the completion runs only once the game is in the level it stays in.
	if (State.bHasWorld && State.bChangingLevel)
	{
		if (Travel->bTryingTheHostsNetwork && !Travel->bHostAnswered && Travel->Failure.IsEmpty() && !State.bInAnotherWorld && Now() >= Travel->TryUntil)
		{
			// Nobody answered on the host's network in time: the connection being made is given up, for the relay.
			Travel->Step = EStep::StoppingTheTry;
			Travel->Game->StopConnecting();
		}
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
	if (Travel->bRefused)
	{
		Travel->Result.RefusedReason = Travel->RefusedReason;
		Logger->LogWarning(FString::Printf(TEXT("The host %s turned this player away (%s). %s"), *WhereOf(*Travel), *Travel->RefusedReason,
			*FlockHostRefusals::FixFor(Travel->RefusedReason)));
		Complete(Travel, FlockTravelToSessionOutcomes::Refused);
		return;
	}
	// The engine gave up on the host's network before the host ever answered: the relay, as when the try ran out.
	if (Travel->bTryingTheHostsNetwork && !Travel->bHostAnswered && !Travel->Failure.IsEmpty())
	{
		MoveTheStoppedTryOn(Travel, State);
		return;
	}
	// Back in a level of its own: the engine gave the connection up.
	const FString Why = Travel->Failure.IsEmpty() ? FString(TEXT("no reason given")) : Travel->Failure;
	// Forwarding a port is no help to a player tried on the host's network or through the relay.
	Logger->LogWarning(Travel->Result.bThroughRelay || Travel->bTryingTheHostsNetwork
		? FString::Printf(TEXT("Could not connect to the host %s (%s)."), *WhereOf(*Travel), *Why)
		: FString::Printf(TEXT("Could not connect to the host %s (%s). Outside a LAN the host's port must be forwarded to it."), *WhereOf(*Travel), *Why));
	Complete(Travel, FlockTravelToSessionOutcomes::CouldNotConnect);
}

void FFlockTravels::MoveTheStoppedTryOn(const FTravelRef& Travel, const FFlockGameNetState& State)
{
	Travel->Step = EStep::StoppingTheTry;
	if (!IsOnItsOwn(State))
	{
		return;
	}
	// The try lasts seconds, and the relay opens only for a session still running.
	const UFlockMultiplayerSession* Session = Travel->Session.Get();
	if (Session == nullptr || Session->HasEnded())
	{
		Complete(Travel, FlockTravelToSessionOutcomes::SessionEnded);
		return;
	}
	Logger->LogInfo(FString::Printf(TEXT("The host's address on this network (%s:%d) did not answer, so this player connects through Flock's relay."),
		*Travel->ConnectAddress, Travel->ConnectPort));
	StartThroughTheRelay(Travel);
}

void FFlockTravels::Cancel(const FTravelRef& Travel)
{
	const TFlockResult<FFlockTravelToSessionResult> Cancelled =
		TFlockResult<FFlockTravelToSessionResult>::Fail(FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Request cancelled")));
	if (Next == Travel)
	{
		// Waiting for the travel it replaced: nothing of its own has begun.
		Next.Reset();
		Finish(Travel, Cancelled);
		return;
	}
	EndEarly(Travel, Cancelled);
}

void FFlockTravels::EndEarly(const FTravelRef& Travel, const TFlockResult<FFlockTravelToSessionResult>& Answer)
{
	if (Current != Travel || Travel->Step == EStep::Done || Travel->Step == EStep::StoppingAfterCancel)
	{
		return;
	}
	const EStep Was = Travel->Step;
	if (Was == EStep::OpeningLevel)
	{
		// The level opens anyway; once it has, a cancel stops it listening. The caller hears now; a newer travel takes the level over.
		Travel->Step = EStep::StoppingAfterCancel;
		FOnTravel OnComplete = MoveTemp(Travel->OnComplete);
		Travel->OnComplete = nullptr;
		if (OnComplete)
		{
			OnComplete(Answer);
		}
		return;
	}
	Travel->Step = EStep::Done;
	Travel->OnceSettled = nullptr;
	Travel->OnItsWay.Cancel();
	// A level this travel opened stops listening; one the host kept, or one a newer travel takes over, stays.
	if ((Was == EStep::Publishing || Was == EStep::OpeningRelay) && !Travel->bKeptHosting && !Travel->bRestarted)
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
	Finish(Travel, Answer);
}

void FFlockTravels::StopForShutdown()
{
	if (Next.IsValid())
	{
		const FTravelRef Waiting = Next.ToSharedRef();
		Next.Reset();
		Fail(Waiting, ShutDownWhileTraveling());
	}
	if (Current.IsValid())
	{
		const FTravelRef Travel = Current.ToSharedRef();
		Travel->Step = EStep::Done;
		Travel->OnceSettled = nullptr;
		Travel->OnItsWay.Cancel();
		Finish(Travel, TFlockResult<FFlockTravelToSessionResult>::Fail(ShutDownWhileTraveling()));
	}
	// No relay thread outlives Flock.
	LeaveTheHost();
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
	// A player in the host's game stays with it, its bridge kept; any other end lets the bridge go before the completion runs, so
	// a travel started from it finds the session's relay free. The relay itself stays the session's.
	const bool bIn = Result.bSuccess && !Result.Value.bIsHost && Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::Connected, ESearchCase::CaseSensitive);
	if (bIn && Travel->Game.IsValid() && !Travel->bAlreadyIn)
	{
		const TSharedRef<FWithTheHost> With = MakeShared<FWithTheHost>();
		With->Session = Travel->Session;
		With->Game = Travel->Game;
		With->Destination = FString::Printf(TEXT("%s:%d"), *Travel->ConnectAddress, Travel->ConnectPort);
		With->Bridge = Travel->Bridge;
		With->TokenOnHand = Travel->JoinToken;
		With->TokenOnHandGoodUntil = Travel->JoinTokenGoodUntil;
		With->HostPlayerId = Travel->Session.IsValid() ? Travel->Session->GetHostPlayerId() : FString();
		With->Result = Result.Value;
		WithTheHost = With;
	}
	else if (Travel->Bridge.IsValid())
	{
		Travel->Bridge->Close();
	}
	Travel->Bridge.Reset();
	// The engine's failure handlers go with it.
	Travel->Game.Reset();
	FOnTravel OnComplete = MoveTemp(Travel->OnComplete);
	Travel->OnComplete = nullptr;
	if (OnComplete)
	{
		OnComplete(Result);
	}
}

// ---- a player with its host ----

void FFlockTravels::KeepWithTheHost()
{
	if (!WithTheHost.IsValid())
	{
		return;
	}
	const TSharedRef<FWithTheHost> With = WithTheHost.ToSharedRef();
	const UFlockMultiplayerSession* Session = With->Session.Get();
	const bool bRelayStopped = With->Bridge.IsValid() && With->Bridge->GetRelay()->HasStopped();
	if (Session == nullptr || Session->HasEnded() || bRelayStopped || !With->Game.IsValid())
	{
		LeaveTheHost();
		return;
	}
	const FFlockGameNetState State = With->Game->Read();
	// With the host while connected or connecting to it (the engine swaps one connection for the next within a frame, a level
	// change of the host's included); anywhere else, it has left.
	const bool bWithIt = State.bHasWorld && State.Destination.Equals(With->Destination, ESearchCase::IgnoreCase);
	if (!bWithIt)
	{
		LeaveTheHost();
		return;
	}
	// A host's level change without seamless travel sends every player back through a new connection of the engine's own, made
	// with the options of the player's first travel and so its first join token, which may be past its 60 s. The token on hand
	// goes on it at once; a new one is asked for when none on hand is good, and goes in if it comes before the login (the host
	// takes no connection until its level has changed: its net driver's ServerTravelPause, 4 s by default).
	const double Time = Now();
	// The level it plays with the host, followed through the host's level changes.
	if (State.NetMode == NM_Client && State.bLetInByHost && !State.bChangingLevel)
	{
		With->Level = With->Game->CurrentLevel();
	}
	// A host that lost its relay and opened another publishes a new address: this player's bridge reaches the old one, so its
	// connection through the relay is gone. Told as this device's relay lost (once), so the game travels again.
	if (!With->bToldHostMoved && HostMovedOnTheRelay(*With, *Session))
	{
		With->bToldHostMoved = true;
		if (const TSharedPtr<FFlockMultiplayerSessions> Owner = Sessions.Pin())
		{
			Owner->TellTheHostMovedOnTheRelay(With->Session.Get());
		}
	}
	if (State.bConnecting)
	{
		if (!With->bConnectionHandled)
		{
			With->bConnectionHandled = true;
			// A new connection of the engine's through the same bridge: the host's is told, so it takes it from a loopback port of its own.
			if (With->Bridge.IsValid())
			{
				With->Bridge->GameConnectsAgain();
			}
			if (!With->TokenOnHand.IsEmpty() && With->TokenOnHandGoodUntil - Time >= UseTokenWithSecondsLeft)
			{
				PutTokenOnTheConnectionBack(With, With->TokenOnHand);
			}
			else
			{
				With->bConnectionWaitsForAToken = true;
			}
		}
	}
	else if (With->bConnectionHandled)
	{
		With->bConnectionHandled = false;
		if (With->bConnectionWaitsForAToken)
		{
			// The request goes on: its token is kept on hand for the next connection back.
			With->bConnectionWaitsForAToken = false;
			SayTheFirstTokenGoes(TEXT("Flock had not answered before the connection logged in"));
		}
	}
	KeepATokenOnHand(With, Time);
	if (With->Bridge.IsValid())
	{
		With->Bridge->Tick();
	}
}

void FFlockTravels::KeepATokenOnHand(const TSharedRef<FWithTheHost>& With, double Time)
{
	const bool bRunningLow = With->TokenOnHand.IsEmpty() || With->TokenOnHandGoodUntil - Time < ReplaceTokenWithSecondsLeft;
	if (With->bTokenOnItsWay || !(With->bConnectionWaitsForAToken || (bRunningLow && Time >= With->NextAskAt)))
	{
		return;
	}
	const TSharedPtr<FFlockMultiplayerSessions> Owner = Sessions.Pin();
	if (!Owner.IsValid())
	{
		return;
	}
	const TWeakPtr<FFlockTravels> WeakSelf = AsShared();
	const TWeakPtr<FWithTheHost> WeakWith = With;
	With->bTokenOnItsWay = true;
	With->TokenAskedAt = Time;
	// A session ended on Flock under this request is learned as its end (the next frame leaves the host), not logged as a failure.
	const FFlockRequestHandle Asking = Owner->RequestJoinTokenToKeepOnHand(With->Session.Get(), [WeakSelf, WeakWith](TFlockResult<FFlockMultiplayerSessionJoinToken> Token)
	{
		const TSharedPtr<FFlockTravels> Self = WeakSelf.Pin();
		const TSharedPtr<FWithTheHost> Pinned = WeakWith.Pin();
		// A token for another stay with a host, or for one left (the leave cancels it), changes nothing.
		if (!Self.IsValid() || !Pinned.IsValid() || Self->WithTheHost != Pinned)
		{
			return;
		}
		Pinned->bTokenOnItsWay = false;
		Pinned->TokenOnItsWay = FFlockRequestHandle();
		if (!Token.bSuccess)
		{
			Pinned->NextAskAt = Self->Now() + AskForATokenAgainAfterSeconds;
			if (Pinned->bConnectionWaitsForAToken)
			{
				Pinned->bConnectionWaitsForAToken = false;
				Self->SayTheFirstTokenGoes(Token.Error.ReasonAndFix());
			}
			return;
		}
		Pinned->TokenOnHand = Token.Value.Token;
		Pinned->TokenOnHandGoodUntil = Pinned->TokenAskedAt + (Token.Value.ExpiresInSeconds > 0 ? Token.Value.ExpiresInSeconds : 60);
		if (Pinned->bConnectionWaitsForAToken)
		{
			Pinned->bConnectionWaitsForAToken = false;
			Self->PutTokenOnTheConnectionBack(Pinned.ToSharedRef(), Token.Value.Token);
		}
	});
	if (With->bTokenOnItsWay)
	{
		With->TokenOnItsWay = Asking;
	}
}

void FFlockTravels::PutTokenOnTheConnectionBack(const TSharedRef<FWithTheHost>& With, const FString& Token)
{
	if (!With->Game.IsValid() || !With->Game->PutJoinTokenOnTheConnectionBeingMade(Token))
	{
		SayTheFirstTokenGoes(TEXT("the new one came after the connection had logged in"));
	}
}

void FFlockTravels::SayTheFirstTokenGoes(const FString& Why)
{
	Logger->LogWarning(FString::Printf(TEXT("No new join token for the connection back to the host (%s), so it goes with the player's first one, which "
		"the host's check turns away once its 60 s are up."), *Why));
}

void FFlockTravels::LeaveTheHost()
{
	if (!WithTheHost.IsValid())
	{
		return;
	}
	const TSharedRef<FWithTheHost> Leaving = WithTheHost.ToSharedRef();
	WithTheHost.Reset();
	if (Leaving->Session.IsValid() && !Leaving->Level.IsEmpty())
	{
		LevelPlayedSessionId = Leaving->Session->GetId();
		LevelPlayed = Leaving->Level;
		LastHostDestination = Leaving->Destination;
	}
	// No token is needed any more: its request ends, quietly.
	Leaving->bTokenOnItsWay = false;
	Leaving->bConnectionWaitsForAToken = false;
	FFlockRequestHandle Asking = Leaving->TokenOnItsWay;
	Leaving->TokenOnItsWay = FFlockRequestHandle();
	Asking.Cancel();
	if (Leaving->Bridge.IsValid())
	{
		Leaving->Bridge->Close();
	}
}
