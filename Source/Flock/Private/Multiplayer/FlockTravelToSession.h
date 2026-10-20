// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "FlockLogger.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockResult.h"
#include "Models/FlockMultiplayerModels.h"
#include "Multiplayer/Relay/FlockRelayBridge.h"
#include "UObject/StrongObjectPtr.h"

class AGameModeBase;
class APlayerController;
class FFlockMultiplayerSessions;
class UFlockMultiplayerSession;

/** What the engine says of the game's networking at one moment, as a travel reads it each frame. */
struct FFlockGameNetState
{
	/** False once the game has no world (it is ending). */
	bool bHasWorld = false;
	/** True once the world the travel started from has been replaced by a level opening or a connection to a host. */
	bool bInAnotherWorld = false;
	/** A level is opening, or a connection to a host is being made. */
	bool bChangingLevel = false;
	ENetMode NetMode = NM_Standalone;
	/** The port the world's net driver listens on; 0 when it does not listen. */
	int32 ListeningPort = 0;
	/** The world is a client whose connection to the host is open and whose player the host has let in. */
	bool bLetInByHost = false;
	/** Where the world's connection to a host goes, or the one being made, as "address:port"; empty when there is none. */
	FString Destination;
	/** A connection to a host is being made: the engine's pending connection. */
	bool bConnecting = false;
	/** A connection being made to a host has heard the host's engine answer (its challenge, before the host's join check). */
	bool bHostAnswered = false;
};

/** The engine as one travel uses it: the game's own engine, or a stand-in for tests. Game-thread only. */
class IFlockGameTravel
{
public:
	virtual ~IFlockGameTravel() = default;

	virtual FFlockGameNetState Read() const = 0;
	/** The level the game is in, as a level opening names it. */
	virtual FString CurrentLevel() const = 0;
	/** Remembers the world now, so Read says once it is replaced. */
	virtual void MarkWorld() = 0;
	/** Opens Level listening for players (the engine's ?listen). */
	virtual void OpenListening(const FString& Level) = 0;
	/** Travels to the host at Url ("address:port?options"). */
	virtual void TravelTo(const FString& Url) = 0;
	/** The world stops listening; it stays in its level, on its own. */
	virtual void StopListening() = 0;
	/** A connection to a host still being made is given up; the game stays in its level. */
	virtual void StopConnecting() = 0;
	/**
	 * Hands OnFailure each network or travel failure of this game's world or its connection, until this object goes: its kind
	 * (the engine's name for it, such as PendingConnectionFailure) and the engine's words.
	 */
	virtual void WatchFailures(TFunction<void(const FString& Kind, const FString& Words)> OnFailure) = 0;

	// ---- the host's side, read by the check of joining players ----

	/**
	 * The connection the engine is asking the game mode about: one waiting to be let in whose request carries exactly Options,
	 * from Address, and not handed out before; 0 when there is none. Each connection keeps its number while it lasts.
	 */
	virtual uint64 FindJoiningConnection(const FString& Options, const FString& Address) = 0;
	/** True while the connection is still on the host: waiting to be let in, or let in and not gone. */
	virtual bool IsStillHere(uint64 Connection) const = 0;
	/** The number of the connection Controller plays through, when it is one handed out; 0 otherwise (this device's own players have none). */
	virtual uint64 ConnectionOf(const APlayerController* Controller) const = 0;
	/** True for a player controller of this game's own, local to this device. */
	virtual bool IsOwnPlayer(const APlayerController* Controller) const = 0;
	/** True when the game mode runs in this game. */
	virtual bool IsThisGame(const AGameModeBase* GameMode) const = 0;
	/** The port of this device's loopback (127.0.0.1) a connection comes from, a relayed player's through the relay bridge; 0 for any other. */
	virtual int32 LoopbackPortOf(uint64 Connection) const = 0;
	/** True while any connection on the host, waiting to be let in or let in, comes from 127.0.0.1 on Port. */
	virtual bool IsAnyConnectionFromLoopbackPort(int32 Port) const = 0;
	/** Seconds since the host last heard from the connection (a player's engine sends at least every 0.2 s while it lasts); large once it is gone. */
	virtual double SecondsSinceHeardFrom(uint64 Connection) const = 0;
	/** The host closes the connection; the player's engine is told, if anything still reaches it. */
	virtual void CloseConnection(uint64 Connection) = 0;

	// ---- a player's side, while it is with its host ----

	/**
	 * Puts Token in place of the join token in the options of the connection being made to a host, so the login it sends once
	 * the host's engine answers carries it; false when no connection is being made, or it has logged in already.
	 */
	virtual bool PutJoinTokenOnTheConnectionBeingMade(const FString& Token) = 0;
};

/** Makes the engine side of a travel for the game WorldContextObject is in; null, with OutRefusal said, when it has no game world. */
using FFlockMakeGameTravel = TFunction<TSharedPtr<IFlockGameTravel>(UObject* WorldContextObject, FString& OutRefusal)>;

/** The engine's own side of a travel: the game instance's world context, its world and its net driver. */
FFlockMakeGameTravel FlockEngineTravel();

/**
 * The one owner of Travel To Session: one travel at a time, a newer one taking over (the one under way ends Restarted, its engine
 * side left as a cancel leaves it, and the newer begins once that has settled). Called again for the session the game plays
 * (on a host change, or once its relay was lost), a travel does this device's part from where the game is: a host already
 * listening for the session keeps its level, a player connected to the old host waits for the new one there, and a host that
 * is now a player stops listening first. The host turns on the check of joining players, opens its level
 * listening, opens its address on Flock's relay unless told not to (Options.Relay), then publishes how to reach it on the port
 * its net driver listens on. A player waits for that, then connects to the host on its network or through the relay: a host on
 * the relay is reached through a bridge on this device's loopback standing in for it, so the engine's own net driver connects as
 * on a direct connection. It asks Flock for a join token and travels with it, and is done once the host has let it in or turned
 * it away. The engine owns a failed connection (it gives the connection up and goes back to the game's default level), so a
 * travel only reads what happened.
 *
 * Once a player is in, it stays with its host: its relay bridge lasts while the game is connected or connecting to that host
 * (a host's level change without seamless travel sends every player back to it through a new connection of the engine's own),
 * and it keeps a fresh join token on hand that goes on each such connection before its login, since the engine would send the
 * first one again and Flock refuses a token past its 60 s. Moved on once a frame. Game-thread only.
 */
class FFlockTravels : public TSharedFromThis<FFlockTravels>
{
public:
	using FOnTravel = TFunction<void(TFlockResult<FFlockTravelToSessionResult>)>;

	/**
	 * How long a When Needed player tries the host's address on its network before going through Flock's relay: two of the
	 * engine's connect attempts (its handshake is sent again every second, read in StatelessConnectHandlerComponent), and 3 s.
	 */
	static constexpr double TryOnTheHostsNetworkSeconds = 3.0;
	/**
	 * While a player is with its host it keeps a join token on hand for the engine's connection back after a host's level change,
	 * replaced once it has less than this left: every 40 s of a 60 s token, about 1.5 requests a minute (owner, 2026-10-18).
	 */
	static constexpr double ReplaceTokenWithSecondsLeft = 20.0;
	/** The least a token on hand must have left to go on a connection back; with less, a new one is asked for that connection. */
	static constexpr double UseTokenWithSecondsLeft = 10.0;
	/** After Flock gave no token, the next is asked this much later (at once for a connection back that needs one). */
	static constexpr double AskForATokenAgainAfterSeconds = 10.0;
	/**
	 * How long a travel waits for the engine to finish a level change or a connection it is making before going on (the engine's
	 * own way out of a lost connection takes a frame; a replaced travel's level opening, a load).
	 */
	static constexpr double SettleWithinSeconds = 30.0;

	FFlockTravels(const TWeakPtr<FFlockMultiplayerSessions>& InSessions, const TSharedRef<IFlockLogger>& InLogger, TFunction<double()> InNow);

	FFlockRequestHandle Travel(UFlockMultiplayerSession* Session, UObject* WorldContextObject, const FFlockTravelToSessionOptions& Options,
		FOnTravel OnComplete);

	/** Reads where the travel's engine side is and moves it on, and keeps a player with its host; run once a frame. */
	void Tick();

	/** Flock is shutting down: a travel ends Cancelled, its engine side left as it is; a player's relay bridge closes. */
	void StopForShutdown();

	void SetGameTravelForTesting(FFlockMakeGameTravel Make) { MakeGameTravel = MoveTemp(Make); }
	bool IsTravelingForTesting() const { return Current.IsValid() || Next.IsValid(); }
	/** The relay bridge of the travel under way or of the host this player is with, or null. */
	FFlockRelayBridge::FPtr GetPlayersBridgeForTesting() const;

	/**
	 * Stands in, when not empty, for the host's address on this network that a When Needed player tries before the relay, so a
	 * real-player run on one machine reaches an address nobody answers (192.0.2.1), as a network that keeps its devices apart does.
	 */
	static FLOCK_API FString HostsNetworkAddressForTesting;
	bool IsWithTheHostForTesting() const { return WithTheHost.IsValid(); }

private:
	enum class EStep : uint8
	{
		Beginning, Settling, OpeningLevel, OpeningRelay, Publishing, WaitingForHost, FindingAddress, OpeningRelayToTheHost, PublishingSeatAddress,
		AskingForJoinToken, Connecting, StoppingTheTry, StoppingAfterCancel, Done
	};

	struct FTravel
	{
		EStep Step = EStep::OpeningLevel;
		TStrongObjectPtr<UFlockMultiplayerSession> Session;
		TSharedPtr<IFlockGameTravel> Game;
		/** A host's engine side for the check of joining players, which outlives the travel. */
		TSharedPtr<IFlockGameTravel> HostSide;
		FFlockTravelToSessionOptions Options;
		FString Level;
		FFlockTravelToSessionResult Result;
		FOnTravel OnComplete;
		/** The publish, wait, address search, relay opening or join token on its way, cancelled with the travel. */
		FFlockRequestHandle OnItsWay;
		/** The kind and the engine's words of the first failure it told, if any. */
		FString Failure;
		/** Why the host turned this player away, when the first failure was the host's refusal. */
		FString RefusedReason;
		bool bRefused = false;
		/** Where the engine connects: the host's address, or the relay bridge on 127.0.0.1. */
		FString ConnectAddress;
		int32 ConnectPort = 0;
		/** A player's: the host's address on the relay and the relay server it is on, when it published one. */
		FFlockRelayPeer HostOnTheRelay;
		FString RelayServer;
		/** A player's: the host's relay connection says its bridge reads the bridges' own messages, and names direct addresses (a host that offers direct paths). */
		bool bHostReadsBridgeMessages = false;
		bool bHostOffersDirectPaths = false;
		/** The session's relay this travel uses: the host's for its players, a player's to reach its host. */
		TSharedPtr<FFlockRelayConnection> Relay;
		/** A player's bridge standing in for the host, kept once it is in and closed otherwise. */
		FFlockRelayBridge::FPtr Bridge;
		/** A When Needed player trying the host's address on its network: given up at this time unless the host answered. */
		bool bTryingTheHostsNetwork = false;
		double TryUntil = 0.0;
		/** The join token the travel connects with, and until when it is good (from when it was asked for). */
		FString JoinToken;
		double JoinTokenGoodUntil = 0.0;
		bool bHostAnswered = false;
		/**
		 * The game was in this session's networking when the travel was asked for: with its host, or brought there by the travel
		 * this one replaced. It may then go on from the host's level, which its connection to the new host replaces.
		 */
		bool bFromTheSession = false;
		/** Where the game was connected for the session when the travel was asked for ("address:port"); empty when nowhere. */
		FString FromDestination;
		/** The host kept the level it was already listening on (it did not open it), so ending this travel leaves it listening. */
		bool bKeptHosting = false;
		/** A newer travel took over: what this one started is left for the newer one to decide (it hosts on, or stops). */
		bool bRestarted = false;
		/** The player was already in the host's game; it stays with that host as it was. */
		bool bAlreadyIn = false;
		/** While Settling: what goes on once the engine has settled, and until when it may take. */
		TFunction<void(const TSharedRef<FTravel>&)> OnceSettled;
		double SettleBy = 0.0;
	};
	using FTravelRef = TSharedRef<FTravel>;

	/** A player that is in its host's game: the engine side it watches, the host it is with and the bridge that reaches it. */
	struct FWithTheHost
	{
		TStrongObjectPtr<UFlockMultiplayerSession> Session;
		TSharedPtr<IFlockGameTravel> Game;
		FString Destination;
		FFlockRelayBridge::FPtr Bridge;
		/** A join token kept fresh for the engine's connection back to the host, and until when it is good. */
		FString TokenOnHand;
		double TokenOnHandGoodUntil = 0.0;
		/** When the next token may be asked for, after Flock gave none. */
		double NextAskAt = 0.0;
		/** The one token request on its way, and when it was sent. */
		FFlockRequestHandle TokenOnItsWay;
		bool bTokenOnItsWay = false;
		double TokenAskedAt = 0.0;
		/** The connection the engine is making to the host again has been given a token, or waits for the one on its way. */
		bool bConnectionHandled = false;
		bool bConnectionWaitsForAToken = false;
		/** The level the game plays with the host, kept for the session once it leaves: a player that becomes the host opens it. */
		FString Level;
		/** The host the player connected to, and how, for a travel asked for again while nothing changed. */
		FString HostPlayerId;
		FFlockTravelToSessionResult Result;
		/** The host was found on another relay address than the one this player's bridge reaches (said once). */
		bool bToldHostMoved = false;
	};

	/** Does this device's part as the session says now (host or player), from where the game is, once the engine has settled. */
	void Begin(const FTravelRef& Travel);
	/** Then runs at once when the engine is not changing level, and once it has finished otherwise (at most SettleWithinSeconds). */
	void WhenSettled(const FTravelRef& Travel, TFunction<void(const FTravelRef&)> Then);
	void MoveSettlingOn(const FTravelRef& Travel, const FFlockGameNetState& State);
	/**
	 * The newest travel asked for takes Current over, the one it replaced having answered. A level that one was opening is the
	 * newer one's: it waits for the engine to settle, as every travel does, which owns not beginning while a level opens.
	 */
	void BeginTheNext();
	/** Ends a travel before its time with Answer, its engine side as a cancel leaves it: a level opening stops listening once open. */
	void EndEarly(const FTravelRef& Travel, const TFlockResult<FFlockTravelToSessionResult>& Answer);
	/** True when the game can travel to a host: on its own, or still in the level of the host it played the session with. */
	static bool IsFreeToTravel(const FTravel& Travel, const FFlockGameNetState& State);
	/** True when the game is a client of the host it played the session with, as the travel found it. */
	static bool IsInTheHostsLevel(const FTravel& Travel, const FFlockGameNetState& State);
	/** True when the player is in the game of the host the session names now, on the address it published: nothing to do again. */
	bool IsInTheHostsGame(const UFlockMultiplayerSession& Session, const FFlockGameNetState& State) const;
	/** True when the session's host is on another relay address than the one With's bridge reaches. */
	static bool HostMovedOnTheRelay(const FWithTheHost& With, const UFlockMultiplayerSession& Session);
	void StartHosting(const FTravelRef& Travel);
	/** A host already listening for the session: its level and its players stay; its relay opens again if needed and it publishes. */
	void KeepHosting(const FTravelRef& Travel, int32 Port);
	/** A host listening on Port opens its relay address (unless Relay is Never) and publishes how to reach it. */
	void OpenToPlayers(const FTravelRef& Travel, int32 Port);
	/** The host's address on Flock's relay, opened to the relay's own IP (which every address of a relay that gives one IP shares). */
	void OpenRelayForPlayers(const FTravelRef& Travel);
	/** The relay could not be opened: Always stops hosting (relay_failed); When Needed publishes directly, saying why unless switched off. */
	void RelayCouldNotOpen(const FTravelRef& Travel, const FFlockRelayFailure& Failure);
	/**
	 * Publishes through OnTheRelay when given, directly otherwise, once the engine still listens on the port the travel saw: a host
	 * that stopped meanwhile is not published.
	 */
	void PublishForPlayers(const FTravelRef& Travel, const TSharedPtr<FFlockRelayConnection>& OnTheRelay);
	void StartWaiting(const FTravelRef& Travel);
	void TookConnection(const FTravelRef& Travel, UFlockMultiplayerSession* Session);
	/** Finds the host's direct address; When Needed tries it only when this device shares the host's public address. */
	void FindHostsAddress(const FTravelRef& Travel, bool bThenTheRelay);
	/** Reserves on the relay server the host uses, opens to the host's relay address, says it for the seat, and connects through a bridge. */
	void StartThroughTheRelay(const FTravelRef& Travel);
	void PublishSeatAddress(const FTravelRef& Travel);
	void ConnectThroughTheBridge(const FTravelRef& Travel);
	/** Flock's relay could not take this player to its host. */
	void RelayFailedFor(const FTravelRef& Travel, const FFlockRelayFailure& Failure);
	/** Asks Flock for the join token the host checks, once where to connect is known: a token lasts 60 s. */
	void StartConnecting(const FTravelRef& Travel, const FString& Address, int32 Port);
	void TravelWithToken(const FTravelRef& Travel, const FString& JoinToken);
	void MoveOpeningOn(const FTravelRef& Travel, const FFlockGameNetState& State);
	void MoveConnectingOn(const FTravelRef& Travel, const FFlockGameNetState& State);
	/** The try on the host's network has been given up: once the engine has settled, the relay. */
	void MoveTheStoppedTryOn(const FTravelRef& Travel, const FFlockGameNetState& State);
	void Cancel(const FTravelRef& Travel);
	/** Ends the travel with an outcome; the completion runs once the travel is no longer current, so it may start another. */
	void Complete(const FTravelRef& Travel, const FString& Outcome);
	void Fail(const FTravelRef& Travel, const FFlockError& Error);
	void Finish(const FTravelRef& Travel, const TFlockResult<FFlockTravelToSessionResult>& Result);
	/** "at a.b.c.d:port", or "through Flock's relay at a.b.c.d:port", for the logs. */
	static FString WhereOf(const FTravel& Travel);
	/** True when this game is on its own in a level that is not changing: neither connected, hosting nor traveling. */
	static bool IsOnItsOwn(const FFlockGameNetState& State);

	/** Keeps a player with its host: a connection made to it again gets a new join token, and the bridge closes once the game has left it. */
	void KeepWithTheHost();
	/** Asks for a join token when the one on hand runs low or a connection back waits for one: one request at a time. */
	void KeepATokenOnHand(const TSharedRef<FWithTheHost>& With, double Time);
	/** Puts Token on the connection being made to the host, said when the connection has logged in already. */
	void PutTokenOnTheConnectionBack(const TSharedRef<FWithTheHost>& With, const FString& Token);
	/** The connection back to the host goes with the token of the player's first travel, which its 60 s may have run out on. */
	void SayTheFirstTokenGoes(const FString& Why);
	/** The player is no longer with its host: its bridge closes, and a token still asked for is given up quietly. */
	void LeaveTheHost();

	TWeakPtr<FFlockMultiplayerSessions> Sessions;
	TSharedRef<IFlockLogger> Logger;
	TFunction<double()> Now;
	FFlockMakeGameTravel MakeGameTravel;
	TSharedPtr<FTravel> Current;
	/** The newest travel asked for, until the ones it replaces have answered (a travel asked for from their answer comes next). */
	TSharedPtr<FTravel> Next;
	/** The session the game last played and the level it played it in, for a player that becomes its host. */
	FString LevelPlayedSessionId;
	FString LevelPlayed;
	/** Where the game was connected to that host ("address:port"), so only a client of that host goes on from its level. */
	FString LastHostDestination;
	TSharedPtr<FWithTheHost> WithTheHost;
};
