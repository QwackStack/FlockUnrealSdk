// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockJsonData.h"
#include "UObject/ObjectPtr.h"
#include "FlockMultiplayerModels.generated.h"

class UFlockMultiplayerSession;

/** One row of the game's matchmaking queue list, as much of it as the SDK reads. */
USTRUCT()
struct FLOCK_API FFlockMatchmakingQueue
{
	GENERATED_BODY()

	UPROPERTY()
	FString Id;

	UPROPERTY()
	FString Name;
};

/** Why a party stopped being the player's. */
UENUM(BlueprintType)
enum class EFlockPartyEndReason : uint8
{
	/** The party has not ended. */
	None,
	/** The player left it with Leave. */
	Left,
	/** The player disbanded it with Disband. */
	Disbanded,
	/** The server no longer lists the player in it: removed by the leader, the party disbanded, or the player left it from another device. */
	Removed,
	/** The sign-in the party belonged to ended: a sign-out, another player signing in, or Flock shutting down. The player is still in the party on the server. */
	SignedOut,
};

/** One player in a party. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockPartyMember
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FString PlayerId;
};

/**
 * A party as a party route answers it. Only the two reads (the player's party, a party by id) carry its members, in the
 * order they joined; the other answers leave MemberIds empty. Settings are the game's own data, kept with its keys as sent.
 */
USTRUCT()
struct FLOCK_API FFlockPartyReading
{
	GENERATED_BODY()

	UPROPERTY()
	FString Id;

	UPROPERTY()
	FString LeaderPlayerId;

	UPROPERTY()
	FString InviteCode;

	UPROPERTY()
	int32 MaxSize = 0;

	UPROPERTY()
	FFlockJsonData Settings;

	UPROPERTY()
	TArray<FString> MemberIds;

	/** Reads a party; one that names no party (no id) is not one, and fails the read. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPartyReading& OutStruct, FString& OutError);
};

/** The answer to "the player's party": the party, or none (the server answers a null result). */
USTRUCT()
struct FLOCK_API FFlockMyPartyReading
{
	GENERATED_BODY()

	UPROPERTY()
	bool bInParty = false;

	UPROPERTY()
	FFlockPartyReading Party;

	/** Reads the envelope: a null result is "in no party"; no result member at all is not the server's answer. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockMyPartyReading& OutStruct, FString& OutError);
};

/** Why a multiplayer session stopped being the player's. An open set: the server can add reasons, so compare with these and keep a default. */
namespace FlockMultiplayerSessionEndReasons
{
	/** The host ended it. */
	inline constexpr const TCHAR* EndedByHost = TEXT("ended_by_host");
	/** The host left and nobody was left to take over. */
	inline constexpr const TCHAR* HostLeft = TEXT("host_left");
	/** It reached the game's longest session age. */
	inline constexpr const TCHAR* Expired = TEXT("expired");
	/** Every player left. */
	inline constexpr const TCHAR* Empty = TEXT("empty");
	/** The player left it, with Leave or from another device. */
	inline constexpr const TCHAR* Left = TEXT("left");
	/** The server stopped hearing from the player and gave the seat up. */
	inline constexpr const TCHAR* Dropped = TEXT("dropped");
	/** The player hosted or joined another session, which gives this seat up. */
	inline constexpr const TCHAR* MovedToAnotherSession = TEXT("moved_to_another_session");
	/** The sign-in the session belonged to ended: a sign-out, another player signing in, or Flock shutting down. */
	inline constexpr const TCHAR* SignedOut = TEXT("signed_out");
}

/** One player seated in a multiplayer session. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockMultiplayerSessionPlayer
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FString PlayerId;
};

/** How to reach a session's host, as the host published it: a mode and whatever values that mode needs. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockMultiplayerSessionConnection
{
	GENERATED_BODY()

	/** What kind of connection this is, such as "direct", or one the game names itself. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FString Mode;

	/** Everything the host published, the mode included; keys as sent. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FFlockJsonData Values;
};

/** The connection modes the SDK publishes. An open set: a game may publish a mode of its own, such as "steam". */
namespace FlockMultiplayerConnectionModes
{
	/** The host's address and port, published by Publish Direct Connection. */
	inline constexpr const TCHAR* Direct = TEXT("direct");
}

/** The values a direct connection holds on the wire; "address" and "port" are the names the backend expects. */
namespace FlockDirectConnectionValues
{
	/** The address players outside the host's network use: its public address when known, its LAN address otherwise. */
	inline constexpr const TCHAR* Address = TEXT("address");
	inline constexpr const TCHAR* Port = TEXT("port");
	inline constexpr const TCHAR* LanAddress = TEXT("lan_address");
	inline constexpr const TCHAR* PublicAddress = TEXT("public_address");
}

/** Where this game connects to reach a direct host. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockDirectAddress
{
	GENERATED_BODY()

	/** The host's IPv4 address; empty when the host published no direct IPv4 address and port this game can use. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FString Address;

	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	int32 Port = 0;

	/** True when this is the host's LAN address: chosen because this device shares the host's public address, or the only address the host knew. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	bool bIsLan = false;

	bool IsFound() const { return !Address.IsEmpty(); }
};

/** The STUN servers the relay route lists ("stun:host:port"), as an answer asked with no session holds them. */
USTRUCT()
struct FLOCK_API FFlockStunServerList
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FString> Urls;

	/** Reads the answer; one with no list of servers is not the server's answer, and fails the read. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockStunServerList& OutStruct, FString& OutError);
};

/** How Travel To Session ended. An open set: compare with these and keep a default. */
namespace FlockTravelToSessionOutcomes
{
	/** This game hosts: its level opened listening for players, and how to reach it is published. */
	inline constexpr const TCHAR* StartedAsHost = TEXT("started_as_host");
	/** This game connected to the host and is in the host's level. */
	inline constexpr const TCHAR* Connected = TEXT("connected");
	/** The host did not publish how to reach it within Wait For Host Seconds. */
	inline constexpr const TCHAR* HostNeverPublished = TEXT("host_never_published");
	/** The session ended while this game waited for the host. */
	inline constexpr const TCHAR* SessionEnded = TEXT("session_ended");
	/**
	 * Connecting to the host failed, and the engine has taken this game back to its default level; or the host published no
	 * address and port this game can use, and it stayed where it was. Outside a LAN the host's port must be forwarded to it.
	 */
	inline constexpr const TCHAR* CouldNotConnect = TEXT("could_not_connect");
	/** The host's level opened without listening for players (or did not open), so nothing was published. */
	inline constexpr const TCHAR* CouldNotStart = TEXT("could_not_start");
	/** The host published a connection of the game's own (such as "steam"); the result's Connection holds it. */
	inline constexpr const TCHAR* NotDirect = TEXT("not_direct");
}

/** What a game can choose about Travel To Session. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockTravelToSessionOptions
{
	GENERATED_BODY()

	/**
	 * The level the host opens listening for players, such as "/Game/Maps/Arena"; empty opens the level it is in again. A
	 * player travels to whatever level the host has open.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Multiplayer")
	FString HostLevel;

	/** How long a player waits for the host to publish how to reach it. */
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Multiplayer")
	float WaitForHostSeconds = 60.f;
};

/** How Travel To Session ended. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockTravelToSessionResult
{
	GENERATED_BODY()

	/** One of FlockTravelToSessionOutcomes. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FString Outcome;

	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	bool bIsHost = false;

	/** The address published (host) or traveled to (player); empty when there was none. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FString Address;

	/** The port the host's level listens on (host) or the port traveled to (player); 0 when there was none. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	int32 Port = 0;

	/** The host's connection as published; its Mode is empty when the host never published. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FFlockMultiplayerSessionConnection Connection;
};

/** A short-lived proof that the player holds a seat, to show the host when connecting. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockMultiplayerSessionJoinToken
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FString Token;

	/** How long the token is good for from when it was given (60 s). */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	int32 ExpiresInSeconds = 0;

	/** Reads the token; an answer with no token is not one, and fails the read. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockMultiplayerSessionJoinToken& OutStruct, FString& OutError);
};

/** The player a join token belongs to, as the host's check answers it. */
USTRUCT()
struct FLOCK_API FFlockVerifiedJoinToken
{
	GENERATED_BODY()

	UPROPERTY()
	FString SessionId;

	UPROPERTY()
	FString PlayerId;

	/** Reads the answer; one that names no player is not one, and fails the read. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockVerifiedJoinToken& OutStruct, FString& OutError);
};

/** The server's words for a seat held, a seat it gave up and a session that is over. */
namespace FlockMultiplayerSessionWireValues
{
	inline constexpr const TCHAR* SeatJoined = TEXT("joined");
	inline constexpr const TCHAR* SeatDropped = TEXT("dropped");
	inline constexpr const TCHAR* SessionEnded = TEXT("ended");
}

/** One seat of a session, as the session's answer lists it; Status is the server's "joined", "left" or "dropped". */
USTRUCT()
struct FLOCK_API FFlockMultiplayerSessionSeat
{
	GENERATED_BODY()

	UPROPERTY()
	FString PlayerId;

	UPROPERTY()
	FString Status;
};

/**
 * A multiplayer session as every session route answers it: the whole session, with every player who held a seat in it in
 * seniority order (longest seated first). The two epochs only grow, so a reading with a lower one was taken earlier.
 */
USTRUCT()
struct FLOCK_API FFlockMultiplayerSessionReading
{
	GENERATED_BODY()

	UPROPERTY()
	FString Id;

	UPROPERTY()
	FString Status;

	/** Why the session ended; empty while it lasts. */
	UPROPERTY()
	FString EndedReason;

	UPROPERTY()
	FString HostPlayerId;

	UPROPERTY()
	int32 HostEpoch = 0;

	UPROPERTY()
	FString JoinCode;

	UPROPERTY()
	int32 MaxPlayers = 0;

	/** True when the host published a connection the reader may see: the server blanks it for a player without a seat. */
	UPROPERTY()
	bool bHasConnection = false;

	UPROPERTY()
	FFlockMultiplayerSessionConnection Connection;

	UPROPERTY()
	int32 ConnectionEpoch = 0;

	/** The game's own data on the session, keys as sent. */
	UPROPERTY()
	FFlockJsonData Data;

	UPROPERTY()
	TArray<FFlockMultiplayerSessionSeat> Seats;

	/** How often to heartbeat for the game's timeouts; 0 when the answer gives none. */
	UPROPERTY()
	int32 HeartbeatIntervalSeconds = 0;

	/** The players holding a seat now, in seniority order. */
	TArray<FString> SeatedPlayerIds() const;

	/** The seat the player holds or once held in this session, or null when they never held one. */
	const FFlockMultiplayerSessionSeat* FindSeat(const FString& PlayerId) const;

	/** Reads a session; one that names no session (no id), or a seat with no player, fails the read. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockMultiplayerSessionReading& OutStruct, FString& OutError);
};

/**
 * How a search for a match ended. An open set: the server can add others, so compare FFlockMatchmakingResult::Outcome with
 * these and treat any other as ended without a match.
 */
namespace FlockMatchmakingOutcomes
{
	/** Players were matched; the result's Session is the session the match seats them in. */
	inline constexpr const TCHAR* Matched = TEXT("matched");
	/** No match was found within the server's time for a search (5 minutes). Search again to keep looking. */
	inline constexpr const TCHAR* Expired = TEXT("expired");
	/** A player joined or left the party while it searched, so the server stopped the search; the leader searches again. */
	inline constexpr const TCHAR* PartyChanged = TEXT("party_changed");
	/** Stopped by another game: a party member's, or this player's on another device. A search this game cancels completes the call as Cancelled instead. */
	inline constexpr const TCHAR* Cancelled = TEXT("cancelled");
}

/** What the game can choose about a search. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockMatchmakingOptions
{
	GENERATED_BODY()

	/** The game's own data on the search, sent to the server as given, keys as written; empty sends none. */
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Multiplayer")
	FFlockCommandData Attributes;
};

/** A search for a match that has ended: matched, or how it ended without one. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockMatchmakingResult
{
	GENERATED_BODY()

	/** One of FlockMatchmakingOutcomes, or an outcome the server added. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FString Outcome;

	/**
	 * The session the match seats its players in, held like any other; null without a match, when the game's multiplayer
	 * settings make no session on a match, or when reading it failed (GetMySession finds it).
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	TObjectPtr<UFlockMultiplayerSession> Session = nullptr;

	/** The matched players' ids, the longest waiting first; empty without a match. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	TArray<FString> PlayerIds;

	bool IsMatched() const { return Outcome.Equals(FlockMatchmakingOutcomes::Matched, ESearchCase::CaseSensitive); }
};

/** The wire spellings of a search's ticket. */
namespace FlockMatchmakingWireValues
{
	inline constexpr const TCHAR* TicketQueued = TEXT("queued");
	inline constexpr const TCHAR* TicketMatched = TEXT("matched");
	inline constexpr const TCHAR* TicketCancelled = TEXT("cancelled");
	inline constexpr const TCHAR* TicketExpired = TEXT("expired");
	/** The cancel reason the server gives when a party's roster changed under its search. */
	inline constexpr const TCHAR* CancelledForPartyChange = TEXT("party_changed");
}

/** A match, as much of it as a ticket's reading carries. */
USTRUCT()
struct FLOCK_API FFlockMatchmakingMatch
{
	GENERATED_BODY()

	UPROPERTY()
	FString Id;

	/** The matched players, the longest waiting first. */
	UPROPERTY()
	TArray<FString> PlayerIds;

	/** The session the match made, or empty when the game's settings make none. */
	UPROPERTY()
	FString SessionId;
};

/** One reading of a search's ticket, as the ticket routes answer it. */
USTRUCT()
struct FLOCK_API FFlockMatchmakingTicketReading
{
	GENERATED_BODY()

	UPROPERTY()
	FString Id;

	UPROPERTY()
	FString QueueId;

	UPROPERTY()
	FString PlayerId;

	/** queued, matched, cancelled or expired (FlockMatchmakingWireValues), or a status the server added. */
	UPROPERTY()
	FString Status;

	/** The party the search is for, or empty for a search alone. */
	UPROPERTY()
	FString PartyId;

	/** Why a cancelled ticket was cancelled: the player, or party_changed. */
	UPROPERTY()
	FString CancelReason;

	/** True once the reading carries the match: the reads do, once there is one; the create and cancel answers do not. */
	UPROPERTY()
	bool bHasMatch = false;

	UPROPERTY()
	FFlockMatchmakingMatch Match;

	bool IsQueued() const;
	bool IsMatched() const;
	/** True for a ticket of this party; an empty party id means a search alone. */
	bool IsForParty(const FString& InPartyId) const;

	/** Reads a ticket; one that names no ticket (no id) or no status is not one, and fails the read. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockMatchmakingTicketReading& OutStruct, FString& OutError);
};

/** The answer to making a search: one ticket per player in it (the party's members, or the player alone). */
USTRUCT()
struct FLOCK_API FFlockMatchmakingTicketsMade
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FFlockMatchmakingTicketReading> Tickets;

	/** The ticket of this player among them, or null. */
	const FFlockMatchmakingTicketReading* FindTicketOf(const FString& PlayerId) const;

	/** Reads the envelope: a result that is not a list of at least one ticket is not the server's answer. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockMatchmakingTicketsMade& OutStruct, FString& OutError);
};
