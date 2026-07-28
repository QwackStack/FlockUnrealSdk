// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

class UFlockConfig;
struct FFlockResolveResult;

/**
 * Resolves the Game Version name to its ID at edit time and bakes it onto UFlockConfig, so runtime
 * init needs no network.
 */
class FLOCKEDITOR_API FFlockVersionResolver
{
public:
	/** The backend by-name Game Version lookup URL. Single source so resolve and any connection-test agree. */
	static FString ByNameUrl(const FString& ApiUrl, const FString& GameVersion);

	/**
	 * Bakes a successful result onto the config (leaves the prior ID on failure). Returns whether the
	 * ID changed. Pure — the caller persists via TryUpdateDefaultConfigFile().
	 */
	static bool ApplyResult(UFlockConfig& Config, const FFlockResolveResult& Result);

	/**
	 * Editor convenience: resolve via the active lookup, then (on success) apply, persist to
	 * DefaultGame.ini, and surface the outcome as an editor toast. Fails cleanly when no version lookup
	 * is registered.
	 */
	static void ResolveAndBake(UFlockConfig& Config);
};
