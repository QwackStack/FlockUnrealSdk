// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockLogger.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockResult.h"
#include "Multiplayer/FlockJoinChecks.h"
#include "Multiplayer/FlockTravelToSession.h"
#include "Multiplayer/Relay/FlockRelayBridge.h"
#include "UObject/StrongObjectPtr.h"

class AGameModeBase;
class APlayerController;
class IFlockGameTravel;
class UFlockMultiplayerSession;

/**
 * The words a host's check sends a player it turns away: the reason after a fixed beginning, which the player's Travel To Session
 * reads. The engine tells a host closing its connections with words of its own ("Host closed the connection.", in the host's
 * language) through the same failure (measured), so only words with this beginning are a refusal.
 */
namespace FlockHostRefusals
{
	inline constexpr const TCHAR* Beginning = TEXT("The host turned this player away: ");

	FString WordsFor(const FString& Reason);

	/** True, with the reason, for words a host's check sent. */
	bool TryRead(const FString& Words, FString& OutReason);

	/** How a game gets past a refusal for this reason, for the joining player's log; one of the game's own or the engine's gets a general fix. */
	FString FixFor(const FString& Reason);
}

/** One joining player, as the game mode's PreLoginAsync hands it over. */
struct FFlockJoiningRequest
{
	FString Options;
	FString Address;
	FUniqueNetIdRepl UniqueId;
	/** The engine's own checks of the player (the game mode's PreLogin): its refusal, or empty to go on. */
	TFunction<FString()> RunEngineChecks;
	/** Hands the engine the answer, once: empty lets the player in. */
	TFunction<void(const FString& Refusal)> Answer;
	/** False once the game mode that asked is gone, so nothing of the game's runs for it. */
	TFunction<bool()> IsAskerAlive;
	FFlockApproveJoiningPlayer ApproveJoiningPlayer;
};

/**
 * The one owner of the host's check of joining players. Travel To Session's host turns it on for its game before its level
 * opens listening; it ends once the game is neither listening nor changing level (a host travel that does not start hosting
 * stops listening), at another host travel, and at Flock's shutdown. Players the host takes through Flock's relay reach it from
 * the relay bridge's loopback ports: one let in keeps its place there and gets a relay channel, and the bridge forgets it once no
 * connection of it is left; the bridge closes with the hosting, with its relay, and at shutdown.
 * A player is let in only once Flock verified their join token, one connection a player; a connection
 * is remembered only once let in, and one no longer on the host holds no place, so a refusal and a departure need no removal of
 * their own. Every check is answered once: by Flock's answer, by its 8 s running out, or when hosting ends; an answer landing
 * after that changes nothing. Game-thread only.
 */
class FFlockJoinChecksOnHost : public TSharedFromThis<FFlockJoinChecksOnHost>
{
public:
	/** How long Flock has to answer: the engine never drops a waiting player itself (measured 130 s), so this bound is what tells the player why. */
	static constexpr double DefaultCheckSeconds = 8.0;
	/**
	 * A player's earlier connection the host has heard nothing from for this long gives way to the player's newer one once that is
	 * let in (it is closed): what a lost relay or a game that stopped leaves on the host until the engine's own timeout, a minute.
	 * A newer connection that comes sooner waits for it, as long as the earlier stays quiet; one heard from keeps its place (one
	 * connection a player). The newer must bring a token of its own: a join token stays good for its 60 s and travels in the clear,
	 * so the earlier one's token presented again never takes its place.
	 */
	static constexpr double EarlierConnectionGivesWayAfterSeconds = 5.0;

	FFlockJoinChecksOnHost(const TSharedRef<IFlockLogger>& InLogger, TFunction<double()> InNow);
	~FFlockJoinChecksOnHost();

	/**
	 * Session is hosted by the game Game is the engine side of; a hosting before it ends first. With bRelayOnly (Relay Always),
	 * a player not arriving through the relay bridge is turned away (relay_only) before Flock is asked.
	 */
	void StartHosting(UFlockMultiplayerSession* Session, const TSharedRef<IFlockGameTravel>& Game, bool bRelayOnly = false);

	/** Players come in through Flock's relay by Bridge while Session is hosted; closed at once when it is not, or a bridge is in use already. */
	void UseRelayBridge(const UFlockMultiplayerSession* Session, const FFlockRelayBridge::FRef& Bridge);

	/** True while this game hosts Session through Travel To Session (its level listening, or opening). */
	bool IsHosting(const UFlockMultiplayerSession* Session) const { return Hosting.IsValid() && Session != nullptr && Hosting->Session.Get() == Session; }

	/** The relay bridge of the hosting, or null. */
	FFlockRelayBridge::FPtr GetRelayBridge() const { return Hosting.IsValid() ? Hosting->Bridge : nullptr; }

	/** The game no longer hosts through Travel To Session: every waiting check is answered as refused, and nothing is remembered. */
	void StopHosting();

	/** One joining player; Request.Answer is called once, now or later. */
	void Check(FFlockJoiningRequest Request);

	/** The Flock player behind Controller while this game hosts Session; empty otherwise. */
	FString PlayerFor(const UFlockMultiplayerSession* Session, const APlayerController* Controller) const;

	/** Answers the checks whose time ran out, and ends hosting once the game neither listens nor changes level; run once a frame. */
	void Tick();

	/**
	 * Flock is shutting down: every waiting check is answered as refused. While the hosted level stays open to players, every
	 * joining player is then turned away (flock_unreachable) through the engine's PreLogin event, until that level closes: this
	 * object is kept alive by the engine's ticker meanwhile, past the provider that owned it.
	 */
	void StopForShutdown();

	/** The engine's PreLogin event: a game mode of the hosting game that does not run this check lets nobody in. */
	void TakePreLoginEvent(AGameModeBase* GameMode, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage);

	/** The level is listening, or one is opening: players can reach it, or soon will. */
	static bool IsOpenToPlayers(const FFlockGameNetState& State);

	bool IsHostingForTesting() const { return Hosting.IsValid(); }
	int32 GetChecksWaitingForTesting() const { return Waiting.Num(); }
	void SetCheckSecondsForTesting(double Seconds) { CheckSeconds = Seconds; }

private:
	struct FHosting
	{
		TStrongObjectPtr<UFlockMultiplayerSession> Session;
		TSharedPtr<IFlockGameTravel> Game;
		/** Each connection let in: its player, and the join token it came with. */
		struct FLetIn
		{
			FString PlayerId;
			FString Token;
		};
		TMap<uint64, FLetIn> LetIn;
		/** The game mode not running the check was told once. */
		bool bToldCheckMissing = false;
		/** Flock shut down while the level stayed open: everyone is turned away until it closes. */
		bool bFlockStopped = false;
		/** A player turned away for that was told once. */
		bool bToldFlockStopped = false;
		/** Every player comes through the relay bridge (Relay Always); anyone else is turned away. */
		bool bRelayOnly = false;
		/** A connection let in through the bridge: the relay peer behind it, and the loopback port of the bridge's it came from. */
		struct FRelayed
		{
			FFlockRelayPeer Peer;
			int32 LoopbackPort = 0;
		};
		/** How relayed players reach this host, and each connection let in through it. */
		FFlockRelayBridge::FPtr Bridge;
		TMap<uint64, FRelayed> RelayedPlayers;
	};

	struct FCheck
	{
		TSharedPtr<FHosting> Hosting;
		uint64 Connection = 0;
		FFlockJoiningRequest Request;
		double Deadline = 0.0;
		FFlockRequestHandle Asking;
		bool bAnswered = false;
	};
	using FCheckRef = TSharedRef<FCheck>;

	/** A verified player's newer connection waiting for its earlier one, gone quiet, to be silent long enough to give way. */
	struct FWaitForTheEarlier
	{
		FCheckRef Check;
		FString PlayerId;
		uint64 Earlier = 0;
		/** How long the earlier one was silent at the last look: a shorter silence means it was heard from again. */
		double LastSilence = 0.0;
		double GiveUpAt = 0.0;
	};

	/** Flock's answer to a check still waiting. */
	void Verified(const FCheckRef& Check, const TFlockResult<FString>& Result);
	/** The rest of the check of a verified player, Earlier (0 for none) giving way once the game's own checks let the newer one in. */
	void LetInOverTheEarlier(const FCheckRef& Check, const FString& PlayerId, uint64 Earlier);
	/** The newer connections waiting for an earlier one, moved on once a frame. */
	void MoveTheWaitsForTheEarlierOn();
	/** Flock's verdict, or the host's state, as a reason; any other failure lets nobody in unchecked. */
	FString ReasonFor(const FCheckRef& Check, const FFlockError& Failure) const;
	/** The player's connection other than Connection still on the host, or 0; connections gone are forgotten here. */
	static uint64 ConnectionElsewhere(FHosting& InHosting, const FString& PlayerId, uint64 Connection);
	/** Takes the check out of the waiting ones and gives its request up; false when it was answered already. */
	bool Take(const FCheckRef& Check);
	/** The relay peer Connection comes through, from one of the bridge's loopback ports (OutLoopbackPort, when given); false for a connection of any other kind. */
	static bool TryGetRelayPeer(const FHosting& InHosting, uint64 Connection, FFlockRelayPeer& OutPeer, int32* OutLoopbackPort = nullptr);
	/** A player let in from the bridge keeps its place there and gets a relay channel. */
	static void LetInThroughTheRelay(FHosting& InHosting, uint64 Connection);
	/**
	 * Relayed players whose connection is gone: their address is forgotten unless another connection still comes through it, or its
	 * place moved on to a newer connection of the player's (a loopback port of its own).
	 */
	static void ForgetRelayedPlayersGone(FHosting& InHosting);
	static void CloseRelayBridge(FHosting& InHosting);
	static void Refuse(const FFlockJoiningRequest& Request, const FString& Reason);
	void AnswerEveryWaitingCheck();
	TArray<FWaitForTheEarlier> WaitsForTheEarlier;
	/** The engine's ticker, after a shutdown with the level open: false, ending it, once that level closes. */
	bool RefusesUntilTheLevelCloses();
	void ListenForPreLogin();
	void StopListeningForPreLogin();

	TSharedRef<IFlockLogger> Logger;
	TFunction<double()> Now;
	double CheckSeconds = DefaultCheckSeconds;
	TSharedPtr<FHosting> Hosting;
	TArray<FCheckRef> Waiting;
	/** True while this check runs the engine's own checks, whose PreLogin event is then this check's own. */
	bool bRunningEngineChecks = false;
	FDelegateHandle PreLoginHandle;
};
