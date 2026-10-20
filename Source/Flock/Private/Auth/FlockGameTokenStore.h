// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockTokenStore.h"

/**
 * One game's saved sign-in, kept apart from every other game's, so a sign-in made with one game is never restored by another (a
 * player of one game is no player of the next). Builds before 1.33.0 kept one sign-in for every game: the first game to read one
 * takes that over, so nobody is signed out by updating, and it is never taken twice; a new sign-in or a sign-out discards it.
 * Game thread only.
 */
class FFlockGameTokenStore : public IFlockTokenStore
{
public:
	/** Game keeps this game's own sign-in; EveryGame the one every game shared before 1.33.0. */
	FFlockGameTokenStore(const TSharedRef<IFlockTokenStore>& InGame, const TSharedRef<IFlockTokenStore>& InEveryGame);

	virtual void Save(const FFlockStoredTokens& Tokens) override;
	virtual bool Load(FFlockStoredTokens& OutTokens) override;
	virtual void Clear() override;

private:
	TSharedRef<IFlockTokenStore> Game;
	TSharedRef<IFlockTokenStore> EveryGame;
};
