// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockTokenStore.h"

/** In-memory IFlockTokenStore for automation tests, with fail switches for the store-failure paths. */
class FFlockMemoryTokenStore : public IFlockTokenStore
{
public:
	virtual void Save(const FFlockStoredTokens& Tokens) override
	{
		++SaveCount;
		if (bFailSave) { return; }
		Stored = Tokens;
		bHasTokens = true;
	}

	virtual bool Load(FFlockStoredTokens& OutTokens) override
	{
		++LoadCount;
		if (bFailLoad || !bHasTokens || Stored.AccessToken.IsEmpty()) { return false; }
		OutTokens = Stored;
		return true;
	}

	virtual void Clear() override
	{
		++ClearCount;
		Stored = FFlockStoredTokens();
		bHasTokens = false;
	}

	FFlockStoredTokens Stored;
	bool bHasTokens = false;
	bool bFailSave = false;
	bool bFailLoad = false;
	int32 SaveCount = 0;
	int32 LoadCount = 0;
	int32 ClearCount = 0;
};
