// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestLibrary.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FlockPlaytestSubsystem.h"

namespace
{
	UFlockPlaytestSubsystem* FindPlaytestSubsystem(const UObject* WorldContextObject)
	{
		const UGameInstance* GameInstance = Cast<UGameInstance>(WorldContextObject);
		if (GameInstance == nullptr && GEngine != nullptr)
		{
			const UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
			GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;
		}
		return GameInstance != nullptr ? GameInstance->GetSubsystem<UFlockPlaytestSubsystem>() : nullptr;
	}
}

bool UFlockPlaytestLibrary::RecordPlaytestEvent(const UObject* WorldContextObject, const FString& EventName,
	const FFlockCommandData& Properties)
{
	UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->RecordPlaytestEvent(EventName, Properties);
}
