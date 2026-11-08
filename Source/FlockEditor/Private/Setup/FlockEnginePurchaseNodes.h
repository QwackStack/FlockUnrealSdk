// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

class UBlueprint;

/**
 * Finds the game's Blueprints that use the engine's own in-app purchase nodes (the "In-App Purchase" nodes of the engine's
 * online subsystem utilities, current and deprecated). Those nodes finish a store purchase themselves, before Flock delivers it.
 */
struct FFlockEnginePurchaseNodes
{
	/**
	 * The paths of the project's Blueprints (under /Game) that use them. Only Blueprints that depend on the engine's online
	 * subsystem utilities are loaded and looked through, so a project that uses none loads nothing.
	 */
	static TArray<FString> FindBlueprintsUsingThem();

	/** True when a node of Blueprint is one of the engine's in-app purchase nodes. */
	static bool UsesThem(const UBlueprint& Blueprint);

	/** True when Class is one of the engine's in-app purchase node classes (their names start with InAppPurchase). */
	static bool IsAnEnginePurchaseClass(const UClass* Class);
};
