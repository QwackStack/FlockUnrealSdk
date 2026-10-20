// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Models/FlockMultiplayerModels.h"

/**
 * The host's game's own check of a joining player, run once Flock has let them in: return true to let them in, or false with a
 * reason the player is told (Refused Reason); a refusal with no reason is told as refused_by_the_game.
 */
using FFlockApproveJoiningPlayer = TFunction<bool(const FFlockJoiningPlayer& Player, FString& OutRefusedReason)>;

/** The host's check of joining players, for a game that hosts through Travel To Session. */
namespace FlockJoinChecks
{
	/** The option of the travel URL a joining player's join token rides in (?FlockJoinToken=...); Travel To Session adds it. */
	inline constexpr const TCHAR* JoinTokenOption = TEXT("FlockJoinToken");

	/**
	 * Call from the game mode's PreLoginAsync, in place of the engine's own, with what the engine gave it. While this game hosts
	 * through Travel To Session, a player is let in only once Flock has verified the join token their travel URL carries, with
	 * one connection a player: no token, a token Flock refuses (made up, another session's, expired, or of a player who gave up
	 * their seat), the same player twice, a session that ended and a device that no longer hosts are each refused with their own
	 * reason, and so is a check Flock does not answer within 8 s, since nobody is let in unchecked. Then the engine's own checks
	 * run (the game mode's PreLogin, such as a full game), then ApproveJoiningPlayer. OnComplete is always called, and the game
	 * thread never waits. While this game is not hosting through Travel To Session, this is the engine's own PreLoginAsync and
	 * ApproveJoiningPlayer is not run. A game mode that hosts through Travel To Session without calling this lets nobody in
	 * (join_check_missing): Flock refuses its players. A player's connection is known as Flock's player by Player For Connection.
	 */
	FLOCK_API void CheckJoiningPlayer(AGameModeBase* GameMode, const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId,
		const AGameModeBase::FOnPreLoginCompleteDelegate& OnComplete, FFlockApproveJoiningPlayer ApproveJoiningPlayer = nullptr);
}
