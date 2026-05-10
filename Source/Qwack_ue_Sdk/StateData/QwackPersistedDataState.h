// Copyright 2022, Qwack. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"

#include "QwackPersistedDataState.generated.h"

/**
 * 
 */
UCLASS()
class QWACK_UE_SDK_API UQwackPersistedDataState : public USaveGame
{
	GENERATED_BODY()
public:
	UPROPERTY(VisibleAnywhere)
	FString PlayerToken = "";
	UPROPERTY(VisibleAnywhere)
	FString PlayerSteamToken = "";
	UPROPERTY(VisibleAnywhere)
	FString PlayerRefreshToken = "";
	UPROPERTY(VisibleAnywhere)
	FString PlayerID = "";

	FString ServerToken = "";
	FString ServerRefreshToken = "";
};
