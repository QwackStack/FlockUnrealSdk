// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameMode.h"
#include "GameFramework/GameModeBase.h"
#include "Models/FlockMultiplayerModels.h"
#include "FlockGameModes.generated.h"

/**
 * A game mode that lets in only the session's players while this game hosts through Travel To Session: the parent for the
 * game's own game mode, as a Blueprint or in C++. A joining player gets in once Flock has verified the join token their travel
 * carries (one connection a player), then the engine's own checks (such as a full game), then Approve Joining Player, the
 * game's own check. While this game does not host through Travel To Session, joining players meet the engine's own checks
 * alone. Built on Game Mode Base; Flock Game Mode is the same on Game Mode, for a game that uses match states.
 */
UCLASS(Blueprintable)
class FLOCK_API AFlockGameModeBase : public AGameModeBase
{
	GENERATED_BODY()

public:
	virtual void PreLoginAsync(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId,
		const FOnPreLoginCompleteDelegate& OnComplete) override;

	/**
	 * The game's own check of a joining player, after Flock's and the engine's, while this game hosts through Travel To Session.
	 * Joining Player's Player Id is the player Flock verified; its Options are the travel's, without the join token. Return
	 * false to turn the player away, with a Refused Reason the player's Travel To Session reports (empty reads as
	 * refused_by_the_game). Lets every player in until overridden.
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "Flock|Multiplayer")
	bool ApproveJoiningPlayer(const FFlockJoiningPlayer& JoiningPlayer, FString& RefusedReason);
	virtual bool ApproveJoiningPlayer_Implementation(const FFlockJoiningPlayer& JoiningPlayer, FString& RefusedReason);
};

/** Flock Game Mode Base on Game Mode (match states, ready-to-start and the like): lets in only the session's players. */
UCLASS(Blueprintable)
class FLOCK_API AFlockGameMode : public AGameMode
{
	GENERATED_BODY()

public:
	virtual void PreLoginAsync(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId,
		const FOnPreLoginCompleteDelegate& OnComplete) override;

	/**
	 * The game's own check of a joining player, after Flock's and the engine's, while this game hosts through Travel To Session.
	 * Joining Player's Player Id is the player Flock verified; its Options are the travel's, without the join token. Return
	 * false to turn the player away, with a Refused Reason the player's Travel To Session reports (empty reads as
	 * refused_by_the_game). Lets every player in until overridden.
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "Flock|Multiplayer")
	bool ApproveJoiningPlayer(const FFlockJoiningPlayer& JoiningPlayer, FString& RefusedReason);
	virtual bool ApproveJoiningPlayer_Implementation(const FFlockJoiningPlayer& JoiningPlayer, FString& RefusedReason);
};
