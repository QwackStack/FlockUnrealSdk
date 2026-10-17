// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Models/FlockMultiplayerModels.h"
#include "FlockMultiplayerLibrary.generated.h"

/**
 * Pure Blueprint nodes for multiplayer: every spelling a multiplayer result can carry, to compare a result's Outcome, Refused
 * Reason or End Reason against rather than typing it, and Is Matched. Each returns the SDK's own constant, so Blueprint and C++
 * cannot drift. These are open sets: the server, the host's game or the engine can give a value not listed here, so a graph
 * compares with these and keeps a default.
 */
UCLASS()
class FLOCK_API UFlockMultiplayerLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** True when the search found a match: the result's Session is where the players play. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Is Matched"), Category = "Flock|Multiplayer")
	static bool IsMatched(const FFlockMatchmakingResult& Result) { return Result.IsMatched(); }

	// ── Why a session stopped being the player's (a session's End Reason) ──

	/** The host ended it. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Session End Reason Ended By Host"), Category = "Flock|Multiplayer")
	static FString SessionEndReasonEndedByHost() { return FlockMultiplayerSessionEndReasons::EndedByHost; }

	/** The host left and nobody was left to take over. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Session End Reason Host Left"), Category = "Flock|Multiplayer")
	static FString SessionEndReasonHostLeft() { return FlockMultiplayerSessionEndReasons::HostLeft; }

	/** It reached the game's longest session age. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Session End Reason Expired"), Category = "Flock|Multiplayer")
	static FString SessionEndReasonExpired() { return FlockMultiplayerSessionEndReasons::Expired; }

	/** Every player left. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Session End Reason Empty"), Category = "Flock|Multiplayer")
	static FString SessionEndReasonEmpty() { return FlockMultiplayerSessionEndReasons::Empty; }

	/** The player left it, with Leave Session or from another device. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Session End Reason Left"), Category = "Flock|Multiplayer")
	static FString SessionEndReasonLeft() { return FlockMultiplayerSessionEndReasons::Left; }

	/** The server stopped hearing from the player and gave the seat up. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Session End Reason Dropped"), Category = "Flock|Multiplayer")
	static FString SessionEndReasonDropped() { return FlockMultiplayerSessionEndReasons::Dropped; }

	/** The player hosted or joined another session, which gives this seat up. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Session End Reason Moved To Another Session"), Category = "Flock|Multiplayer")
	static FString SessionEndReasonMovedToAnotherSession() { return FlockMultiplayerSessionEndReasons::MovedToAnotherSession; }

	/** The sign-in the session belonged to ended: a sign-out, another player signing in, or Flock shutting down. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Session End Reason Signed Out"), Category = "Flock|Multiplayer")
	static FString SessionEndReasonSignedOut() { return FlockMultiplayerSessionEndReasons::SignedOut; }

	// ── How Travel To Session ended (its result's Outcome) ──

	/** This game hosts: its level opened listening for players, and how to reach it is published. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Travel Outcome Started As Host"), Category = "Flock|Multiplayer")
	static FString TravelOutcomeStartedAsHost() { return FlockTravelToSessionOutcomes::StartedAsHost; }

	/** This game connected to the host and is in the host's level. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Travel Outcome Connected"), Category = "Flock|Multiplayer")
	static FString TravelOutcomeConnected() { return FlockTravelToSessionOutcomes::Connected; }

	/** The host did not publish how to reach it within Wait For Host Seconds. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Travel Outcome Host Never Published"), Category = "Flock|Multiplayer")
	static FString TravelOutcomeHostNeverPublished() { return FlockTravelToSessionOutcomes::HostNeverPublished; }

	/** The session ended while this game waited for the host. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Travel Outcome Session Ended"), Category = "Flock|Multiplayer")
	static FString TravelOutcomeSessionEnded() { return FlockTravelToSessionOutcomes::SessionEnded; }

	/** Connecting to the host failed, or the host published no address and port this game can use. Outside a LAN the host's port must be forwarded to it. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Travel Outcome Could Not Connect"), Category = "Flock|Multiplayer")
	static FString TravelOutcomeCouldNotConnect() { return FlockTravelToSessionOutcomes::CouldNotConnect; }

	/** The host's level opened without listening for players (or did not open), so nothing was published. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Travel Outcome Could Not Start"), Category = "Flock|Multiplayer")
	static FString TravelOutcomeCouldNotStart() { return FlockTravelToSessionOutcomes::CouldNotStart; }

	/** The host published a connection of the game's own (such as "steam"); the result's Connection holds it. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Travel Outcome Not Direct"), Category = "Flock|Multiplayer")
	static FString TravelOutcomeNotDirect() { return FlockTravelToSessionOutcomes::NotDirect; }

	/** The host turned this player away; the result's Refused Reason says why. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Travel Outcome Refused"), Category = "Flock|Multiplayer")
	static FString TravelOutcomeRefused() { return FlockTravelToSessionOutcomes::Refused; }

	// ── Why a host turned a joining player away (a travel result's Refused Reason) ──

	/** The player sent no join token: it did not come through Travel To Session. Flock was not asked. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Refused Reason No Join Token"), Category = "Flock|Multiplayer")
	static FString RefusedReasonNoJoinToken() { return FlockTravelToSessionRefusedReasons::NoJoinToken; }

	/** Flock said the token is not good for this session: made up, another session's, expired (they last 60 s), or its player gave up their seat. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Refused Reason Join Token Invalid"), Category = "Flock|Multiplayer")
	static FString RefusedReasonJoinTokenInvalid() { return FlockTravelToSessionRefusedReasons::JoinTokenInvalid; }

	/** The same player is already let in through another connection; one connection a player. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Refused Reason Already Connected"), Category = "Flock|Multiplayer")
	static FString RefusedReasonAlreadyConnected() { return FlockTravelToSessionRefusedReasons::AlreadyConnected; }

	/** The host could not check the token with Flock in time (8 s), or Flock's answer could not be read; nobody is let in unchecked. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Refused Reason Flock Unreachable"), Category = "Flock|Multiplayer")
	static FString RefusedReasonFlockUnreachable() { return FlockTravelToSessionRefusedReasons::FlockUnreachable; }

	/** The session has ended. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Refused Reason Session Ended"), Category = "Flock|Multiplayer")
	static FString RefusedReasonSessionEnded() { return FlockTravelToSessionRefusedReasons::SessionEnded; }

	/** The device reached no longer hosts the session. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Refused Reason Not Host"), Category = "Flock|Multiplayer")
	static FString RefusedReasonNotHost() { return FlockTravelToSessionRefusedReasons::NotHost; }

	/** The host's game turned the player away with no reason of its own. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Refused Reason Refused By The Game"), Category = "Flock|Multiplayer")
	static FString RefusedReasonRefusedByTheGame() { return FlockTravelToSessionRefusedReasons::RefusedByTheGame; }

	/** The host's game mode does not run Flock's check of joining players (a Flock game mode does), so nobody gets in. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Refused Reason Join Check Missing"), Category = "Flock|Multiplayer")
	static FString RefusedReasonJoinCheckMissing() { return FlockTravelToSessionRefusedReasons::JoinCheckMissing; }

	// ── How a search for a match ended (its result's Outcome) ──

	/** Players were matched; the result's Session is the session the match seats them in. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Matchmaking Outcome Matched"), Category = "Flock|Multiplayer")
	static FString MatchmakingOutcomeMatched() { return FlockMatchmakingOutcomes::Matched; }

	/** No match was found within the server's time for a search (5 minutes). Search again to keep looking. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Matchmaking Outcome Expired"), Category = "Flock|Multiplayer")
	static FString MatchmakingOutcomeExpired() { return FlockMatchmakingOutcomes::Expired; }

	/** A player joined or left the party while it searched, so the server stopped the search; the leader searches again. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Matchmaking Outcome Party Changed"), Category = "Flock|Multiplayer")
	static FString MatchmakingOutcomePartyChanged() { return FlockMatchmakingOutcomes::PartyChanged; }

	/** Stopped by another game: a party member's, or this player's on another device. A search this game cancels fires no pin. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Matchmaking Outcome Cancelled"), Category = "Flock|Multiplayer")
	static FString MatchmakingOutcomeCancelled() { return FlockMatchmakingOutcomes::Cancelled; }

	// ── How the host is reached (a session's Connection) ──

	/** The host's address and port, published by Publish Direct Connection or Travel To Session. A game may publish a mode of its own, such as "steam". */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Connection Mode Direct"), Category = "Flock|Multiplayer")
	static FString ConnectionModeDirect() { return FlockMultiplayerConnectionModes::Direct; }

	/** The address players outside the host's network use: its public address when known, its LAN address otherwise. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Direct Connection Value Address"), Category = "Flock|Multiplayer")
	static FString DirectConnectionValueAddress() { return FlockDirectConnectionValues::Address; }

	/** The port the host's game listens on. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Direct Connection Value Port"), Category = "Flock|Multiplayer")
	static FString DirectConnectionValuePort() { return FlockDirectConnectionValues::Port; }

	/** The host's address on its own network. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Direct Connection Value LAN Address"), Category = "Flock|Multiplayer")
	static FString DirectConnectionValueLanAddress() { return FlockDirectConnectionValues::LanAddress; }

	/** The host's address as the internet sees it, when known. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Direct Connection Value Public Address"), Category = "Flock|Multiplayer")
	static FString DirectConnectionValuePublicAddress() { return FlockDirectConnectionValues::PublicAddress; }
};
