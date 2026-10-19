// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "GameFramework/OnlineReplStructs.h"
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
	/** The host's address on Flock's relay, published by Travel To Session, with its direct addresses too when players on its network may connect directly. */
	inline constexpr const TCHAR* Relay = TEXT("relay");
}

/** What a relay connection adds to the direct values: the host's address on the relay and the relay server it is on. */
namespace FlockRelayConnectionValues
{
	/** The host's address on Flock's relay, "a.b.c.d:port"; a player's seat names its own the same way. */
	inline constexpr const TCHAR* RelayAddress = TEXT("relay_address");
	/** The relay server the host's address is on, "name:port"; players reserve their own address on it. */
	inline constexpr const TCHAR* RelayServer = TEXT("relay_server");
	/**
	 * The highest version of the messages two devices' relay bridges send each other (to move to a direct path, and to say a new
	 * connection starts) that this device's bridge reads, every earlier version included; published by the host for the session and
	 * by a player for its seat. Absent from a device whose bridge would hand them to the engine, which closes a connection they reach.
	 */
	inline constexpr const TCHAR* BridgeMessages = TEXT("bridge_messages");
}

/** Which players Travel To Session takes through Flock's relay. Set it the same on every player of a game. */
UENUM(BlueprintType)
enum class EFlockRelayUse : uint8
{
	/**
	 * A player sharing the host's public address tries the host's address on its network for a few seconds, then goes through
	 * Flock's relay if that is not answered; everyone else goes through the relay, so no port has to be forwarded. A player who came
	 * through the relay moves to a direct path once it and the host reach each other (each learns where Flock's servers see it and
	 * offers that through the relay, so the two ends learn each other's addresses), and back to the relay when that path goes quiet;
	 * the game's connection carries on through both. Against a host that offers no relay, players connect to the address it published.
	 */
	WhenNeeded,
	/**
	 * Every player goes through Flock's relay, on the host's network too, and stays on it: the host publishes no address of its own
	 * and turns away any player that reaches it any other way, so no player's address reaches the host or the host's a player.
	 */
	Always,
	/** Never through the relay: players reach the host on its network, or on a port it forwarded. */
	Never,
};

/** Why Flock's relay could not be used (a travel result's Relay Failure Reason). An open set: compare with these and keep a default. */
namespace FlockRelayFailureReasons
{
	/** Flock listed no relay: switched off for this game in the Flock dashboard, or none set up. */
	inline constexpr const TCHAR* NotOffered = TEXT("relay_not_offered");
	/** The host takes no players through Flock's relay, and this player's Relay is Always. */
	inline constexpr const TCHAR* NotOfferedByTheHost = TEXT("relay_not_offered_by_host");
	/** Flock does not list, for this player, the relay server the host uses. */
	inline constexpr const TCHAR* ServerNotListed = TEXT("relay_server_not_listed");
	/** The studio's relay is paused until its bill is paid. */
	inline constexpr const TCHAR* Paused = TEXT("relay_paused");
	/** The player asked Flock for relay logins more than 12 times in a minute, or its seat drew the most logins one seat may (10). */
	inline constexpr const TCHAR* TooManyLogins = TEXT("relay_too_many_logins");
	/** Flock says the player has no seat in the session. */
	inline constexpr const TCHAR* NotInSession = TEXT("relay_not_in_session");
	/** Flock did not answer the request for relay logins. */
	inline constexpr const TCHAR* FlockUnreachable = TEXT("flock_unreachable");
	/** Flock refused the request for relay logins for another reason. */
	inline constexpr const TCHAR* LoginsRefused = TEXT("relay_logins_refused");
	/** The relay server's name could not be looked up, or the server did not answer. */
	inline constexpr const TCHAR* Unreachable = TEXT("relay_unreachable");
	/** The relay refused the login Flock minted. */
	inline constexpr const TCHAR* WrongLogin = TEXT("relay_wrong_login");
	/** The relay has no room for another address. */
	inline constexpr const TCHAR* Full = TEXT("relay_full");
	/** The relay refused to let packets in from the host's relay address, or from a player's. */
	inline constexpr const TCHAR* OpenRefused = TEXT("relay_open_refused");
	/** The relay refused a request for another reason. */
	inline constexpr const TCHAR* Refused = TEXT("relay_refused");
	/** The relay let the address go (a renewal was refused, or none was answered before it lapsed), or it has answered nothing for 45 s: this device's packets no longer reach it. */
	inline constexpr const TCHAR* Lost = TEXT("relay_lost");
	/** The relay connection was closed by its owner (the session's end) before the call. */
	inline constexpr const TCHAR* Closed = TEXT("relay_closed");
	/** The relay needs threads of its own, and this launch of the game runs without threads. */
	inline constexpr const TCHAR* NoThread = TEXT("relay_no_thread");
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
	 * address and port this game can use, and it stayed where it was. Outside a LAN a host that offers no relay must have its
	 * port forwarded to it.
	 */
	inline constexpr const TCHAR* CouldNotConnect = TEXT("could_not_connect");
	/** The host's level opened without listening for players (or did not open), so nothing was published. */
	inline constexpr const TCHAR* CouldNotStart = TEXT("could_not_start");
	/** The host published a connection of the game's own (such as "steam"); the result's Connection holds it. */
	inline constexpr const TCHAR* NotDirect = TEXT("not_direct");
	/** The host turned this player away; the result's Refused Reason says why, and the engine has taken this game back to its default level. */
	inline constexpr const TCHAR* Refused = TEXT("refused");
	/** Flock's relay was needed and could not be used; the result's Relay Failure Reason says why (a FlockRelayFailureReasons value). */
	inline constexpr const TCHAR* RelayFailed = TEXT("relay_failed");
	/**
	 * The game called Travel To Session again before this one ended, and the newer one took over: nothing to do here, its own
	 * answer follows. What this one had started was left as a cancel leaves it.
	 */
	inline constexpr const TCHAR* Restarted = TEXT("restarted");
}

/**
 * Why a host turned a joining player away (Refused Reason). An open set: compare with these and keep a default, since the host's
 * game gives reasons of its own, and so does the engine (such as "Server full.").
 */
namespace FlockTravelToSessionRefusedReasons
{
	/** The player sent no join token: it did not come through Travel To Session. Flock was not asked. */
	inline constexpr const TCHAR* NoJoinToken = TEXT("no_join_token");
	/** Flock said the token is not good for this session: made up, another session's, expired (they last 60 s), or its player gave up their seat. */
	inline constexpr const TCHAR* JoinTokenInvalid = TEXT("join_token_invalid");
	/** The same player is already let in through another connection; one connection a player. */
	inline constexpr const TCHAR* AlreadyConnected = TEXT("already_connected");
	/** The host could not check the token with Flock in time (8 s), or Flock's answer could not be read; nobody is let in unchecked. */
	inline constexpr const TCHAR* FlockUnreachable = TEXT("flock_unreachable");
	/** The session has ended. */
	inline constexpr const TCHAR* SessionEnded = TEXT("session_ended");
	/** The device reached no longer hosts the session. */
	inline constexpr const TCHAR* NotHost = TEXT("not_host");
	/** The host's game turned the player away with no reason of its own. */
	inline constexpr const TCHAR* RefusedByTheGame = TEXT("refused_by_the_game");
	/** The host's game mode does not run Flock's check of joining players (FlockJoinChecks::CheckJoiningPlayer), so nobody gets in. */
	inline constexpr const TCHAR* JoinCheckMissing = TEXT("join_check_missing");
	/** The host takes players through Flock's relay only (its Relay is Always), and this player reached it another way. Flock was not asked. */
	inline constexpr const TCHAR* RelayOnly = TEXT("relay_only");
}

/** What a game can choose about Travel To Session. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockTravelToSessionOptions
{
	GENERATED_BODY()

	/**
	 * The level the host opens listening for players, such as "/Game/Maps/Arena"; empty opens the level it is in again, or, for a
	 * player of the session that has become its host, the level it played with the old host (the engine may have taken it back
	 * to its default level meanwhile). A host already listening for the session keeps its level. A player travels to whatever
	 * level the host has open.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Multiplayer")
	FString HostLevel;

	/** How long a player waits for the host to publish how to reach it. */
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Multiplayer")
	float WaitForHostSeconds = 60.f;

	/**
	 * Which players go through Flock's relay: by default players on the host's network connect directly (through the relay if
	 * the host does not answer there), everyone else through the relay. Set it the same on every player of a game.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Multiplayer")
	EFlockRelayUse Relay = EFlockRelayUse::WhenNeeded;
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

	/**
	 * For a host, the direct address it published (empty when it published none, with Relay Always); for a player, the host's
	 * address it traveled to, the host's relay IP when it went through the relay.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FString Address;

	/** The port the host's level listens on (host), or the host's port traveled to (player), on the relay when through it; 0 when there was none. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	int32 Port = 0;

	/** True for a player connected through Flock's relay, and for a host that takes players through it. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	bool bThroughRelay = false;

	/**
	 * Why Flock's relay could not be used, a FlockRelayFailureReasons value: set with the relay_failed outcome, and for a host that
	 * takes players on its network only because its relay could not be opened; empty otherwise.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FString RelayFailureReason;

	/** The host's connection as published; its Mode is empty when the host never published. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FFlockMultiplayerSessionConnection Connection;

	/**
	 * Why the host turned this player away when Outcome is refused: a FlockTravelToSessionRefusedReasons value, the host's game's own
	 * reason, or the engine's (such as "Server full."); empty otherwise.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FString RefusedReason;
};

/** A joining player Flock has let in, handed to the host's game's own check (FlockJoinChecks::CheckJoiningPlayer). */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockJoiningPlayer
{
	GENERATED_BODY()

	/** The Flock player, as Flock verified their join token. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FString PlayerId;

	/** The options of the player's travel URL as the engine gave them (such as "?Name=Ann"), without Flock's join token. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FString Options;

	/** The player's address, as the engine gave it: 127.0.0.1 for a player that came through Flock's relay. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FString Address;

	/** The engine's id the player passed, as the engine gave it. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FUniqueNetIdRepl UniqueId;
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

	/**
	 * The seat's address on Flock's relay ("a.b.c.d:port"), as the player published it for its seat (a connection of mode relay);
	 * empty for none. The server shows it to the host (every seat's) and to the seat's own player only.
	 */
	UPROPERTY()
	FString RelayAddress;

	/** True when the seat's relay connection says its device's relay bridge reads the bridges' own messages (FlockRelayConnectionValues::BridgeMessages). */
	UPROPERTY()
	bool bReadsBridgeMessages = false;
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
