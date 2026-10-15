// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "FlockLogger.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockResult.h"
#include "Models/FlockMultiplayerModels.h"
#include "UObject/StrongObjectPtr.h"

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
	/** Travels to the host at Address:Port. */
	virtual void TravelTo(const FString& Address, int32 Port) = 0;
	/** The world stops listening; it stays in its level, on its own. */
	virtual void StopListening() = 0;
	/** A connection to a host still being made is given up; the game stays in its level. */
	virtual void StopConnecting() = 0;
	/** Hands OnFailure the engine's words for each network or travel failure of this game's world or its connection; until this object goes. */
	virtual void WatchFailures(TFunction<void(const FString& Why)> OnFailure) = 0;
};

/** Makes the engine side of a travel for the game WorldContextObject is in; null, with OutRefusal said, when it has no game world. */
using FFlockMakeGameTravel = TFunction<TSharedPtr<IFlockGameTravel>(UObject* WorldContextObject, FString& OutRefusal)>;

/** The engine's own side of a travel: the game instance's world context, its world and its net driver. */
FFlockMakeGameTravel FlockEngineTravel();

/**
 * The one owner of Travel To Session: one travel at a time. The host opens its level listening, then publishes how to reach it
 * on the port its net driver listens on; a player waits for that, finds which address to use, travels there and is done once
 * the host has let it in. The engine owns a failed connection (it gives the connection up and goes back to the game's default
 * level), so a travel only reads what happened. Moved on once a frame. Game-thread only.
 */
class FFlockTravels : public TSharedFromThis<FFlockTravels>
{
public:
	using FOnTravel = TFunction<void(TFlockResult<FFlockTravelToSessionResult>)>;

	FFlockTravels(const TWeakPtr<FFlockMultiplayerSessions>& InSessions, const TSharedRef<IFlockLogger>& InLogger);

	FFlockRequestHandle Travel(UFlockMultiplayerSession* Session, UObject* WorldContextObject, const FFlockTravelToSessionOptions& Options,
		FOnTravel OnComplete);

	/** Reads where the travel's engine side is and moves it on; run once a frame. */
	void Tick();

	/** Flock is shutting down: a travel ends Cancelled, its engine side left as it is. */
	void StopForShutdown();

	void SetGameTravelForTesting(FFlockMakeGameTravel Make) { MakeGameTravel = MoveTemp(Make); }
	bool IsTravelingForTesting() const { return Current.IsValid(); }

private:
	enum class EStep : uint8 { OpeningLevel, Publishing, WaitingForHost, FindingAddress, Connecting, StoppingAfterCancel, Done };

	struct FTravel
	{
		EStep Step = EStep::OpeningLevel;
		TStrongObjectPtr<UFlockMultiplayerSession> Session;
		TSharedPtr<IFlockGameTravel> Game;
		FFlockTravelToSessionOptions Options;
		FString Level;
		FFlockTravelToSessionResult Result;
		FOnTravel OnComplete;
		/** The publish, wait or address search on its way, cancelled with the travel. */
		FFlockRequestHandle OnItsWay;
		/** The engine's words for the failure it told, if any. */
		FString Failure;
	};
	using FTravelRef = TSharedRef<FTravel>;

	void StartHosting(const FTravelRef& Travel);
	void StartWaiting(const FTravelRef& Travel);
	void TookConnection(const FTravelRef& Travel, UFlockMultiplayerSession* Session);
	void StartConnecting(const FTravelRef& Travel, const FFlockDirectAddress& Address);
	void MoveOpeningOn(const FTravelRef& Travel, const FFlockGameNetState& State);
	void MoveConnectingOn(const FTravelRef& Travel, const FFlockGameNetState& State);
	void Cancel(const FTravelRef& Travel);
	/** Ends the travel with an outcome; the completion runs once the travel is no longer current, so it may start another. */
	void Complete(const FTravelRef& Travel, const FString& Outcome);
	void Fail(const FTravelRef& Travel, const FFlockError& Error);
	void Finish(const FTravelRef& Travel, const TFlockResult<FFlockTravelToSessionResult>& Result);
	/** True when this game is on its own in a level that is not changing: neither connected, hosting nor traveling. */
	static bool IsOnItsOwn(const FFlockGameNetState& State);

	TWeakPtr<FFlockMultiplayerSessions> Sessions;
	TSharedRef<IFlockLogger> Logger;
	FFlockMakeGameTravel MakeGameTravel;
	TSharedPtr<FTravel> Current;
};
