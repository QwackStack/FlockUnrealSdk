// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

/** How a Blueprint node that waits is kept alive: by the game instance its World Context leads to, until a pin has fired. */
namespace FlockBlueprintNodeLife
{
	/**
	 * The game instance a node registers with, so it keeps the node until a pin has fired: the graph that made it holds it only
	 * in a variable a second call of the same node replaces, and a search, a travel or a web checkout's payment outlives that by
	 * minutes. Null when the world context leads to none, which registering ignores (and logs nothing, unlike the engine's own
	 * lookup).
	 */
	inline UGameInstance* GameInstanceOf(const UObject* WorldContextObject)
	{
		if (WorldContextObject == nullptr)
		{
			return nullptr;
		}
		const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
		if (UGameInstance* Instance = World ? World->GetGameInstance() : nullptr)
		{
			return Instance;
		}
		if (const UGameInstance* Instance = Cast<UGameInstance>(WorldContextObject))
		{
			return const_cast<UGameInstance*>(Instance);
		}
		return WorldContextObject->GetTypedOuter<UGameInstance>();
	}
}
