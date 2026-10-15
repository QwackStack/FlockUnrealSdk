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

bool FFlockMultiplayerSessionJoinToken::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockMultiplayerSessionJoinToken& OutStruct,
	FString& OutError)
{
	OutStruct = FFlockMultiplayerSessionJoinToken();
	Object->TryGetStringField(TEXT("token"), OutStruct.Token);
	if (OutStruct.Token.IsEmpty())
	{
		OutError = TEXT("Invalid response from server (the answer names no join token)");
		return false;
	}
	Object->TryGetNumberField(TEXT("expires_in"), OutStruct.ExpiresInSeconds);
	return true;
}

bool FFlockVerifiedJoinToken::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockVerifiedJoinToken& OutStruct, FString& OutError)
{
	OutStruct = FFlockVerifiedJoinToken();
	Object->TryGetStringField(TEXT("session_id"), OutStruct.SessionId);
	Object->TryGetStringField(TEXT("player_id"), OutStruct.PlayerId);
	if (OutStruct.PlayerId.IsEmpty())
	{
		OutError = TEXT("Invalid response from server (the answer names no player)");
		return false;
	}
	return true;
}

bool FFlockStunServerList::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockStunServerList& OutStruct, FString& OutError)
{
	OutStruct = FFlockStunServerList();
	const TArray<TSharedPtr<FJsonValue>>* Servers = nullptr;
	if (!Object->TryGetArrayField(TEXT("ice_servers"), Servers) || Servers == nullptr)
	{
		OutError = TEXT("Invalid response from server (the answer lists no servers)");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Server : *Servers)
	{
		const TSharedPtr<FJsonObject>* ServerObject = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Urls = nullptr;
		if (!Server.IsValid() || !Server->TryGetObject(ServerObject) || ServerObject == nullptr
			|| !(*ServerObject)->TryGetArrayField(TEXT("urls"), Urls) || Urls == nullptr)
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& Url : *Urls)
		{
			FString Text;
			// A relay entry carries a login and is for the relay alone; only the STUN servers say where this device is seen from.
			if (Url.IsValid() && Url->TryGetString(Text) && Text.StartsWith(TEXT("stun:"), ESearchCase::IgnoreCase))
			{
				OutStruct.Urls.Add(Text);
			}
		}
	}
	return true;
}

TArray<FString> FFlockMultiplayerSessionReading::SeatedPlayerIds() const
{
	TArray<FString> Seated;
	for (const FFlockMultiplayerSessionSeat& Seat : Seats)
	{
		if (Seat.Status.Equals(FlockMultiplayerSessionWireValues::SeatJoined, ESearchCase::CaseSensitive))
		{
			Seated.Add(Seat.PlayerId);
		}
	}
	return Seated;
}

const FFlockMultiplayerSessionSeat* FFlockMultiplayerSessionReading::FindSeat(const FString& PlayerId) const
{
	return Seats.FindByPredicate([&PlayerId](const FFlockMultiplayerSessionSeat& Seat) { return Seat.PlayerId.Equals(PlayerId, ESearchCase::CaseSensitive); });
}

bool FFlockMultiplayerSessionReading::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockMultiplayerSessionReading& OutStruct,
	FString& OutError)
{
	OutStruct = FFlockMultiplayerSessionReading();
	Object->TryGetStringField(TEXT("id"), OutStruct.Id);
	if (OutStruct.Id.IsEmpty())
	{
		OutError = TEXT("Invalid response from server (the answer names no session)");
		return false;
	}
	Object->TryGetStringField(TEXT("status"), OutStruct.Status);
	Object->TryGetStringField(TEXT("ended_reason"), OutStruct.EndedReason);
	Object->TryGetStringField(TEXT("host_player_id"), OutStruct.HostPlayerId);
	Object->TryGetNumberField(TEXT("host_epoch"), OutStruct.HostEpoch);
	Object->TryGetStringField(TEXT("join_code"), OutStruct.JoinCode);
	Object->TryGetNumberField(TEXT("max_players"), OutStruct.MaxPlayers);
	Object->TryGetNumberField(TEXT("connection_epoch"), OutStruct.ConnectionEpoch);
	Object->TryGetNumberField(TEXT("heartbeat_interval_seconds"), OutStruct.HeartbeatIntervalSeconds);
	// The game's own data, its keys exactly as the host sent them.
	OutStruct.Data = FFlockJsonData::FromJson(Object->TryGetField(TEXT("data")));

	// The server sends {} before the host first publishes, and to a player without a seat; a published one always has a mode.
	const TSharedPtr<FJsonObject>* ConnectionObject = nullptr;
	FString Mode;
	if (Object->TryGetObjectField(TEXT("connection_info"), ConnectionObject) && ConnectionObject != nullptr && ConnectionObject->IsValid()
		&& (*ConnectionObject)->TryGetStringField(TEXT("mode"), Mode) && !Mode.IsEmpty())
	{
		OutStruct.bHasConnection = true;
		OutStruct.Connection.Mode = Mode;
		OutStruct.Connection.Values = FFlockJsonData::FromJson(Object->TryGetField(TEXT("connection_info")));
	}

	const TArray<TSharedPtr<FJsonValue>>* Participants = nullptr;
	if (Object->TryGetArrayField(TEXT("participants"), Participants) && Participants != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Participant : *Participants)
		{
			const TSharedPtr<FJsonObject>* ParticipantObject = nullptr;
			FFlockMultiplayerSessionSeat Seat;
			if (!Participant.IsValid() || !Participant->TryGetObject(ParticipantObject) || ParticipantObject == nullptr
				|| !(*ParticipantObject)->TryGetStringField(TEXT("player_id"), Seat.PlayerId) || Seat.PlayerId.IsEmpty())
			{
				OutError = TEXT("Invalid response from server (a seat with no player id)");
				return false;
			}
			(*ParticipantObject)->TryGetStringField(TEXT("status"), Seat.Status);
			OutStruct.Seats.Add(Seat);
		}
	}
	return true;
}

bool FFlockMatchmakingTicketReading::IsQueued() const
{
	return Status.Equals(FlockMatchmakingWireValues::TicketQueued, ESearchCase::CaseSensitive);
}

bool FFlockMatchmakingTicketReading::IsMatched() const
{
	return Status.Equals(FlockMatchmakingWireValues::TicketMatched, ESearchCase::CaseSensitive);
}

bool FFlockMatchmakingTicketReading::IsForParty(const FString& InPartyId) const
{
	return PartyId.Equals(InPartyId, ESearchCase::CaseSensitive);
}

bool FFlockMatchmakingTicketReading::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockMatchmakingTicketReading& OutStruct, FString& OutError)
{
	OutStruct = FFlockMatchmakingTicketReading();
	Object->TryGetStringField(TEXT("id"), OutStruct.Id);
	Object->TryGetStringField(TEXT("status"), OutStruct.Status);
	if (OutStruct.Id.IsEmpty() || OutStruct.Status.IsEmpty())
	{
		OutError = TEXT("Invalid response from server (the answer names no search)");
		return false;
	}
	Object->TryGetStringField(TEXT("queue_id"), OutStruct.QueueId);
	Object->TryGetStringField(TEXT("player_id"), OutStruct.PlayerId);
	// Null on the wire for a search alone, which a string read leaves empty.
	Object->TryGetStringField(TEXT("party_id"), OutStruct.PartyId);
	Object->TryGetStringField(TEXT("cancel_reason"), OutStruct.CancelReason);

	const TSharedPtr<FJsonObject>* MatchObject = nullptr;
	if (Object->TryGetObjectField(TEXT("match"), MatchObject) && MatchObject != nullptr && MatchObject->IsValid())
	{
		OutStruct.bHasMatch = true;
		(*MatchObject)->TryGetStringField(TEXT("id"), OutStruct.Match.Id);
		const TArray<TSharedPtr<FJsonValue>>* Players = nullptr;
		if ((*MatchObject)->TryGetArrayField(TEXT("player_ids"), Players) && Players != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Player : *Players)
			{
				FString PlayerId;
				if (Player.IsValid() && Player->TryGetString(PlayerId) && !PlayerId.IsEmpty())
				{
					OutStruct.Match.PlayerIds.Add(PlayerId);
				}
			}
		}
		const TSharedPtr<FJsonObject>* Connection = nullptr;
		if ((*MatchObject)->TryGetObjectField(TEXT("connection_info"), Connection) && Connection != nullptr && Connection->IsValid())
		{
			(*Connection)->TryGetStringField(TEXT("session_id"), OutStruct.Match.SessionId);
		}
	}
	return true;
}

const FFlockMatchmakingTicketReading* FFlockMatchmakingTicketsMade::FindTicketOf(const FString& PlayerId) const
{
	return Tickets.FindByPredicate([&](const FFlockMatchmakingTicketReading& Ticket)
	{
		return Ticket.PlayerId.Equals(PlayerId, ESearchCase::CaseSensitive);
	});
}

bool FFlockMatchmakingTicketsMade::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockMatchmakingTicketsMade& OutStruct, FString& OutError)
{
	OutStruct = FFlockMatchmakingTicketsMade();
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Object->TryGetArrayField(TEXT("result"), Items) || Items == nullptr)
	{
		OutError = TEXT("Invalid response from server (missing result)");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Item : *Items)
	{
		const TSharedPtr<FJsonObject>* TicketObject = nullptr;
		if (!Item.IsValid() || !Item->TryGetObject(TicketObject) || TicketObject == nullptr || !TicketObject->IsValid())
		{
			OutError = TEXT("Invalid response from server (a search that is not one)");
			return false;
		}
		FFlockMatchmakingTicketReading Ticket;
		if (!FFlockMatchmakingTicketReading::FromWireObject((*TicketObject).ToSharedRef(), Ticket, OutError))
		{
			return false;
		}
		OutStruct.Tickets.Add(Ticket);
	}
	if (OutStruct.Tickets.Num() == 0)
	{
		OutError = TEXT("Invalid response from server (no search was made)");
		return false;
	}
	return true;
}
