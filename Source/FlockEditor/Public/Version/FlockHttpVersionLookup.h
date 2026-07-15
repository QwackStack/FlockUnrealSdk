// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Version/FlockVersionLookup.h"

/**
 * Real, HTTP-backed Game Version lookup — replaces the default FFlockStubVersionLookup. Resolves the
 * version name to its ID via the runtime FFlockHttpClient against `game_version/by-name/{name}`.
 * CanResolve() returns true, which flips the build guard (UFlockConfigValidator) live.
 */
class FLOCKEDITOR_API FFlockHttpVersionLookup : public IFlockVersionLookup
{
public:
	virtual void Resolve(const FString& ApiUrl, const FString& ApiKey, const FString& GameVersion, FFlockResolveComplete OnComplete) override;
	virtual bool CanResolve() const override { return true; }
};
