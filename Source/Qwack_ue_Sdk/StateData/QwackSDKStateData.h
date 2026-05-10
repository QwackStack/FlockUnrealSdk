// Copyright 2022, SaharaStorms. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "QwackSDKStateData.generated.h"

/**
 * 
 */
UCLASS()
class QWACK_UE_SDK_API UQwackSDKStateData : public UObject
{
	GENERATED_BODY()
	static FString PlayerToken;
	static FString PlayerSteamToken;
	static FString PlayerRefreshToken;
	static FString PlayerID;
	static FString ServerToken;
	static FString ServerRefreshToken;
	inline static const FString SaveSlot = "SaharaSDK";
	inline static constexpr int SaveIndex = 0;

	static bool DataLoaded;
	static void LoadData();
	static void SaveData();
public:
	UFUNCTION(BlueprintCallable)
	static FString GetToken();
	UFUNCTION(BlueprintCallable)
	static FString GetPlayerSteamToken();
	static FString GetPlayerRefreshToken();
	static FString GetPlayerID();
	UFUNCTION(BlueprintCallable)
	static FString GetServerToken();
	UFUNCTION(BlueprintCallable)
	static FString GetServerRefreshToken();
	static void SetPlayerTokenn(FString NewToken);
	static void SetPlayerSteamToken(FString NewPlayerSteamToken);
	static void SetPlayerRefreshToken(FString NewPlayerRefreshToken);
	static void SetPlayerID(FString NewPlayerID);

	static void SetServerToken(FString NewServerToken);
	static void SetServerRefreshToken(FString NewServerRefreshToken);
};
