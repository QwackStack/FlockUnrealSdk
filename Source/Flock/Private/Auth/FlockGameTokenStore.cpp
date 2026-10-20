// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Auth/FlockGameTokenStore.h"

FFlockGameTokenStore::FFlockGameTokenStore(const TSharedRef<IFlockTokenStore>& InGame, const TSharedRef<IFlockTokenStore>& InEveryGame)
	: Game(InGame)
	, EveryGame(InEveryGame)
{
}

void FFlockGameTokenStore::Save(const FFlockStoredTokens& Tokens)
{
	Game->Save(Tokens);
	// A new sign-in replaces whatever an earlier build kept, so no later game takes that over.
	EveryGame->Clear();
}

bool FFlockGameTokenStore::Load(FFlockStoredTokens& OutTokens)
{
	if (Game->Load(OutTokens))
	{
		return true;
	}
	FFlockStoredTokens Earlier;
	if (!EveryGame->Load(Earlier))
	{
		return false;
	}
	Game->Save(Earlier);
	// Cleared only once the game's own file holds it: a save that failed without a word (a full disk) leaves the earlier sign-in
	// for the next launch rather than losing it.
	FFlockStoredTokens Kept;
	if (Game->Load(Kept) && Kept.AccessToken.Equals(Earlier.AccessToken, ESearchCase::CaseSensitive)
		&& Kept.RefreshToken.Equals(Earlier.RefreshToken, ESearchCase::CaseSensitive))
	{
		EveryGame->Clear();
	}
	OutTokens = MoveTemp(Earlier);
	return true;
}

void FFlockGameTokenStore::Clear()
{
	Game->Clear();
	EveryGame->Clear();
}
