// Copyright 2022, Qwack. All Rights Reserved.


#include "QwackSDKStateData.h"

#include "QwackPersistedDataState.h"
#include "Kismet/GameplayStatics.h"


FString UQwackSDKStateData::PlayerToken = "";
FString UQwackSDKStateData::PlayerSteamToken = "";
FString UQwackSDKStateData::PlayerRefreshToken = "";
FString UQwackSDKStateData::ServerToken = "";
FString UQwackSDKStateData::ServerRefreshToken = "";
FString UQwackSDKStateData::PlayerID = FGenericPlatformMisc::GetDeviceId != nullptr ? FGenericPlatformMisc::GetDeviceId() : FGuid::NewGuid().ToString();
bool UQwackSDKStateData::DataLoaded = false;
void UQwackSDKStateData::LoadData()
{
	/*if(DataLoaded)
	{
		return;
	}*/
	if (UQwackPersistedDataState* LoadedDataState = Cast<UQwackPersistedDataState>(UGameplayStatics::LoadGameFromSlot(SaveSlot, SaveIndex)))
	{
		PlayerToken = LoadedDataState->PlayerToken.IsEmpty() ? PlayerToken : LoadedDataState->PlayerToken;
		PlayerID = LoadedDataState->PlayerID.IsEmpty() ? FPlatformMisc::GetDeviceId() : LoadedDataState->PlayerID;
		PlayerSteamToken = LoadedDataState->PlayerSteamToken.IsEmpty() ?  PlayerSteamToken :LoadedDataState->PlayerSteamToken ;
		PlayerRefreshToken = LoadedDataState->PlayerRefreshToken;
		ServerToken = LoadedDataState->ServerToken.IsEmpty()? ServerToken : LoadedDataState->ServerToken;
		ServerRefreshToken = LoadedDataState->ServerRefreshToken.IsEmpty()? ServerRefreshToken : LoadedDataState->ServerRefreshToken ;
		//UE_LOG(LogTemp, Log, TEXT("LOADED SAVE STATE "));
		DataLoaded = true;
		return;
	}
	UE_LOG(LogTemp, Log, TEXT("FAILED TO LOAD SAVE STATE "));
}

void UQwackSDKStateData::SaveData()
{
	if (UQwackPersistedDataState* SavedDataState = Cast<UQwackPersistedDataState>(UGameplayStatics::CreateSaveGameObject(UQwackPersistedDataState::StaticClass())))
	{
		SavedDataState->PlayerToken = PlayerToken;
		SavedDataState->PlayerID = PlayerID;
		SavedDataState->PlayerSteamToken = PlayerSteamToken;
		SavedDataState->PlayerRefreshToken = PlayerRefreshToken;
		SavedDataState->ServerToken = ServerToken;

		if (UGameplayStatics::SaveGameToSlot(SavedDataState, SaveSlot, SaveIndex)) {
			
			return;
		}
	}
	
}

FString UQwackSDKStateData::GetToken()
{
	LoadData();
	return PlayerToken;
}

FString UQwackSDKStateData::GetPlayerSteamToken()
{
	LoadData();
	return PlayerSteamToken;
}

FString UQwackSDKStateData::GetPlayerRefreshToken()
{
	LoadData();
	return PlayerRefreshToken;
}

FString UQwackSDKStateData::GetPlayerID()
{
	LoadData();
	return PlayerID;
}

FString UQwackSDKStateData::GetServerToken()
{
	LoadData();
	return ServerToken;
}

FString UQwackSDKStateData::GetServerRefreshToken()
{
	LoadData();
	return ServerRefreshToken;
}

void UQwackSDKStateData::SetPlayerTokenn(FString NewToken)
{
	LoadData();
	if(NewToken.Equals(PlayerToken))
	{
		return;
	}
	PlayerToken = NewToken;
	SaveData();
}

void UQwackSDKStateData::SetPlayerSteamToken(FString NewPlayerSteamToken)
{
	LoadData();
	if(NewPlayerSteamToken.Equals(PlayerSteamToken))
	{
		return;
	}
	PlayerSteamToken = NewPlayerSteamToken;
	SaveData();
}

void UQwackSDKStateData::SetPlayerRefreshToken(FString NewPlayerRefreshToken)
{
	LoadData();
	if(NewPlayerRefreshToken.Equals(PlayerRefreshToken))
	{
		return;
	}
	PlayerRefreshToken = NewPlayerRefreshToken;
	SaveData();
}

void UQwackSDKStateData::SetPlayerID(FString NewPlayerID)
{
	LoadData();
	if(NewPlayerID.Equals(PlayerID))
	{
		return;
	}
	PlayerID = NewPlayerID;
	SaveData();
}

void UQwackSDKStateData::SetServerToken(FString NewServerToken)
{
	LoadData();
	if(NewServerToken.Equals(PlayerID))
	{
		return;
	}
	ServerToken = NewServerToken;
	SaveData();
}

void UQwackSDKStateData::SetServerRefreshToken(FString NewServerRefreshToken)
{
	LoadData();
	if(NewServerRefreshToken.Equals(PlayerID))
	{
		return;
	}
	ServerRefreshToken = NewServerRefreshToken;
	SaveData();
}
