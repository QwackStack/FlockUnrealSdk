// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockJoinChecksOnHost.h"
#include "Containers/Ticker.h"
#include "GameFramework/GameModeBase.h"
#include "Misc/ScopeExit.h"
#include "Multiplayer/FlockMultiplayerSession.h"
#include "Multiplayer/FlockTravelToSession.h"

namespace
{
	/** The key of one option of a travel URL's options ("Key=Value", or "Key" alone). */
	FString OptionKey(const FString& Option)
	{
		FString Key = Option;
		Option.Split(TEXT("="), &Key, nullptr);
		return Key;
	}

	/** The join token the options carry; the engine's own option keys ignore letter case, and so does this. */
	FString JoinTokenIn(const FString& Options)
	{
		TArray<FString> Parts;
		Options.ParseIntoArray(Parts, TEXT("?"), true);
		for (const FString& Part : Parts)
		{
			FString Value;
			if (OptionKey(Part).Equals(FlockJoinChecks::JoinTokenOption, ESearchCase::IgnoreCase) && Part.Split(TEXT("="), nullptr, &Value))
			{
				return Value;
			}
		}
		return FString();
	}

	/** The options as the player's game sent them, without Flock's join token. */
	FString OptionsWithoutJoinToken(const FString& Options)
	{
		TArray<FString> Parts;
		Options.ParseIntoArray(Parts, TEXT("?"), true);
		FString Kept;
		for (const FString& Part : Parts)
		{
			if (!OptionKey(Part).Equals(FlockJoinChecks::JoinTokenOption, ESearchCase::IgnoreCase))
			{
				Kept += TEXT("?") + Part;
			}
		}
		return Kept;
	}

	/** Words for the engine of a connection nobody is waiting on any more: it says only that the connection was null (measured). */
	constexpr const TCHAR* NobodyWaits = TEXT("left_while_checked");
}

FString FlockHostRefusals::WordsFor(const FString& Reason)
{
	return FString(Beginning) + Reason;
}

bool FlockHostRefusals::TryRead(const FString& Words, FString& OutReason)
{
	if (!Words.StartsWith(Beginning, ESearchCase::CaseSensitive) || Words.Len() == FCString::Strlen(Beginning))
	{
		return false;
	}
	OutReason = Words.Mid(FCString::Strlen(Beginning));
	return true;
}

FString FlockHostRefusals::FixFor(const FString& Reason)
{
	using namespace FlockTravelToSessionRefusedReasons;
	// Reasons are identifiers: a game's own reason spelled like one of Flock's in another case is the game's own.
	const auto Is = [&Reason](const TCHAR* Known) { return Reason.Equals(Known, ESearchCase::CaseSensitive); };
	if (Is(NoJoinToken))
	{
		return TEXT("Travel to the session with Travel To Session, which sends the join token the host checks.");
	}
	if (Is(JoinTokenInvalid))
	{
		return TEXT("Travel To Session again, which asks for a new token (they last 60 s); a player no longer seated joins the session again first.");
	}
	if (Is(AlreadyConnected))
	{
		return TEXT("This player is already in the host's game through another connection; leave it before connecting again.");
	}
	if (Is(FlockUnreachable))
	{
		return TEXT("The host could not check the join token with Flock in time; Travel To Session again in a moment.");
	}
	if (Is(SessionEnded))
	{
		return TEXT("The session has ended; find or host another.");
	}
	if (Is(NotHost))
	{
		return TEXT("That device no longer hosts the session; Travel To Session again once the new host has published.");
	}
	if (Is(RefusedByTheGame))
	{
		return TEXT("The host's game turned the player away without saying why; its own rules say what lets a player in.");
	}
	if (Is(JoinCheckMissing))
	{
		return TEXT("The host's game mode does not run Flock's check of joining players: its PreLoginAsync must call FlockJoinChecks::CheckJoiningPlayer.");
	}
	if (Is(RelayOnly))
	{
		return TEXT("The host takes players through Flock's relay only: travel with Travel To Session, with Relay not set to Never, which reaches it through the relay.");
	}
	return TEXT("The host's game turned the player away for this reason; its own rules say what lets a player in.");
}

FFlockJoinChecksOnHost::FFlockJoinChecksOnHost(const TSharedRef<IFlockLogger>& InLogger, TFunction<double()> InNow)
	: Logger(InLogger)
	, Now(MoveTemp(InNow))
{
}

FFlockJoinChecksOnHost::~FFlockJoinChecksOnHost()
{
	StopListeningForPreLogin();
}

void FFlockJoinChecksOnHost::StartHosting(UFlockMultiplayerSession* Session, const TSharedRef<IFlockGameTravel>& Game, bool bRelayOnly)
{
	StopHosting();
	Hosting = MakeShared<FHosting>();
	Hosting->Session.Reset(Session);
	Hosting->Game = Game;
	Hosting->bRelayOnly = bRelayOnly;
	ListenForPreLogin();
}

void FFlockJoinChecksOnHost::UseRelayBridge(const UFlockMultiplayerSession* Session, const FFlockRelayBridge::FRef& Bridge)
{
	if (!Hosting.IsValid() || Hosting->Session.Get() != Session || Hosting->Bridge.IsValid())
	{
		Bridge->Close();
		return;
	}
	Hosting->Bridge = Bridge;
}

void FFlockJoinChecksOnHost::StopHosting()
{
	if (!Hosting.IsValid())
	{
		return;
	}
	CloseRelayBridge(*Hosting);
	Hosting.Reset();
	StopListeningForPreLogin();
	AnswerEveryWaitingCheck();
}

void FFlockJoinChecksOnHost::CloseRelayBridge(FHosting& InHosting)
{
	if (InHosting.Bridge.IsValid())
	{
		InHosting.Bridge->Close();
		InHosting.Bridge.Reset();
	}
	InHosting.RelayedPlayers.Reset();
}

bool FFlockJoinChecksOnHost::TryGetRelayPeer(const FHosting& InHosting, uint64 Connection, FFlockRelayPeer& OutPeer, int32* OutLoopbackPort)
{
	const int32 Port = InHosting.Game->LoopbackPortOf(Connection);
	if (OutLoopbackPort != nullptr)
	{
		*OutLoopbackPort = Port;
	}
	return InHosting.Bridge.IsValid() && Port > 0 && InHosting.Bridge->TryGetPeer(Port, OutPeer);
}

void FFlockJoinChecksOnHost::LetInThroughTheRelay(FHosting& InHosting, uint64 Connection)
{
	FFlockRelayPeer Peer;
	int32 Port = 0;
	if (TryGetRelayPeer(InHosting, Connection, Peer, &Port))
	{
		InHosting.RelayedPlayers.Add(Connection, FHosting::FRelayed{ Peer, Port });
		InHosting.Bridge->LetIn(Peer);
	}
}

void FFlockJoinChecksOnHost::ForgetRelayedPlayersGone(FHosting& InHosting)
{
	if (!InHosting.Bridge.IsValid())
	{
		return;
	}
	for (auto It = InHosting.RelayedPlayers.CreateIterator(); It; ++It)
	{
		if (InHosting.Game->IsStillHere(It.Key()))
		{
			continue;
		}
		const FHosting::FRelayed Gone = It.Value();
		const FFlockRelayPeer Peer = Gone.Peer;
		It.RemoveCurrent();
		// What the gone connection held alone is let go: a newer connection of the player's let in through the same relay address
		// before this one was seen gone keeps its place, its channel and its link.
		bool bAnotherLetInThroughIt = false;
		for (const TPair<uint64, FHosting::FRelayed>& Other : InHosting.RelayedPlayers)
		{
			bAnotherLetInThroughIt = bAnotherLetInThroughIt || (Other.Value.Peer == Peer && InHosting.Game->IsStillHere(Other.Key));
		}
		const int32 Port = InHosting.Bridge->LoopbackPortOf(Peer);
		// The player's game connected again and its place moved to a loopback port of its own: that place is the newer connection's.
		if (bAnotherLetInThroughIt || (Port > 0 && Port != Gone.LoopbackPort))
		{
			continue;
		}
		// The player's game may be connecting again through the same relay address (a host's level change sends it back): that
		// connection keeps its path, and waits to be let in as a newly heard peer does.
		if (Port > 0 && InHosting.Game->IsAnyConnectionFromLoopbackPort(Port))
		{
			InHosting.Bridge->NoLongerLetIn(Peer);
		}
		else
		{
			InHosting.Bridge->Forget(Peer);
		}
	}
}

void FFlockJoinChecksOnHost::AnswerEveryWaitingCheck()
{
	// Nobody waits on Flock for this hosting now. The engine never drops a waiting player itself, so one still waiting is turned
	// away rather than left there; nothing is remembered, and an answer landing later finds its check answered.
	const TArray<FCheckRef> Answering = Waiting;
	for (const FCheckRef& Check : Answering)
	{
		if (Take(Check))
		{
			Refuse(Check->Request, FlockTravelToSessionRefusedReasons::FlockUnreachable);
		}
	}
	// Verified, but waiting for an earlier connection of theirs to give way: turned away the same, the hosting being over.
	const TArray<FWaitForTheEarlier> Holding = MoveTemp(WaitsForTheEarlier);
	WaitsForTheEarlier.Reset();
	for (const FWaitForTheEarlier& Wait : Holding)
	{
		Refuse(Wait.Check->Request, FlockTravelToSessionRefusedReasons::FlockUnreachable);
	}
}

bool FFlockJoinChecksOnHost::IsOpenToPlayers(const FFlockGameNetState& State)
{
	const bool bListening = State.bHasWorld && State.NetMode == NM_ListenServer && State.ListeningPort > 0;
	return bListening || (State.bHasWorld && State.bChangingLevel);
}

void FFlockJoinChecksOnHost::StopForShutdown()
{
	if (!Hosting.IsValid())
	{
		return;
	}
	if (!IsOpenToPlayers(Hosting->Game->Read()))
	{
		StopHosting();
		return;
	}
	// No relay thread outlives Flock: the relayed players' path closes with it.
	CloseRelayBridge(*Hosting);
	// Flock shut down while the hosted level is still open to players: nobody can be checked, so nobody is let in, until that
	// level closes. This outlives its owner, kept by the engine's ticker; the engine's PreLogin event, which the game mode's call
	// raises with Flock not running, is where players are turned away.
	AnswerEveryWaitingCheck();
	Hosting->bFlockStopped = true;
	Hosting->Game->MarkWorld();
	const TSharedRef<FFlockJoinChecksOnHost> Self = AsShared();
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Self](float)
	{
		return Self->RefusesUntilTheLevelCloses();
	}));
}

bool FFlockJoinChecksOnHost::RefusesUntilTheLevelCloses()
{
	if (!Hosting.IsValid() || !Hosting->bFlockStopped)
	{
		return false;
	}
	// The level the session was hosted in has closed, or the game is in another one (hosting again, or anywhere else).
	const FFlockGameNetState State = Hosting->Game->Read();
	if (!IsOpenToPlayers(State) || State.bInAnotherWorld)
	{
		StopHosting();
		return false;
	}
	return true;
}

void FFlockJoinChecksOnHost::Check(FFlockJoiningRequest Request)
{
	if (!Hosting.IsValid())
	{
		// Not hosting through Travel To Session: the engine's own PreLoginAsync.
		const FString Refusal = Request.RunEngineChecks ? Request.RunEngineChecks() : FString();
		if (Request.Answer)
		{
			Request.Answer(Refusal);
		}
		return;
	}
	const FString Token = JoinTokenIn(Request.Options);
	if (Token.IsEmpty())
	{
		Refuse(Request, FlockTravelToSessionRefusedReasons::NoJoinToken);
		return;
	}
	const uint64 Connection = Hosting->Game->FindJoiningConnection(Request.Options, Request.Address);
	if (Connection == 0)
	{
		Logger->LogWarning(FString::Printf(TEXT("Refused a joining player from %s: no connection waiting on this host asked with the options the game mode passed, so the player could not be told apart. "
			"Call FlockJoinChecks::CheckJoiningPlayer from the game mode's PreLoginAsync itself, with the Options and Address the engine gave it, unchanged."), *Request.Address));
		Refuse(Request, FlockTravelToSessionRefusedReasons::JoinCheckMissing);
		return;
	}
	// Relay Always: every player comes through the relay, so one that reached this host another way never gets in, whatever its token.
	FFlockRelayPeer ThroughTheRelay;
	if (Hosting->bRelayOnly && !TryGetRelayPeer(*Hosting, Connection, ThroughTheRelay))
	{
		Refuse(Request, FlockTravelToSessionRefusedReasons::RelayOnly);
		return;
	}

	const FCheckRef Check = MakeShared<FCheck>();
	Check->Hosting = Hosting;
	Check->Connection = Connection;
	Check->Request = MoveTemp(Request);
	Check->Deadline = Now() + CheckSeconds;
	Waiting.Add(Check);
	const TWeakPtr<FFlockJoinChecksOnHost> WeakSelf = AsShared();
	const TWeakPtr<FCheck> WeakCheck = Check;
	// Asked without holding the frame; the answer is taken on the game thread, where the engine reads it. A session that has
	// ended is refused by the session itself, with no request.
	const FFlockRequestHandle Asking = Hosting->Session->VerifyJoinToken(Token, [WeakSelf, WeakCheck](TFlockResult<FString> Result)
	{
		const TSharedPtr<FFlockJoinChecksOnHost> Self = WeakSelf.Pin();
		const TSharedPtr<FCheck> Pinned = WeakCheck.Pin();
		if (Self.IsValid() && Pinned.IsValid())
		{
			Self->Verified(Pinned.ToSharedRef(), Result);
		}
	});
	// An answer given at once has settled the check already; the handle kept is one still on its way.
	if (!Check->bAnswered)
	{
		Check->Asking = Asking;
	}
}

void FFlockJoinChecksOnHost::Verified(const FCheckRef& Check, const TFlockResult<FString>& Result)
{
	if (!Take(Check))
	{
		return;
	}
	FHosting& Held = *Check->Hosting;
	const FString Reason = Result.bSuccess ? FString() : ReasonFor(Check, Result.Error);
	if (!Held.Game->IsStillHere(Check->Connection) || (Check->Request.IsAskerAlive && !Check->Request.IsAskerAlive()))
	{
		// The player left while being checked, or the level that asked is gone: nothing of the game's runs and nothing is remembered.
		Refuse(Check->Request, Reason.IsEmpty() ? FString(NobodyWaits) : Reason);
		return;
	}
	if (!Reason.IsEmpty())
	{
		if (Reason.Equals(FlockTravelToSessionRefusedReasons::FlockUnreachable, ESearchCase::CaseSensitive))
		{
			Logger->LogWarning(FString::Printf(TEXT("Refused a joining player from %s: Flock could not check their join token (%s). %s"),
				*Check->Request.Address, *Result.Error.ReasonAndFix(), TEXT("Nobody is let in unchecked; the player can travel again once this device reaches Flock.")));
		}
		Refuse(Check->Request, Reason);
		return;
	}
	const FString PlayerId = Result.Value;
	// One connection a player: one the host hears from keeps its place, and one presenting the earlier one's own token never takes
	// it. A silent one gives way once the newer one is let in; one going quiet is waited for.
	const uint64 Earlier = ConnectionElsewhere(Held, PlayerId, Check->Connection);
	if (Earlier != 0)
	{
		const FHosting::FLetIn* Was = Held.LetIn.Find(Earlier);
		if (Was != nullptr && Was->Token.Equals(JoinTokenIn(Check->Request.Options), ESearchCase::CaseSensitive))
		{
			Refuse(Check->Request, FlockTravelToSessionRefusedReasons::AlreadyConnected);
			return;
		}
		const double Silence = Held.Game->SecondsSinceHeardFrom(Earlier);
		if (Silence < EarlierConnectionGivesWayAfterSeconds)
		{
			FWaitForTheEarlier Wait{ Check, PlayerId, Earlier, Silence, Now() + EarlierConnectionGivesWayAfterSeconds };
			WaitsForTheEarlier.Add(MoveTemp(Wait));
			return;
		}
	}
	LetInOverTheEarlier(Check, PlayerId, Earlier);
}

void FFlockJoinChecksOnHost::MoveTheWaitsForTheEarlierOn()
{
	if (WaitsForTheEarlier.Num() == 0)
	{
		return;
	}
	const double Time = Now();
	const TArray<FWaitForTheEarlier> Holding = WaitsForTheEarlier;
	WaitsForTheEarlier.Reset();
	for (FWaitForTheEarlier Wait : Holding)
	{
		const FCheckRef& Check = Wait.Check;
		FHosting& Held = *Check->Hosting;
		// A hosting that ended answered its waits (AnswerEveryWaitingCheck).
		if (!Held.Game->IsStillHere(Check->Connection) || (Check->Request.IsAskerAlive && !Check->Request.IsAskerAlive()))
		{
			// The newer connection or the level that asked is gone: nobody waits for the answer.
			Refuse(Check->Request, NobodyWaits);
			continue;
		}
		const bool bEarlierGone = !Held.Game->IsStillHere(Wait.Earlier);
		const double Silence = bEarlierGone ? EarlierConnectionGivesWayAfterSeconds : Held.Game->SecondsSinceHeardFrom(Wait.Earlier);
		if (bEarlierGone || Silence >= EarlierConnectionGivesWayAfterSeconds)
		{
			LetInOverTheEarlier(Check, Wait.PlayerId, bEarlierGone ? 0 : Wait.Earlier);
			continue;
		}
		// Heard from again (its silence started over), or still not silent long enough by the time the wait can last: it stays.
		if (Silence < Wait.LastSilence || Time >= Wait.GiveUpAt)
		{
			Refuse(Check->Request, FlockTravelToSessionRefusedReasons::AlreadyConnected);
			continue;
		}
		Wait.LastSilence = Silence;
		WaitsForTheEarlier.Add(MoveTemp(Wait));
	}
}

void FFlockJoinChecksOnHost::LetInOverTheEarlier(const FCheckRef& Check, const FString& PlayerId, uint64 Earlier)
{
	FHosting& Held = *Check->Hosting;
	FString EngineRefusal;
	if (Check->Request.RunEngineChecks)
	{
		// The engine's PreLogin event this raises is this check's own, not a game mode that skips it.
		bRunningEngineChecks = true;
		ON_SCOPE_EXIT { bRunningEngineChecks = false; };
		EngineRefusal = Check->Request.RunEngineChecks();
	}
	if (!EngineRefusal.IsEmpty())
	{
		Refuse(Check->Request, EngineRefusal);
		return;
	}
	if (Check->Request.ApproveJoiningPlayer)
	{
		FFlockJoiningPlayer Player;
		Player.PlayerId = PlayerId;
		Player.Options = OptionsWithoutJoinToken(Check->Request.Options);
		Player.Address = Check->Request.Address;
		Player.UniqueId = Check->Request.UniqueId;
		FString GamesReason;
		if (!Check->Request.ApproveJoiningPlayer(Player, GamesReason))
		{
			Refuse(Check->Request, GamesReason.IsEmpty() ? FString(FlockTravelToSessionRefusedReasons::RefusedByTheGame) : GamesReason);
			return;
		}
	}
	FHosting::FRelayed EarlierRelayed;
	bool bEarlierRelayed = false;
	if (Earlier != 0)
	{
		// Taken out here, so the forgetting of relayed players gone leaves the newer connection's relay address alone.
		Held.LetIn.Remove(Earlier);
		bEarlierRelayed = Held.RelayedPlayers.RemoveAndCopyValue(Earlier, EarlierRelayed);
		Held.Game->CloseConnection(Earlier);
		Logger->LogInfo(FString::Printf(TEXT("Closed a player's earlier connection, silent for %g s or more, for their newer one from %s."),
			EarlierConnectionGivesWayAfterSeconds, *Check->Request.Address));
	}
	// Remembered once let in, so a refused connection never holds the player's place; a relayed one keeps its place on the bridge.
	Held.LetIn.Add(Check->Connection, FHosting::FLetIn{ PlayerId, JoinTokenIn(Check->Request.Options) });
	LetInThroughTheRelay(Held, Check->Connection);
	if (bEarlierRelayed && Held.Bridge.IsValid() && !(Held.RelayedPlayers.Contains(Check->Connection) && Held.RelayedPlayers[Check->Connection].Peer == EarlierRelayed.Peer))
	{
		// The earlier one's relay address, unless the newer comes through it too.
		Held.Bridge->Forget(EarlierRelayed.Peer);
	}
	if (Check->Request.Answer)
	{
		Check->Request.Answer(FString());
	}
}

FString FFlockJoinChecksOnHost::ReasonFor(const FCheckRef& Check, const FFlockError& Failure) const
{
	const UFlockMultiplayerSession* Session = Check->Hosting->Session.Get();
	if (Session == nullptr || Session->HasEnded() || Failure.ErrorCode == EFlockErrorCode::MultiplayerSessionNotFound)
	{
		return FlockTravelToSessionRefusedReasons::SessionEnded;
	}
	if (Failure.ErrorCode == EFlockErrorCode::MultiplayerInvalidJoinToken)
	{
		return FlockTravelToSessionRefusedReasons::JoinTokenInvalid;
	}
	if (Failure.ErrorCode == EFlockErrorCode::MultiplayerNotHost)
	{
		return FlockTravelToSessionRefusedReasons::NotHost;
	}
	return FlockTravelToSessionRefusedReasons::FlockUnreachable;
}

uint64 FFlockJoinChecksOnHost::ConnectionElsewhere(FHosting& InHosting, const FString& PlayerId, uint64 Connection)
{
	uint64 Elsewhere = 0;
	for (auto It = InHosting.LetIn.CreateIterator(); It; ++It)
	{
		if (!InHosting.Game->IsStillHere(It.Key()))
		{
			It.RemoveCurrent();
		}
		else if (It.Key() != Connection && It.Value().PlayerId.Equals(PlayerId, ESearchCase::CaseSensitive))
		{
			Elsewhere = It.Key();
		}
	}
	return Elsewhere;
}

bool FFlockJoinChecksOnHost::Take(const FCheckRef& Check)
{
	if (Check->bAnswered)
	{
		return false;
	}
	Check->bAnswered = true;
	Waiting.Remove(Check);
	// Given up after it is marked, so the answer cancelling hands back changes nothing.
	FFlockRequestHandle Asking = Check->Asking;
	Check->Asking = FFlockRequestHandle();
	Asking.Cancel();
	return true;
}

void FFlockJoinChecksOnHost::Refuse(const FFlockJoiningRequest& Request, const FString& Reason)
{
	if (Request.Answer)
	{
		Request.Answer(FlockHostRefusals::WordsFor(Reason));
	}
}

FString FFlockJoinChecksOnHost::PlayerFor(const UFlockMultiplayerSession* Session, const APlayerController* Controller) const
{
	if (!Hosting.IsValid() || Session == nullptr || Controller == nullptr || Hosting->Session.Get() != Session)
	{
		return FString();
	}
	if (Hosting->Game->IsOwnPlayer(Controller))
	{
		return Session->PlayerId;
	}
	const uint64 Connection = Hosting->Game->ConnectionOf(Controller);
	if (Connection == 0 || !Hosting->Game->IsStillHere(Connection))
	{
		return FString();
	}
	const FHosting::FLetIn* Found = Hosting->LetIn.Find(Connection);
	return Found != nullptr ? Found->PlayerId : FString();
}

void FFlockJoinChecksOnHost::Tick()
{
	// Not listening, and no level opening (the host's own, from before Travel To Session asked for it, or a later one): the engine
	// has closed every connection, and nobody joins this hosting any more.
	const FFlockGameNetState State = Hosting.IsValid() ? Hosting->Game->Read() : FFlockGameNetState();
	if (Hosting.IsValid() && !IsOpenToPlayers(State))
	{
		StopHosting();
	}
	if (Hosting.IsValid() && Hosting->Bridge.IsValid())
	{
		FHosting& Held = *Hosting;
		// The relay stopped (the session ended, it was lost, or another opened in its place): its players' path is gone.
		if (Held.Bridge->GetRelay()->HasStopped())
		{
			CloseRelayBridge(Held);
		}
		else
		{
			// A level opened again (the host's level change) may listen on another port.
			Held.Bridge->SetNetDriverPort(State.ListeningPort);
			ForgetRelayedPlayersGone(Held);
			Held.Bridge->Tick();
		}
	}
	MoveTheWaitsForTheEarlierOn();
	if (Waiting.Num() == 0)
	{
		return;
	}
	const double Time = Now();
	const TArray<FCheckRef> Due = Waiting.FilterByPredicate([Time](const FCheckRef& Check) { return Check->Deadline <= Time; });
	for (const FCheckRef& Check : Due)
	{
		if (!Take(Check))
		{
			continue;
		}
		const UFlockMultiplayerSession* Session = Check->Hosting->Session.Get();
		const bool bEnded = Session == nullptr || Session->HasEnded();
		const FString Reason = bEnded ? FlockTravelToSessionRefusedReasons::SessionEnded : FlockTravelToSessionRefusedReasons::FlockUnreachable;
		// Said only for a player really refused for Flock's silence.
		if (!bEnded && Check->Hosting->Game->IsStillHere(Check->Connection))
		{
			// The HTTP layer goes on trying through failures that can pass (no answer, a server error, too many checks a minute),
			// each logged by the request log as it comes, so this says only what the check saw.
			Logger->LogWarning(FString::Printf(TEXT("Refused a joining player from %s: Flock gave no answer to the check of their join token within %g s. "
				"Nobody is let in unchecked. The request log above says why: no connection to Flock (check this device's network), or too "
				"many checks a minute from this host (someone is trying many connections); the player can travel again."), *Check->Request.Address, CheckSeconds));
		}
		Refuse(Check->Request, Reason);
	}
}

void FFlockJoinChecksOnHost::TakePreLoginEvent(AGameModeBase* GameMode, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
	if (bRunningEngineChecks || !Hosting.IsValid() || !Hosting->Game->IsThisGame(GameMode))
	{
		return;
	}
	if (Hosting->bFlockStopped)
	{
		// Flock was shut down while the level stayed open: nobody can be checked, whatever the game mode does.
		if (ErrorMessage.IsEmpty())
		{
			ErrorMessage = FlockHostRefusals::WordsFor(FlockTravelToSessionRefusedReasons::FlockUnreachable);
		}
		if (!Hosting->bToldFlockStopped)
		{
			Hosting->bToldFlockStopped = true;
			Logger->LogWarning(TEXT("Turned a joining player away: Flock was shut down while this game hosted its session through Travel To Session, so "
				"nobody can be checked, and every player is turned away until the level stops listening. Leave the hosted level before shutting "
				"Flock down, or initialize Flock and host again."));
		}
		return;
	}
	// A game mode of the hosting game that skips Flock's check: its player is not let in unchecked.
	if (ErrorMessage.IsEmpty())
	{
		ErrorMessage = FlockHostRefusals::WordsFor(FlockTravelToSessionRefusedReasons::JoinCheckMissing);
	}
	if (!Hosting->bToldCheckMissing)
	{
		Hosting->bToldCheckMissing = true;
		Logger->LogError(FString::Printf(TEXT("A player tried to join this game's session, but the game mode %s does not run Flock's check of joining players, so nobody is let in. "
			"Call FlockJoinChecks::CheckJoiningPlayer from the game mode's PreLoginAsync, in place of the engine's own."), *GetNameSafe(GameMode)));
	}
}

void FFlockJoinChecksOnHost::ListenForPreLogin()
{
	if (!PreLoginHandle.IsValid())
	{
		PreLoginHandle = FGameModeEvents::OnGameModePreLoginEvent().AddSP(AsShared(), &FFlockJoinChecksOnHost::TakePreLoginEvent);
	}
}

void FFlockJoinChecksOnHost::StopListeningForPreLogin()
{
	if (PreLoginHandle.IsValid())
	{
		FGameModeEvents::OnGameModePreLoginEvent().Remove(PreLoginHandle);
		PreLoginHandle.Reset();
	}
}
