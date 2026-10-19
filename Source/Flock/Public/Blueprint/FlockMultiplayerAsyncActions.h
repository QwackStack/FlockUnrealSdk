// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Engine/CancellableAsyncAction.h"
#include "Http/FlockError.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockResult.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockMultiplayerModels.h"
#include "FlockMultiplayerAsyncActions.generated.h"

class FFlockMultiplayerProvider;
class UFlockMultiplayerSession;
class UFlockParty;

/**
 * Blueprint async nodes for multiplayer: parties, sessions, searching for a match and travelling into the session's game. Each
 * fires exactly one of its pins, and the game instance keeps it until then, so a node still waiting is never lost to garbage
 * collection. A node that acts on a party or a session takes it as its first input; given none, its failure pin fires and
 * nothing is sent. The others find Flock from their world context; with Flock not initialized their failure pin fires.
 *
 * Find Match, Party Find Match, Wait For Connection and Travel To Session can be cancelled: their Async Action pin has a Cancel
 * function. A cancel the game asks for fires no pin; one Flock makes itself (Flock shutting down) fires the failure pin.
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockPartyPin, UFlockParty*, Party, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockMultiplayerSessionPin, UFlockMultiplayerSession*, Session, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockMatchmakingResultPin, const FFlockMatchmakingResult&, Result, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockJoinTokenPin, const FFlockMultiplayerSessionJoinToken&, JoinToken, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockVerifiedPlayerPin, const FString&, PlayerId, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockDirectAddressPin, const FFlockDirectAddress&, DirectAddress, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockTravelToSessionPin, const FFlockTravelToSessionResult&, Result, const FFlockError&, Error);

/** Which party call a UFlockPartyAction makes. */
enum class EFlockPartyCall : uint8
{
	Create,
	Join,
	Leave,
	Kick,
	MakeLeader,
	Update,
	Disband,
};

/** Which session call a UFlockSessionAction makes. */
enum class EFlockSessionCall : uint8
{
	Host,
	Join,
	Get,
	Leave,
	End,
	MakeHost,
	PublishConnection,
	PublishDirectConnection,
};

/** The party calls that answer the party: make one, join one, and what its players do with it. */
UCLASS()
class FLOCK_API UFlockPartyAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The call is done: the party as it stands now (ended, after a leave or disband). */
	UPROPERTY(BlueprintAssignable)
	FFlockPartyPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockPartyPin OnFailure;

	/**
	 * Makes a party with the signed-in player as its leader and only member. Max Size 0 takes the server's default (4); it takes
	 * 2 to 64 players. Settings are the game's own data on the party, such as a mode, which its players read. A player can be
	 * in one party at a time.
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Create Party", AutoCreateRefTerm = "Settings"), Category = "Flock|Multiplayer")
	static UFlockPartyAction* CreateParty(UObject* WorldContextObject, int32 MaxSize, const FFlockCommandData& Settings);

	/** Joins the party with this invite code; letter case does not matter. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Join Party"), Category = "Flock|Multiplayer")
	static UFlockPartyAction* JoinParty(UObject* WorldContextObject, const FString& InviteCode);

	/** Leaves the party. When the leader leaves, the first of the others leads; the last one out disbands it. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Leave Party"), Category = "Flock|Multiplayer")
	static UFlockPartyAction* LeaveParty(UObject* WorldContextObject, UFlockParty* Party);

	/** Removes another player from the party. Leader only. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Kick From Party"), Category = "Flock|Multiplayer")
	static UFlockPartyAction* KickFromParty(UObject* WorldContextObject, UFlockParty* Party, const FString& PlayerId);

	/** Hands leadership to another player in the party. Leader only. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Make Party Leader"), Category = "Flock|Multiplayer")
	static UFlockPartyAction* MakePartyLeader(UObject* WorldContextObject, UFlockParty* Party, const FString& PlayerId);

	/**
	 * Changes the party's size, its settings or both. New Max Size 0 keeps the size; Replace Settings off keeps the settings,
	 * and on replaces all of them with New Settings. Leader only.
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Update Party", AutoCreateRefTerm = "NewSettings"), Category = "Flock|Multiplayer")
	static UFlockPartyAction* UpdateParty(UObject* WorldContextObject, UFlockParty* Party, int32 NewMaxSize, bool bReplaceSettings,
		const FFlockCommandData& NewSettings);

	/** Disbands the party, so every player in it is free to make or join another. Leader only. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Disband Party"), Category = "Flock|Multiplayer")
	static UFlockPartyAction* DisbandParty(UObject* WorldContextObject, UFlockParty* Party);

	virtual void Activate() override;

	/** Activate with a provider the test built, in place of the one this node finds from its world context. */
	void ActivateWithProviderForTesting(FFlockMultiplayerProvider* Provider);

private:
	void Start(FFlockMultiplayerProvider* Provider);
	void Complete(const TFlockResult<UFlockParty*>& Result);

	/** Weak: a node can wait minutes, and must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;

	UPROPERTY()
	TObjectPtr<UFlockParty> Party;

	EFlockPartyCall Call = EFlockPartyCall::Create;
	FString InviteCode;
	/** The player the leader removes or hands leadership to. */
	FString OtherPlayerId;
	/** Max Size or New Max Size; 0 leaves it unset. */
	int32 MaxSize = 0;
	bool bReplaceSettings = false;
	FFlockCommandData Settings;
};

/** The player's party, read now. */
UCLASS()
class FLOCK_API UFlockGetMyPartyAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The player is in this party. The same object each time while it lasts. */
	UPROPERTY(BlueprintAssignable)
	FFlockPartyPin OnParty;

	/** The player is in no party. */
	UPROPERTY(BlueprintAssignable)
	FFlockPartyPin OnNoParty;

	UPROPERTY(BlueprintAssignable)
	FFlockPartyPin OnFailure;

	/** The signed-in player's party, read now. A party lasts until the player leaves it, across launches, so call this after signing in. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get My Party"), Category = "Flock|Multiplayer")
	static UFlockGetMyPartyAction* GetMyParty(UObject* WorldContextObject);

	virtual void Activate() override;

	/** Activate with a provider the test built, in place of the one this node finds from its world context. */
	void ActivateWithProviderForTesting(FFlockMultiplayerProvider* Provider);

private:
	void Start(FFlockMultiplayerProvider* Provider);
	void Complete(const TFlockResult<UFlockParty*>& Result);

	/** Weak: a node can wait minutes, and must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;
};

/** The session calls that answer the session: host one, take a seat in one, read one, and what its players do with it. */
UCLASS()
class FLOCK_API UFlockSessionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The call is done: the session as it stands now (ended, after a leave or an end). */
	UPROPERTY(BlueprintAssignable)
	FFlockMultiplayerSessionPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockMultiplayerSessionPin OnFailure;

	/**
	 * Hosts a session with the signed-in player as its host and only player. Max Players 0 takes the game's own player count
	 * from the dashboard (8 unless changed there); it takes 2 to 16. Data is the game's own data on the session, such as a map.
	 * With Same Version Only, only players on the host's Game Version can join. A player holds one seat per game, so a session
	 * held until now ends as moved_to_another_session.
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Host Session", AutoCreateRefTerm = "Data"), Category = "Flock|Multiplayer")
	static UFlockSessionAction* HostSession(UObject* WorldContextObject, int32 MaxPlayers, const FFlockCommandData& Data, bool bSameVersionOnly = true);

	/** Takes a seat in the session with this join code; letter case does not matter. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Join Session"), Category = "Flock|Multiplayer")
	static UFlockSessionAction* JoinSession(UObject* WorldContextObject, const FString& JoinCode);

	/** A session the player holds or once held a seat in, read now; one the player is no longer seated in comes back ended. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Session"), Category = "Flock|Multiplayer")
	static UFlockSessionAction* GetSession(UObject* WorldContextObject, const FString& SessionId);

	/** Gives up the player's seat. When the host leaves, the longest seated of the others hosts. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Leave Session"), Category = "Flock|Multiplayer")
	static UFlockSessionAction* LeaveSession(UObject* WorldContextObject, UFlockMultiplayerSession* Session);

	/** Ends the session for every player. Host only. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock End Session"), Category = "Flock|Multiplayer")
	static UFlockSessionAction* EndSession(UObject* WorldContextObject, UFlockMultiplayerSession* Session);

	/** Hands hosting to another seated player, who then publishes their own connection. Host only. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Make Session Host"), Category = "Flock|Multiplayer")
	static UFlockSessionAction* MakeSessionHost(UObject* WorldContextObject, UFlockMultiplayerSession* Session, const FString& PlayerId);

	/**
	 * Says how to reach the host: a mode (32 characters at most) and the values it needs, under 4 KB in all. Replaces what was
	 * published before. Host only. Travel To Session publishes for a host itself.
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Publish Connection", AutoCreateRefTerm = "Values"), Category = "Flock|Multiplayer")
	static UFlockSessionAction* PublishConnection(UObject* WorldContextObject, UFlockMultiplayerSession* Session, const FString& Mode,
		const FFlockCommandData& Values);

	/**
	 * Publishes how to reach this device directly on Port, the port its game listens on: its public address, or its LAN address
	 * when that is unknown. Host only. Outside a LAN, players reach it only when the port is forwarded to this device.
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Publish Direct Connection"), Category = "Flock|Multiplayer")
	static UFlockSessionAction* PublishDirectConnection(UObject* WorldContextObject, UFlockMultiplayerSession* Session, int32 Port);

	virtual void Activate() override;

	/** Activate with a provider the test built, in place of the one this node finds from its world context. */
	void ActivateWithProviderForTesting(FFlockMultiplayerProvider* Provider);

private:
	void Start(FFlockMultiplayerProvider* Provider);
	void Complete(const TFlockResult<UFlockMultiplayerSession*>& Result);

	/** Weak: a node can wait minutes, and must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;

	UPROPERTY()
	TObjectPtr<UFlockMultiplayerSession> Session;

	EFlockSessionCall Call = EFlockSessionCall::Host;
	FString JoinCode;
	FString SessionId;
	/** The player the host hands hosting to. */
	FString OtherPlayerId;
	/** 0 leaves it unset: the game's own count. */
	int32 MaxPlayers = 0;
	FFlockCommandData Data;
	bool bSameVersionOnly = true;
	/** The connection Publish Connection publishes. */
	FString Mode;
	FFlockCommandData Values;
	int32 Port = 0;
};

/** The session the player is seated in, found without its id. */
UCLASS()
class FLOCK_API UFlockGetMySessionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The player is seated in this session. The same object each time while it lasts. */
	UPROPERTY(BlueprintAssignable)
	FFlockMultiplayerSessionPin OnSession;

	/** The player is seated nowhere. */
	UPROPERTY(BlueprintAssignable)
	FFlockMultiplayerSessionPin OnNoSession;

	UPROPERTY(BlueprintAssignable)
	FFlockMultiplayerSessionPin OnFailure;

	/** The session the signed-in player is seated in: after a restart, or as a party member whose leader made the session. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get My Session"), Category = "Flock|Multiplayer")
	static UFlockGetMySessionAction* GetMySession(UObject* WorldContextObject);

	virtual void Activate() override;

	/** Activate with a provider the test built, in place of the one this node finds from its world context. */
	void ActivateWithProviderForTesting(FFlockMultiplayerProvider* Provider);

private:
	void Start(FFlockMultiplayerProvider* Provider);
	void Complete(const TFlockResult<UFlockMultiplayerSession*>& Result);

	/** Weak: a node can wait minutes, and must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;
};

/** A search for a match, alone or as a party. */
UCLASS()
class FLOCK_API UFlockFindMatchAction : public UCancellableAsyncAction
{
	GENERATED_BODY()

public:
	/** Matched: the result's Session is where the players play, held like any other; Player Ids lists them. */
	UPROPERTY(BlueprintAssignable)
	FFlockMatchmakingResultPin OnMatched;

	/**
	 * The search ended without a match: the result's Outcome says how (Flock Matchmaking Outcome Expired, Party Changed or
	 * Cancelled, or an outcome the server added).
	 */
	UPROPERTY(BlueprintAssignable)
	FFlockMatchmakingResultPin OnNoMatch;

	UPROPERTY(BlueprintAssignable)
	FFlockMatchmakingResultPin OnFailure;

	/**
	 * Searches for a match alone in the queue named exactly Queue Name (letter case included). A search the server ends without
	 * a match (nobody found within 5 minutes, or another game of this player stopping it) fires On No Match. Cancel stops the
	 * search on the server; a match that landed meanwhile gives its seat up.
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Find Match", AutoCreateRefTerm = "Options"), Category = "Flock|Multiplayer")
	static UFlockFindMatchAction* FindMatch(UObject* WorldContextObject, const FString& QueueName, const FFlockMatchmakingOptions& Options);

	/**
	 * Searches for a match as the party, matched as one unit and never split. The leader's game starts the search in Queue Name;
	 * another member's game waits for the leader's search (Queue Name may be empty) and gets the same match. A player joining
	 * or leaving the party stops the search (party_changed). Cancel stops the party's search for every member.
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Party Find Match", AutoCreateRefTerm = "Options"), Category = "Flock|Multiplayer")
	static UFlockFindMatchAction* PartyFindMatch(UObject* WorldContextObject, UFlockParty* Party, const FString& QueueName,
		const FFlockMatchmakingOptions& Options);

	virtual void Activate() override;
	virtual void Cancel() override;
	virtual bool IsActive() const override { return !bDone; }

	/** Activate with a provider the test built, in place of the one this node finds from its world context. */
	void ActivateWithProviderForTesting(FFlockMultiplayerProvider* Provider);

private:
	void Start(FFlockMultiplayerProvider* Provider);
	void Complete(const TFlockResult<FFlockMatchmakingResult>& Result);

	/** Weak: a node can wait minutes, and must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;

	UPROPERTY()
	TObjectPtr<UFlockParty> Party;

	bool bAsParty = false;
	FString QueueName;
	FFlockMatchmakingOptions Options;
	FFlockRequestHandle Searching;
	/** A pin has fired, or the game cancelled: nothing fires after. */
	bool bDone = false;
};

/** A token proving the player holds a seat in the session, to send the host when connecting. */
UCLASS()
class FLOCK_API UFlockRequestJoinTokenAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockJoinTokenPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockJoinTokenPin OnFailure;

	/** A token proving the player holds a seat, good for 60 s. Travel To Session asks for one itself. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Request Join Token"), Category = "Flock|Multiplayer")
	static UFlockRequestJoinTokenAction* RequestJoinToken(UObject* WorldContextObject, UFlockMultiplayerSession* Session);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockMultiplayerSessionJoinToken>& Result);

	/** Weak: a node can wait minutes, and must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;

	UPROPERTY()
	TObjectPtr<UFlockMultiplayerSession> Session;
};

/** The host's check of a token a connecting player sent. */
UCLASS()
class FLOCK_API UFlockVerifyJoinTokenAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The token is good for this session: the player it was given to. */
	UPROPERTY(BlueprintAssignable)
	FFlockVerifiedPlayerPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockVerifiedPlayerPin OnFailure;

	/**
	 * Checks a token a connecting player sent. Fails coded Multiplayer Invalid Join Token when it is not good for this session.
	 * Host only. A game mode based on Flock Game Mode Base, or one that calls Flock's check of joining players, does this itself.
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Verify Join Token"), Category = "Flock|Multiplayer")
	static UFlockVerifyJoinTokenAction* VerifyJoinToken(UObject* WorldContextObject, UFlockMultiplayerSession* Session, const FString& Token);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FString>& Result);

	/** Weak: a node can wait minutes, and must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;

	UPROPERTY()
	TObjectPtr<UFlockMultiplayerSession> Session;

	FString Token;
};

/** Where this device connects to reach the host's direct connection. */
UCLASS()
class FLOCK_API UFlockFindDirectAddressAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockDirectAddressPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockDirectAddressPin OnFailure;

	/**
	 * The host's LAN address when this device shares its public address, the published address otherwise. Its Address is empty
	 * when the host published no direct IPv4 address and port.
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Find Direct Address"), Category = "Flock|Multiplayer")
	static UFlockFindDirectAddressAction* FindDirectAddress(UObject* WorldContextObject, UFlockMultiplayerSession* Session);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockDirectAddress>& Result);

	/** Weak: a node can wait minutes, and must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;

	UPROPERTY()
	TObjectPtr<UFlockMultiplayerSession> Session;
};

/** Waits for the host to publish how to reach it. */
UCLASS()
class FLOCK_API UFlockWaitForConnectionAction : public UCancellableAsyncAction
{
	GENERATED_BODY()

public:
	/** The host has published how to reach it: read the session's Connection. */
	UPROPERTY(BlueprintAssignable)
	FFlockMultiplayerSessionPin OnConnection;

	/** The session ended while waiting. */
	UPROPERTY(BlueprintAssignable)
	FFlockMultiplayerSessionPin OnSessionEnded;

	/** Timeout Seconds ran out with nothing published. */
	UPROPERTY(BlueprintAssignable)
	FFlockMultiplayerSessionPin OnTimedOut;

	UPROPERTY(BlueprintAssignable)
	FFlockMultiplayerSessionPin OnFailure;

	/**
	 * Waits for the host to publish how to reach it, reading the session every 2 s meanwhile. A host change clears the address
	 * without ending the wait. Answers at once when the host has already published. Travel To Session waits itself.
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Wait For Connection"), Category = "Flock|Multiplayer")
	static UFlockWaitForConnectionAction* WaitForConnection(UObject* WorldContextObject, UFlockMultiplayerSession* Session, float TimeoutSeconds = 30.f);

	virtual void Activate() override;
	virtual void Cancel() override;
	virtual bool IsActive() const override { return !bDone; }

private:
	void Complete(const TFlockResult<UFlockMultiplayerSession*>& Result);

	/** Weak: a node can wait minutes, and must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;

	UPROPERTY()
	TObjectPtr<UFlockMultiplayerSession> Session;

	float TimeoutSeconds = 30.f;
	FFlockRequestHandle Waiting;
	/** A pin has fired, or the game cancelled: nothing fires after. */
	bool bDone = false;
};

/** Brings this game into the session's game: the host opens its level for the players, a player travels to the host. */
UCLASS()
class FLOCK_API UFlockTravelToSessionAction : public UCancellableAsyncAction
{
	GENERATED_BODY()

public:
	/** This game is in the session's game: it hosts it (Started As Host), or the host let it in (Connected). */
	UPROPERTY(BlueprintAssignable)
	FFlockTravelToSessionPin OnJoined;

	/**
	 * This game did not get in: the result's Outcome says why (Flock Travel Outcome ...), and for Refused its Refused Reason says
	 * why the host turned it away (Flock Refused Reason ..., or the host game's own reason).
	 */
	UPROPERTY(BlueprintAssignable)
	FFlockTravelToSessionPin OnNotJoined;

	/**
	 * A mistake (no world, an ended session, this game connected to another session's host or to a server of its own, hosting
	 * another session, a level still opening 30 s on), a publish the server refused, or a join token Flock would not give: the
	 * Error says why and how to fix it.
	 */
	UPROPERTY(BlueprintAssignable)
	FFlockTravelToSessionPin OnFailure;

	/**
	 * Brings this game into the session's game. The host opens Options' Host Level listening for players and publishes how to
	 * reach it; a player waits for that and travels to the host, which lets it in once Flock has checked its join token.
	 *
	 * Its pins fire once the level has changed, so call it from the Game Instance Blueprint (where the engine's own NetworkError
	 * and TravelError events are): a widget or an actor of the current level is gone by then, and never sees its result.
	 * Called from one, it warns in the log. Cancel stops a host listening on a level it opened and a player's connection still
	 * being made. Called again, the newer node takes over (this one fires On Not Joined, Outcome Restarted).
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Travel To Session", AutoCreateRefTerm = "Options"), Category = "Flock|Multiplayer")
	static UFlockTravelToSessionAction* TravelToSession(UObject* WorldContextObject, UFlockMultiplayerSession* Session,
		const FFlockTravelToSessionOptions& Options);

	virtual void Activate() override;
	virtual void Cancel() override;
	virtual bool IsActive() const override { return !bDone; }

	/** Activate with a provider the test built (for its logger), in place of the one this node finds from its world context. */
	void ActivateWithProviderForTesting(FFlockMultiplayerProvider* Provider);

	/** True when Caller belongs to a level, so it is gone once a travel has changed the level. */
	static bool IsGoneAfterTheLevelChanges(const UObject* Caller);

private:
	void Start(FFlockMultiplayerProvider* Provider);
	void Complete(const TFlockResult<FFlockTravelToSessionResult>& Result);

	/** Weak: a node can wait minutes, and must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;

	UPROPERTY()
	TObjectPtr<UFlockMultiplayerSession> Session;

	FFlockTravelToSessionOptions Options;
	FFlockRequestHandle Traveling;
	/** A pin has fired, or the game cancelled: nothing fires after. */
	bool bDone = false;
};
