// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockResult.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockJsonData.h"
#include "Models/FlockMultiplayerModels.h"
#include "FlockMultiplayerSession.generated.h"

class APlayerController;
class FFlockAuthSession;
class FFlockMultiplayerSessions;
class UFlockMultiplayerSession;

/** A session call's completion: the session as it stands once the call is done (ended, after a leave or an end). */
using FFlockOnMultiplayerSession = TFunction<void(TFlockResult<UFlockMultiplayerSession*>)>;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFlockMultiplayerSessionChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockMultiplayerSessionEnded, const FString&, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockMultiplayerSessionRelayLost, const FString&, Reason);

/** What a newer reading changed in a session, worked out when it is taken and raised once the SDK's own state is settled. */
struct FFlockMultiplayerSessionChanges
{
	bool bPlayers = false;
	bool bHost = false;
	bool bConnection = false;
};

/**
 * A multiplayer session the player holds a seat in. While it lasts the SDK keeps the seat with heartbeats at the interval the
 * server gives, and every heartbeat's answer and every call's updates it, so the game needs no reads of its own. The SDK keeps
 * it alive while it is the player's; a game that keeps one past its end holds it in a UPROPERTY. Game-thread only.
 */
UCLASS(BlueprintType)
class FLOCK_API UFlockMultiplayerSession : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	FString GetId() const { return Id; }

	/** The code other players join with (Join Session). Letter case does not matter. */
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	FString GetJoinCode() const { return JoinCode; }

	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	FString GetHostPlayerId() const { return HostPlayerId; }

	/** True when the signed-in player hosts: only the host can publish the connection, end the session, hand hosting over or verify join tokens. */
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	bool IsHost() const { return HostPlayerId.Equals(PlayerId, ESearchCase::CaseSensitive); }

	/** The players seated now, longest seated first: when the host leaves, the first of the others hosts. */
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	TArray<FFlockMultiplayerSessionPlayer> GetPlayers() const { return Players; }

	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	int32 GetMaxPlayers() const { return MaxPlayers; }

	/** The game's own data on the session, set when it was hosted; its keys as sent. */
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	FFlockJsonData GetData() const { return Data; }

	/** True once the host has published how to reach it. False again when hosting moves, until the new host publishes. */
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	bool HasConnection() const { return bHasConnection; }

	/** How to reach the host, as it published it; empty while Has Connection is false. */
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	FFlockMultiplayerSessionConnection GetConnection() const { return Connection; }

	/**
	 * Empty while the session is the player's; why it stopped being so once it has ended: a FlockMultiplayerSessionEndReasons
	 * value, or a reason the server added.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	FString GetEndReason() const;

	/** True once the session is no longer the player's here; its calls are then refused and its heartbeats stop. */
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	bool HasEnded() const { return !GetEndReason().IsEmpty(); }

	/** The seated players changed. Raised with the session already up to date. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Multiplayer")
	FFlockMultiplayerSessionChanged OnPlayersChanged;

	/** Another player hosts the session now. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Multiplayer")
	FFlockMultiplayerSessionChanged OnHostChanged;

	/** The host published how to reach it or published again, or hosting moved and the old address was cleared. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Multiplayer")
	FFlockMultiplayerSessionChanged OnConnectionChanged;

	/** The session stopped being the player's, raised once with the reason, the session already ended. Not raised when Flock shuts down or the game quits. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Multiplayer")
	FFlockMultiplayerSessionEnded OnEnded;

	/**
	 * This device's address on Flock's relay stopped working while the session lasts, raised once a relay with why (a
	 * FlockRelayFailureReasons value, such as unreachable): whatever went through it, a host's relayed players or a player's
	 * connection to its host, is gone. On a player, raised too when its host publishes another relay address (the host's own
	 * relay was lost), since this player's relay no longer reaches it; a new host is On Host Changed's. Call Travel To Session
	 * again, which opens another. Not raised for a relay the SDK gave back (the session ended, the player left, or Flock shut
	 * down).
	 */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Multiplayer")
	FFlockMultiplayerSessionRelayLost OnRelayLost;

	/** Gives up the player's seat. If the server says the player is already out, that counts as leaving. When the host leaves, the longest seated of the others hosts. */
	FFlockRequestHandle Leave(FFlockOnMultiplayerSession OnComplete);

	/** Ends the session for every player. Host only. */
	FFlockRequestHandle End(FFlockOnMultiplayerSession OnComplete);

	/** Hands hosting to another seated player, who then publishes their own connection. Host only. */
	FFlockRequestHandle MakeHost(const FString& NewHostPlayerId, FFlockOnMultiplayerSession OnComplete);

	/**
	 * Says how to reach the host: a mode (32 characters at most) and the values it needs, under 4 KB in all; the mode is sent
	 * with the values. Replaces what was published before. Host only.
	 */
	FFlockRequestHandle PublishConnection(const FString& Mode, const FFlockCommandData& Values, FFlockOnMultiplayerSession OnComplete);

	/** A token proving the player holds a seat, to send the host when connecting; good for 60 s. */
	FFlockRequestHandle RequestJoinToken(TFunction<void(TFlockResult<FFlockMultiplayerSessionJoinToken>)> OnComplete);

	/** Checks a token a connecting player sent and answers that player's id; refused coded multiplayer.invalid_join_token when it is not good for this session. Host only. */
	FFlockRequestHandle VerifyJoinToken(const FString& Token, TFunction<void(TFlockResult<FString>)> OnComplete);

	/**
	 * Publishes how to reach this device directly on Port, the port its game listens on: its public address (one request to
	 * the STUN server Flock lists) or, when that is unknown, its LAN address, with both as found. Host only. Outside a LAN,
	 * players reach it only when the port is forwarded to this device. Travel To Session does this for a host itself.
	 */
	FFlockRequestHandle PublishDirectConnection(int32 Port, FFlockOnMultiplayerSession OnComplete);

	/**
	 * Waits for the host to publish how to reach it, reading the session every 2 s meanwhile, and answers the session: Has
	 * Connection is true once the host has published, Has Ended once the session ended, and neither when TimeoutSeconds ran
	 * out. A host change clears the address without ending the wait. Answers at once when the host has already published.
	 */
	FFlockRequestHandle WaitForConnection(float TimeoutSeconds, FFlockOnMultiplayerSession OnComplete);

	/**
	 * Where this device connects to reach the host's direct connection: the host's LAN address when this device shares its
	 * public address (one request to the STUN server Flock lists says), the published address otherwise. Its Address is empty
	 * when the host published no direct IPv4 address and port.
	 */
	FFlockRequestHandle FindDirectAddress(TFunction<void(TFlockResult<FFlockDirectAddress>)> OnComplete);

	/**
	 * Brings this game into the session's game. The host opens Options.HostLevel listening for players (the engine's ?listen)
	 * and, once it listens, opens its address on Flock's relay (unless Options.Relay is Never) and publishes how to reach it. A
	 * player waits for that (Wait For Host Seconds), then travels to the host's LAN address when both share a public address,
	 * through Flock's relay when the host offers it and this device is elsewhere (or the host did not answer on its network),
	 * and to the published address otherwise, and completes once the host has let it in. Either way the completion runs in the
	 * new level: everything of the level the call was made from is gone by then. How it ended is the result's Outcome
	 * (FlockTravelToSessionOutcomes); it fails only for a mistake (no world, an ended session, this game connected to another
	 * session's host or to a server of its own, hosting another session, a level still opening 30 s on), a publish the server
	 * refused (a host stops listening on a level it opened for this travel), a join token Flock would not give (nothing travels
	 * without one), or a cancel. Cancelling the handle stops a host listening on a level it opened and a player's connection
	 * still being made. Called again, a newer travel takes over (the one under way ends Restarted), doing this device's part for
	 * the session from where the game is (a player already in the host's game is answered Connected at once): on On Host
	 * Changed and On Relay Lost, call it again with the same options. WorldContextObject is anything in the game's world, such
	 * as an actor.
	 */
	FFlockRequestHandle TravelToSession(UObject* WorldContextObject, const FFlockTravelToSessionOptions& Options,
		TFunction<void(TFlockResult<FFlockTravelToSessionResult>)> OnComplete);

	/**
	 * The Flock player behind one of this game's player controllers while it hosts this session through Travel To Session: the
	 * player Flock verified for a joined player's connection (FlockJoinChecks::CheckJoiningPlayer), and this device's own player
	 * for its own controller. Empty for a controller whose connection is gone or was not let in by Flock's check, and while this
	 * game does not host this session through Travel To Session.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	FString PlayerForConnection(const APlayerController* PlayerController) const;

private:
	friend class FFlockMultiplayerSessions;
	friend class FFlockJoinChecksOnHost;

	/** Fills a session the owner has just made; called once, before anyone else sees it. */
	void Begin(const TWeakPtr<FFlockMultiplayerSessions>& InOwner, const TSharedRef<FFlockAuthSession>& InSession, int32 InSignInNumber,
		const FString& InPlayerId, const FFlockMultiplayerSessionReading& Reading);

	/** Takes a newer reading and says what it changed; nothing is raised until the owner calls RaiseChanges. */
	FFlockMultiplayerSessionChanges TakeReading(const FFlockMultiplayerSessionReading& Reading);

	/** Raises what a reading changed, once the owner has everything in place (its heartbeats included). */
	void RaiseChanges(const FFlockMultiplayerSessionChanges& Changes);

	/** The last reading of a session that has ended for the player: kept for what it says, without raising changes. */
	void TakeFinalReading(const FFlockMultiplayerSessionReading& Reading) { CopyFrom(Reading); }

	/** Set once, by the owner. */
	void MarkEnded(const FString& Reason, bool bRaise);

	/** Raised by the owner, once a relay. */
	void RaiseRelayLost(const FString& Reason) { OnRelayLost.Broadcast(Reason); }

	void CopyFrom(const FFlockMultiplayerSessionReading& Reading);

	/** The session's calls when its owner is gone (Flock shut down): refused, as for any session that has ended. */
	static FFlockError RefusedWithoutOwner();

	TWeakPtr<FFlockMultiplayerSessions> Owner;
	TWeakPtr<FFlockAuthSession> AuthSession;
	int32 SignInNumber = 0;
	FString PlayerId;
	/** Set once, by the owner, when the session ends; a sign-in that ended reads as signed out before then. */
	FString EndReasonSet;

	FString Id;
	FString JoinCode;
	FString HostPlayerId;
	int32 HostEpoch = 0;
	TArray<FFlockMultiplayerSessionPlayer> Players;
	int32 MaxPlayers = 0;
	FFlockJsonData Data;
	bool bHasConnection = false;
	FFlockMultiplayerSessionConnection Connection;
	int32 ConnectionEpoch = 0;
	/** How often to heartbeat, as the server says for the game's timeouts. */
	double HeartbeatIntervalSeconds = 0.0;
	/** The seated players whose seat names an address on Flock's relay, as the last reading says: the host reads every seat's, a player its own. */
	TArray<FFlockMultiplayerSessionSeat> SeatsOnTheRelay;

	/** The relay address Player's seat names, or empty; player ids compared letter for letter. */
	FString SeatRelayAddressOf(const FString& Player) const;
};
