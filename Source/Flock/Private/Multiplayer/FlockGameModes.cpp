// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockGameModes.h"

#include "Multiplayer/FlockJoinChecks.h"

namespace
{
	/** The game's own check, asked through its Approve Joining Player; a game mode gone by then turns the player away. */
	template <typename TGameMode>
	FFlockApproveJoiningPlayer AskTheGame(TGameMode* GameMode)
	{
		const TWeakObjectPtr<TGameMode> WeakGameMode(GameMode);
		return [WeakGameMode](const FFlockJoiningPlayer& JoiningPlayer, FString& OutRefusedReason)
		{
			TGameMode* Asked = WeakGameMode.Get();
			return Asked != nullptr && Asked->ApproveJoiningPlayer(JoiningPlayer, OutRefusedReason);
		};
	}
}

void AFlockGameModeBase::PreLoginAsync(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId,
	const FOnPreLoginCompleteDelegate& OnComplete)
{
	FlockJoinChecks::CheckJoiningPlayer(this, Options, Address, UniqueId, OnComplete, AskTheGame(this));
}

bool AFlockGameModeBase::ApproveJoiningPlayer_Implementation(const FFlockJoiningPlayer& JoiningPlayer, FString& RefusedReason)
{
	return true;
}

void AFlockGameMode::PreLoginAsync(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId,
	const FOnPreLoginCompleteDelegate& OnComplete)
{
	FlockJoinChecks::CheckJoiningPlayer(this, Options, Address, UniqueId, OnComplete, AskTheGame(this));
}

bool AFlockGameMode::ApproveJoiningPlayer_Implementation(const FFlockJoiningPlayer& JoiningPlayer, FString& RefusedReason)
{
	return true;
}
