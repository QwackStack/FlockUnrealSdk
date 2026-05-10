// Copyright 2022, Qwack. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "QwackConfig.generated.h"

/**
 * 
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Qwacks SDK Settings"))
class QWACK_UE_SDK_API UQwackConfig : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "QwackSDK", Meta = (DisplayName = "Qwack API Key"))
	FString QwackAPIKey;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "QwackSDK")
	FString GameBaseURI;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "QwackSDK")
	FString AuthBaseURI = "http://localhost:8080/";
	

	/*
	 * To override the flow process must pass player ID or long lived token
	 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "QwackSDK")
	bool DeveolpmentMode = false;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "QwackSDK")
	FString TestingPlayerID;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "QwackSDK")
	FString TestingToken;
};
