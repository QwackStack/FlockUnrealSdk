// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Models/FlockMultiplayerModels.h"
#include "Dom/JsonValue.h"

bool FFlockPartyReading::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPartyReading& OutStruct, FString& OutError)
{
	OutStruct = FFlockPartyReading();
	Object->TryGetStringField(TEXT("id"), OutStruct.Id);
	if (OutStruct.Id.IsEmpty())
	{
		OutError = TEXT("Invalid response from server (the answer names no party)");
		return false;
	}
	Object->TryGetStringField(TEXT("leader_player_id"), OutStruct.LeaderPlayerId);
	Object->TryGetStringField(TEXT("invite_code"), OutStruct.InviteCode);
	Object->TryGetNumberField(TEXT("max_size"), OutStruct.MaxSize);
	// The game's own data, its keys exactly as the leader sent them.
	OutStruct.Settings = FFlockJsonData::FromJson(Object->TryGetField(TEXT("settings")));

	const TArray<TSharedPtr<FJsonValue>>* Members = nullptr;
	if (Object->TryGetArrayField(TEXT("members"), Members) && Members != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Member : *Members)
		{
			const TSharedPtr<FJsonObject>* MemberObject = nullptr;
			FString PlayerId;
			if (!Member.IsValid() || !Member->TryGetObject(MemberObject) || MemberObject == nullptr
				|| !(*MemberObject)->TryGetStringField(TEXT("player_id"), PlayerId) || PlayerId.IsEmpty())
			{
				OutError = TEXT("Invalid response from server (a party member with no player id)");
				return false;
			}
			OutStruct.MemberIds.Add(PlayerId);
		}
	}
	return true;
}

bool FFlockMyPartyReading::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockMyPartyReading& OutStruct, FString& OutError)
{
	OutStruct = FFlockMyPartyReading();
	const TSharedPtr<FJsonValue> Result = Object->TryGetField(TEXT("result"));
	if (!Result.IsValid())
	{
		OutError = TEXT("Invalid response from server (missing result)");
		return false;
	}
	if (Result->IsNull())
	{
		return true;
	}
	const TSharedPtr<FJsonObject>* PartyObject = nullptr;
	if (!Result->TryGetObject(PartyObject) || PartyObject == nullptr || !PartyObject->IsValid())
	{
		OutError = TEXT("Invalid response from server (the result is not a party)");
		return false;
	}
	if (!FFlockPartyReading::FromWireObject((*PartyObject).ToSharedRef(), OutStruct.Party, OutError))
	{
		return false;
	}
	OutStruct.bInParty = true;
	return true;
}
