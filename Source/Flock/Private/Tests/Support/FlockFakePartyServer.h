// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Http/FlockHttpAdapter.h"
#include "Misc/Base64.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/**
 * The party routes as the backend answers them, measured against it (Libraries/Unreal/multiplayer-parties/probe_party_rules.py):
 * members in join order, the first of the others leading when the leader leaves, the last one out disbanding it, a code
 * matched trimmed and in any letter case, a re-join of the same party answered 200, update replacing the whole settings and
 * refusing a size below the roster (party.full), making yourself leader a 200, a kicked player answered party.not_found by
 * the party, null by "my party" and party.not_a_member by leave, and a non-member's kick a 403. The requester is the JWT's
 * subject. Include only from guarded test files.
 */
class FFlockFakePartyServer : public IFlockHttpAdapter
{
public:
	enum class ERoute : uint8 { Create, Join, Mine, Read, Update, Leave, Kick, MakeLeader, Disband, Other };

	struct FParty
	{
		FString Id;
		FString InviteCode;
		FString LeaderPlayerId;
		int32 MaxSize = 4;
		TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
		TArray<FString> Members;
		bool bActive = true;
	};

	/** A request a test holds: its answer is what the server did when it arrived, delivered when the test releases it. */
	struct FHeld
	{
		bool bArrived = false;
		bool bDelivered = false;
		TFunction<void(FFlockHttpResponse)> OnComplete;
		FFlockHttpResponse Response;

		void Release()
		{
			if (bArrived && !bDelivered)
			{
				bDelivered = true;
				OnComplete(Response);
			}
		}
	};

	TArray<FParty> Parties;
	TArray<FFlockHttpRequest> Requests;

	/** Told the party's id whenever its players change, as the backend then stops the party's search for a match. */
	TFunction<void(const FString&)> RosterChanged;

	/** Holds the next request of Route; it is answered (as when it arrived) by Release, or as cancelled by a cancel. */
	TSharedRef<FHeld> HoldNext(ERoute Route)
	{
		const TSharedRef<FHeld> Held = MakeShared<FHeld>();
		HoldQueue.FindOrAdd(Route).Add(Held);
		return Held;
	}

	/** Answers the next request of Route with Response instead of serving it; the server's state does not change. */
	void AnswerNext(ERoute Route, const FFlockHttpResponse& Response) { AnswerQueue.FindOrAdd(Route).Add(Response); }

	int32 Count(ERoute Route) const
	{
		int32 Found = 0;
		for (const FFlockHttpRequest& Request : Requests)
		{
			Found += RouteOf(Request) == Route ? 1 : 0;
		}
		return Found;
	}

	/** The bearer each request of Route carried, in order. */
	TArray<FString> PlayersOf(ERoute Route) const
	{
		TArray<FString> Players;
		for (const FFlockHttpRequest& Request : Requests)
		{
			if (RouteOf(Request) == Route)
			{
				Players.Add(PlayerOf(Request));
			}
		}
		return Players;
	}

	FParty* PartyOf(const FString& PlayerId)
	{
		return Parties.FindByPredicate([&](const FParty& Party)
		{
			return Party.bActive && HasMember(Party, PlayerId);
		});
	}

	FParty* PartyById(const FString& PartyId)
	{
		return Parties.FindByPredicate([&](const FParty& Party) { return Party.Id.Equals(PartyId, ESearchCase::CaseSensitive); });
	}

	/** A party another player made on the server directly. */
	FParty& MadeBy(const FString& PlayerId)
	{
		FParty& Party = Parties.AddDefaulted_GetRef();
		++PartiesMade;
		Party.Id = FString::Printf(TEXT("party-%d"), PartiesMade);
		Party.InviteCode = FString::Printf(TEXT("INV%03d"), PartiesMade);
		Party.LeaderPlayerId = PlayerId;
		Party.Members.Add(PlayerId);
		return Party;
	}

	/** Another player joins on the server directly. */
	void Joins(const FString& PartyId, const FString& PlayerId)
	{
		if (FParty* Party = PartyById(PartyId))
		{
			Party->Members.Add(PlayerId);
			TellRosterChanged(PartyId);
		}
	}

	/** A player leaves on the server directly, as leave does (the next member leads; the last one out disbands it). */
	void Leaves(const FString& PartyId, const FString& PlayerId)
	{
		if (FParty* Party = PartyById(PartyId))
		{
			RemoveMember(*Party, PlayerId);
			TellRosterChanged(PartyId);
		}
	}

	void Disbands(const FString& PartyId)
	{
		if (FParty* Party = PartyById(PartyId))
		{
			Party->bActive = false;
			TellRosterChanged(PartyId);
		}
	}

	static FFlockHttpResponse Refused(int32 Status, const FString& Code, const FString& Message = TEXT("refused"))
	{
		const TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("code"), Code);
		Detail->SetStringField(TEXT("message"), Message);
		const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetObjectField(TEXT("detail"), Detail);
		return Answer(Status, Body);
	}

	static FFlockHttpResponse Answer(int32 Status, const TSharedRef<FJsonObject>& Body)
	{
		FFlockHttpResponse Response;
		Response.Result = EFlockHttpResult::Success;
		Response.StatusCode = Status;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Response.Body);
		FJsonSerializer::Serialize(Body, Writer);
		return Response;
	}

	static FString PlayerOf(const FFlockHttpRequest& Request)
	{
		const FString* Authorization = Request.Headers.Find(TEXT("Authorization"));
		FString Token;
		if (Authorization == nullptr || !Authorization->Split(TEXT("Bearer "), nullptr, &Token))
		{
			return FString();
		}
		TArray<FString> Parts;
		Token.ParseIntoArray(Parts, TEXT("."));
		if (Parts.Num() < 2)
		{
			return FString();
		}
		FString Payload = Parts[1].Replace(TEXT("-"), TEXT("+")).Replace(TEXT("_"), TEXT("/"));
		while (Payload.Len() % 4 != 0)
		{
			Payload += TEXT("=");
		}
		FString Json;
		TSharedPtr<FJsonObject> Claims;
		FString Subject;
		if (FBase64::Decode(Payload, Json) && FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Json), Claims)
			&& Claims.IsValid())
		{
			Claims->TryGetStringField(TEXT("sub"), Subject);
		}
		return Subject;
	}

	static ERoute RouteOf(const FFlockHttpRequest& Request)
	{
		FString Path;
		if (!Request.Url.Split(TEXT("/v1/"), nullptr, &Path))
		{
			return ERoute::Other;
		}
		Path.Split(TEXT("?"), &Path, nullptr);
		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("/"));
		if (Segments.Num() == 0 || Segments[0] != TEXT("party"))
		{
			return ERoute::Other;
		}
		if (Segments.Num() == 1)
		{
			return Request.Method == TEXT("POST") ? ERoute::Create : ERoute::Other;
		}
		if (Segments.Num() == 2)
		{
			if (Segments[1] == TEXT("join"))
			{
				return ERoute::Join;
			}
			if (Segments[1] == TEXT("me"))
			{
				return ERoute::Mine;
			}
			return Request.Method == TEXT("GET") ? ERoute::Read : Request.Method == TEXT("PATCH") ? ERoute::Update
				: Request.Method == TEXT("DELETE") ? ERoute::Disband : ERoute::Other;
		}
		return Segments[2] == TEXT("leave") ? ERoute::Leave : Segments[2] == TEXT("kick") ? ERoute::Kick
			: Segments[2] == TEXT("transfer") ? ERoute::MakeLeader : ERoute::Other;
	}

	virtual FFlockRequestHandle SendAsync(const FFlockHttpRequest& Request, TFunction<void(FFlockHttpResponse)> OnComplete) override
	{
		Requests.Add(Request);
		const ERoute Route = RouteOf(Request);
		FFlockHttpResponse Response;
		TArray<FFlockHttpResponse>* Answers = AnswerQueue.Find(Route);
		if (Answers != nullptr && Answers->Num() > 0)
		{
			Response = (*Answers)[0];
			Answers->RemoveAt(0);
		}
		else
		{
			Response = Serve(Route, Request);
		}

		TArray<TSharedRef<FHeld>>* Holds = HoldQueue.Find(Route);
		if (Holds != nullptr && Holds->Num() > 0)
		{
			const TSharedRef<FHeld> Held = (*Holds)[0];
			Holds->RemoveAt(0);
			Held->bArrived = true;
			Held->OnComplete = OnComplete;
			Held->Response = Response;
			// A cancel answers a held request at once, as the engine's HTTP module does.
			const TSharedRef<FFlockCancelToken> Token = MakeShared<FFlockCancelToken>();
			const TWeakPtr<FHeld> WeakHeld = Held;
			Token->Canceller = [WeakHeld]()
			{
				const TSharedPtr<FHeld> Pinned = WeakHeld.Pin();
				if (Pinned.IsValid() && !Pinned->bDelivered)
				{
					Pinned->bDelivered = true;
					FFlockHttpResponse Cancelled;
					Cancelled.Result = EFlockHttpResult::Cancelled;
					Pinned->OnComplete(Cancelled);
				}
			};
			return FFlockRequestHandle(Token);
		}
		if (OnComplete)
		{
			OnComplete(Response);
		}
		return FFlockRequestHandle();
	}

private:
	TMap<ERoute, TArray<TSharedRef<FHeld>>> HoldQueue;
	TMap<ERoute, TArray<FFlockHttpResponse>> AnswerQueue;
	int32 PartiesMade = 0;

	void TellRosterChanged(const FString& PartyId) const
	{
		if (RosterChanged)
		{
			RosterChanged(PartyId);
		}
	}

	static TSharedRef<FJsonObject> PartyObject(const FParty& Party, bool bWithMembers)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("id"), Party.Id);
		Object->SetStringField(TEXT("game_id"), TEXT("game-1"));
		Object->SetStringField(TEXT("leader_player_id"), Party.LeaderPlayerId);
		Object->SetStringField(TEXT("status"), Party.bActive ? TEXT("active") : TEXT("disbanded"));
		Object->SetStringField(TEXT("invite_code"), Party.InviteCode);
		Object->SetNumberField(TEXT("max_size"), Party.MaxSize);
		Object->SetObjectField(TEXT("settings"), Party.Settings);
		Object->SetStringField(TEXT("created_at"), TEXT("2026-10-13T10:00:00Z"));
		Object->SetStringField(TEXT("updated_at"), TEXT("2026-10-13T10:00:00Z"));
		if (bWithMembers)
		{
			TArray<TSharedPtr<FJsonValue>> Members;
			for (const FString& Member : Party.Members)
			{
				const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("player_id"), Member);
				Row->SetField(TEXT("game_version_id"), MakeShared<FJsonValueNull>());
				Row->SetStringField(TEXT("joined_at"), TEXT("2026-10-13T10:00:00Z"));
				Members.Add(MakeShared<FJsonValueObject>(Row));
			}
			Object->SetArrayField(TEXT("members"), Members);
		}
		return Object;
	}

	static FFlockHttpResponse Enveloped(const FParty& Party, bool bWithMembers)
	{
		const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetObjectField(TEXT("result"), PartyObject(Party, bWithMembers));
		return Answer(200, Body);
	}

	static bool HasMember(const FParty& Party, const FString& PlayerId)
	{
		return Party.Members.ContainsByPredicate([&](const FString& Member) { return Member.Equals(PlayerId, ESearchCase::CaseSensitive); });
	}

	static void RemoveMember(FParty& Party, const FString& PlayerId)
	{
		Party.Members.RemoveAll([&](const FString& Member) { return Member.Equals(PlayerId, ESearchCase::CaseSensitive); });
		if (Party.Members.Num() == 0)
		{
			Party.bActive = false;
		}
		else if (Party.LeaderPlayerId.Equals(PlayerId, ESearchCase::CaseSensitive))
		{
			Party.LeaderPlayerId = Party.Members[0];
		}
	}

	FFlockHttpResponse Serve(ERoute Route, const FFlockHttpRequest& Request)
	{
		const FString Player = PlayerOf(Request);
		TSharedPtr<FJsonObject> Body;
		if (Request.bHasBody)
		{
			FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Request.JsonBody), Body);
		}
		FString PartyId;
		{
			FString Path;
			Request.Url.Split(TEXT("/v1/"), nullptr, &Path);
			TArray<FString> Segments;
			Path.ParseIntoArray(Segments, TEXT("/"));
			PartyId = Segments.Num() > 1 ? Segments[1] : FString();
		}

		switch (Route)
		{
		case ERoute::Create:
		{
			if (PartyOf(Player) != nullptr)
			{
				return Refused(409, TEXT("party.already_in_party"));
			}
			int32 Size = 4;
			if (Body.IsValid() && Body->HasField(TEXT("max_size")))
			{
				if (!Body->TryGetNumberField(TEXT("max_size"), Size) || Size < 2 || Size > 64)
				{
					return Refused(422, TEXT("request.validation_failed"));
				}
			}
			FParty& Party = MadeBy(Player);
			Party.MaxSize = Size;
			const TSharedPtr<FJsonObject>* Settings = nullptr;
			if (Body.IsValid() && Body->TryGetObjectField(TEXT("settings"), Settings) && Settings != nullptr)
			{
				Party.Settings = (*Settings).ToSharedRef();
			}
			return Enveloped(Party, false);
		}
		case ERoute::Join:
		{
			FString Code;
			if (Body.IsValid())
			{
				Body->TryGetStringField(TEXT("invite_code"), Code);
			}
			Code = Code.TrimStartAndEnd().ToUpper();
			FParty* Party = Parties.FindByPredicate([&](const FParty& Each) { return Each.bActive && Each.InviteCode.Equals(Code, ESearchCase::CaseSensitive); });
			if (Party == nullptr)
			{
				return Refused(404, TEXT("party.invalid_invite_code"));
			}
			if (FParty* Mine = PartyOf(Player))
			{
				return Mine == Party ? Enveloped(*Party, false) : Refused(409, TEXT("party.already_in_party"));
			}
			if (Party->Members.Num() >= Party->MaxSize)
			{
				return Refused(409, TEXT("party.full"));
			}
			Party->Members.Add(Player);
			TellRosterChanged(Party->Id);
			return Enveloped(*Party, false);
		}
		case ERoute::Mine:
		{
			const TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
			if (FParty* Party = PartyOf(Player))
			{
				Envelope->SetObjectField(TEXT("result"), PartyObject(*Party, true));
			}
			else
			{
				Envelope->SetField(TEXT("result"), MakeShared<FJsonValueNull>());
			}
			return Answer(200, Envelope);
		}
		case ERoute::Read:
		{
			FParty* Party = PartyById(PartyId);
			if (Party == nullptr || !Party->bActive || !HasMember(*Party, Player))
			{
				return Refused(404, TEXT("party.not_found"));
			}
			return Enveloped(*Party, true);
		}
		case ERoute::Leave:
		{
			FParty* Party = PartyById(PartyId);
			if (Party == nullptr || !Party->bActive)
			{
				return Refused(404, TEXT("party.not_found"));
			}
			if (!HasMember(*Party, Player))
			{
				return Refused(404, TEXT("party.not_a_member"));
			}
			RemoveMember(*Party, Player);
			TellRosterChanged(Party->Id);
			return Enveloped(*Party, false);
		}
		case ERoute::Update:
		case ERoute::Kick:
		case ERoute::MakeLeader:
		case ERoute::Disband:
		{
			FParty* Party = PartyById(PartyId);
			if (Party == nullptr || !Party->bActive)
			{
				return Refused(404, TEXT("party.not_found"));
			}
			if (!Party->LeaderPlayerId.Equals(Player, ESearchCase::CaseSensitive))
			{
				return Refused(403, TEXT("party.not_party_leader"));
			}
			FString Target;
			if (Body.IsValid())
			{
				Body->TryGetStringField(TEXT("player_id"), Target);
			}
			if (Route == ERoute::Update)
			{
				int32 Size = 0;
				if (Body.IsValid() && Body->TryGetNumberField(TEXT("max_size"), Size))
				{
					if (Size < 2 || Size > 64)
					{
						return Refused(422, TEXT("request.validation_failed"));
					}
					if (Size < Party->Members.Num())
					{
						return Refused(400, TEXT("party.full"));
					}
					Party->MaxSize = Size;
				}
				const TSharedPtr<FJsonObject>* Settings = nullptr;
				if (Body.IsValid() && Body->TryGetObjectField(TEXT("settings"), Settings) && Settings != nullptr)
				{
					Party->Settings = (*Settings).ToSharedRef();
				}
			}
			else if (Route == ERoute::Kick)
			{
				if (Target.Equals(Party->LeaderPlayerId, ESearchCase::CaseSensitive))
				{
					return Refused(400, TEXT("party.cannot_kick_leader"));
				}
				if (!HasMember(*Party, Target))
				{
					return Refused(404, TEXT("party.target_not_a_member"));
				}
				RemoveMember(*Party, Target);
				TellRosterChanged(Party->Id);
			}
			else if (Route == ERoute::MakeLeader)
			{
				if (!HasMember(*Party, Target))
				{
					return Refused(404, TEXT("party.target_not_a_member"));
				}
				Party->LeaderPlayerId = Target;
			}
			else
			{
				Party->bActive = false;
				TellRosterChanged(Party->Id);
			}
			return Enveloped(*Party, false);
		}
		default:
			// The player's search, read with every party refresh: none here, answered as the backend answers a player who never searched.
			if (Request.Url.Contains(TEXT("/v1/matchmaking/ticket/current"), ESearchCase::CaseSensitive))
			{
				return Refused(404, TEXT("matchmaking.ticket_not_found"));
			}
			return Refused(404, TEXT("not_found"));
		}
	}
};
