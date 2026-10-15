// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Http/FlockHttpAdapter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/Support/FlockFakePartyServer.h"
#include "Tests/Support/FlockFakeSessionServer.h"

/**
 * The matchmaking ticket routes, with the party and session fakes behind them, and the backend's rules as measured on
 * 2026-10-08 and again on 2026-10-15 (Libraries/Unreal/multiplayer-matchmaking/probe_matchmaking_rules.py, 33/33): a party is
 * queued by its leader as one ticket per member and never split, a player searches once at a time, a ticket is read and
 * cancelled by its own player only, any member's cancel stops the party's search, a roster change stops it as party_changed, a
 * match makes its session (the longest waiting hosts) unless the game's settings make none, and each refusal has the backend's
 * own status and code. Matches form, and tickets expire, when a test says so. The requester is the JWT's subject. Include only
 * from guarded test files.
 */
class FFlockFakeMatchmakingServer : public IFlockHttpAdapter
{
public:
	enum class ERoute : uint8 { Queues, Create, Current, ReadTicket, Cancel, Other };

	static constexpr const TCHAR* Duel = TEXT("duel");
	static constexpr const TCHAR* Trio = TEXT("trio");
	static constexpr const TCHAR* DuelId = TEXT("01QUEUEDUEL000000000000000");
	static constexpr const TCHAR* TrioId = TEXT("01QUEUETRIO000000000000000");

	struct FMatch
	{
		FString Id;
		FString QueueId;
		TArray<FString> PlayerIds;
		FString SessionId;
	};

	struct FTicket
	{
		FString Id;
		FString QueueId;
		FString PlayerId;
		FString Status = TEXT("queued");
		FString PartyId;
		int32 PartySize = 1;
		FString CancelReason;
		TSharedPtr<FMatch> Match;
		FString GameVersion;
		int32 Order = 0;

		bool IsQueued() const { return Status == TEXT("queued"); }
	};
	using FTicketRef = TSharedRef<FTicket>;
	using FHeld = FFlockFakePartyServer::FHeld;

	TSharedRef<FFlockFakeSessionServer> Sessions = MakeShared<FFlockFakeSessionServer>();
	TSharedRef<FFlockFakePartyServer> Parties = MakeShared<FFlockFakePartyServer>();

	/** The game's multiplayer setting "make a session on a match"; on by default, as on the backend. */
	bool bMakesSessions = true;

	/** The game's queues, as the dashboard holds them: duel (2 players) and trio (3). */
	struct FQueue
	{
		FString Id;
		FString Name;
		int32 Size = 0;
	};
	TArray<FQueue> Queues = { { DuelId, Duel, 2 }, { TrioId, Trio, 3 } };

	/** A queue deleted in the dashboard and made again under the same name: a new id; an empty one deletes it. */
	void RemakesQueue(const FString& Name, const FString& NewId)
	{
		Queues.RemoveAll([&](const FQueue& Queue) { return Queue.Name.Equals(Name, ESearchCase::CaseSensitive); });
		if (!NewId.IsEmpty())
		{
			Queues.Add({ NewId, Name, Name == Duel ? 2 : 3 });
		}
	}

	/** Every request this fake served itself (party and session requests go to their own fakes), in order. */
	TArray<FFlockHttpRequest> Requests;

	FFlockFakeMatchmakingServer()
	{
		const TSharedRef<TArray<FTicketRef>> Stoppable = Tickets;
		Parties->RosterChanged = [Stoppable](const FString& PartyId)
		{
			for (const FTicketRef& Ticket : *Stoppable)
			{
				if (Ticket->IsQueued() && Ticket->PartyId.Equals(PartyId, ESearchCase::CaseSensitive))
				{
					Ticket->Status = TEXT("cancelled");
					Ticket->CancelReason = TEXT("party_changed");
				}
			}
		};
	}

	int32 Count(ERoute Route) const { return RequestsTo(Route).Num(); }

	TArray<FFlockHttpRequest> RequestsTo(ERoute Route) const
	{
		return Requests.FilterByPredicate([&](const FFlockHttpRequest& Request) { return RouteOf(Request) == Route; });
	}

	/** The player each request of Route went out as, in order. */
	TArray<FString> PlayersOf(ERoute Route) const
	{
		TArray<FString> Players;
		for (const FFlockHttpRequest& Request : RequestsTo(Route))
		{
			Players.Add(FFlockFakePartyServer::PlayerOf(Request));
		}
		return Players;
	}

	/** Holds the next request of Route; it is answered (as when it arrived) by Release, or as cancelled by a cancel. */
	TSharedRef<FHeld> HoldNext(ERoute Route)
	{
		const TSharedRef<FHeld> Held = MakeShared<FHeld>();
		HoldQueue.FindOrAdd(Route).Add(Held);
		return Held;
	}

	/** Answers the next request of Route with Response instead of serving it; the server's state does not change. */
	void AnswerNext(ERoute Route, const FFlockHttpResponse& Response) { AnswerQueue.FindOrAdd(Route).Add(Response); }

	TArray<FTicketRef> TicketsOf(const FString& PlayerId) const
	{
		return Tickets->FilterByPredicate([&](const FTicketRef& Ticket) { return Ticket->PlayerId.Equals(PlayerId, ESearchCase::CaseSensitive); });
	}

	TSharedPtr<FTicket> QueuedTicketOf(const FString& PlayerId) const
	{
		const FTicketRef* Found = Tickets->FindByPredicate([&](const FTicketRef& Ticket)
		{
			return Ticket->IsQueued() && Ticket->PlayerId.Equals(PlayerId, ESearchCase::CaseSensitive);
		});
		return Found != nullptr ? TSharedPtr<FTicket>(*Found) : TSharedPtr<FTicket>();
	}

	// ---- what other players, or the server's engine, do ----

	/** A player alone, or a party by its leader, joins a queue without the checks a request goes through. */
	TArray<FTicketRef> QueuedBy(const FString& QueueId, const FString& PlayerId, const FFlockFakePartyServer::FParty* Party = nullptr,
		const FString& GameVersion = FFlockFakeSessionServer::GameBuild)
	{
		return MakeTickets(QueueId, Party == nullptr ? TArray<FString>{ PlayerId } : Party->Members, Party != nullptr ? Party->Id : FString(),
			GameVersion);
	}

	/** One tick of the engine: in each queue, the longest waiting fill a match, a party never split and skipped while it does not fit. */
	TArray<TSharedRef<FMatch>> FormMatches()
	{
		TArray<TSharedRef<FMatch>> Formed;
		TArray<FString> Versions;
		for (const FQueue& Queue : Queues)
		{
			TArray<FTicketRef> Waiting = Tickets->FilterByPredicate([&](const FTicketRef& Ticket)
			{
				return Ticket->IsQueued() && Ticket->QueueId == Queue.Id;
			});
			Waiting.StableSort([](const FTicketRef& A, const FTicketRef& B) { return A->Order < B->Order; });
			// A party's tickets go together, in the order the party first appears.
			TArray<TArray<FTicketRef>> Units;
			TArray<FString> UnitKeys;
			for (const FTicketRef& Ticket : Waiting)
			{
				const FString Key = Ticket->PartyId.IsEmpty() ? Ticket->Id : Ticket->PartyId;
				const int32 Index = UnitKeys.IndexOfByKey(Key);
				if (Index == INDEX_NONE)
				{
					UnitKeys.Add(Key);
					Units.Add({ Ticket });
				}
				else
				{
					Units[Index].Add(Ticket);
				}
			}
			TArray<FTicketRef> Chosen;
			for (const TArray<FTicketRef>& Unit : Units)
			{
				if (Chosen.Num() + Unit.Num() > Queue.Size)
				{
					continue;
				}
				Chosen.Append(Unit);
				if (Chosen.Num() < Queue.Size)
				{
					continue;
				}
				const TSharedRef<FMatch> Match = MakeShared<FMatch>();
				Match->Id = FString::Printf(TEXT("01MATCH%019d"), ++IdsMade);
				Match->QueueId = Queue.Id;
				for (const FTicketRef& Ticket : Chosen)
				{
					Match->PlayerIds.Add(Ticket->PlayerId);
					Ticket->Status = TEXT("matched");
					Ticket->Match = Match;
				}
				Formed.Add(Match);
				Versions.Add(Chosen[0]->GameVersion);
				Chosen.Reset();
			}
		}
		if (bMakesSessions)
		{
			for (int32 Index = 0; Index < Formed.Num(); ++Index)
			{
				Formed[Index]->SessionId = Sessions->SeatedByMatch(Formed[Index]->PlayerIds, Versions[Index])->Id;
			}
		}
		return Formed;
	}

	/** The engine's sweep after 5 minutes, for every ticket still queued. */
	void ExpiresAll()
	{
		for (const FTicketRef& Ticket : *Tickets)
		{
			if (Ticket->IsQueued())
			{
				Ticket->Status = TEXT("expired");
			}
		}
	}

	/** The player's other game, or another member of the party, cancels the search. */
	void CancelledElsewhere(const FTicketRef& Ticket) { StopSearch(*Ticket, TEXT("player")); }

	static FFlockHttpResponse Refused(int32 Status, const FString& Code)
	{
		return FFlockFakePartyServer::Refused(Status, Code, TEXT("refused by the test server"));
	}

	static ERoute RouteOf(const FFlockHttpRequest& Request)
	{
		FString Path;
		if (!Request.Url.Split(TEXT("/v1/"), nullptr, &Path))
		{
			return ERoute::Other;
		}
		Path.Split(TEXT("?"), &Path, nullptr);
		if (Path == TEXT("matchmaking/queues"))
		{
			return ERoute::Queues;
		}
		if (Path == TEXT("matchmaking/ticket"))
		{
			return Request.Method == TEXT("POST") ? ERoute::Create : ERoute::Other;
		}
		if (Path == TEXT("matchmaking/ticket/current"))
		{
			return ERoute::Current;
		}
		if (Path.StartsWith(TEXT("matchmaking/ticket/"), ESearchCase::CaseSensitive))
		{
			return Request.Method == TEXT("DELETE") ? ERoute::Cancel : ERoute::ReadTicket;
		}
		return ERoute::Other;
	}

	// ---- the wire ----

	virtual FFlockRequestHandle SendAsync(const FFlockHttpRequest& Request, TFunction<void(FFlockHttpResponse)> OnComplete) override
	{
		FString Path;
		Request.Url.Split(TEXT("/v1/"), nullptr, &Path);
		if (Path == TEXT("party") || Path.StartsWith(TEXT("party/"), ESearchCase::CaseSensitive))
		{
			return Parties->SendAsync(Request, MoveTemp(OnComplete));
		}
		if (Path.StartsWith(TEXT("multiplayer/"), ESearchCase::CaseSensitive))
		{
			return Sessions->SendAsync(Request, MoveTemp(OnComplete));
		}

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
			Response = Serve(Route, Path, Request);
		}

		TArray<TSharedRef<FHeld>>* Holds = HoldQueue.Find(Route);
		if (Holds != nullptr && Holds->Num() > 0)
		{
			const TSharedRef<FHeld> Held = (*Holds)[0];
			Holds->RemoveAt(0);
			Held->bArrived = true;
			Held->OnComplete = OnComplete;
			Held->Response = Response;
			// A cancel answers a held request at once, as the engine's HTTP module does; the server has served it all the same.
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
	TSharedRef<TArray<FTicketRef>> Tickets = MakeShared<TArray<FTicketRef>>();
	TMap<ERoute, TArray<TSharedRef<FHeld>>> HoldQueue;
	TMap<ERoute, TArray<FFlockHttpResponse>> AnswerQueue;
	int32 IdsMade = 0;
	int32 OrdersMade = 0;

	int32 SizeOf(const FString& QueueId) const
	{
		for (const FQueue& Queue : Queues)
		{
			if (QueueId.Equals(Queue.Id, ESearchCase::CaseSensitive))
			{
				return Queue.Size;
			}
		}
		return 0;
	}

	void StopSearch(FTicket& Stopped, const FString& Reason)
	{
		for (const FTicketRef& Ticket : *Tickets)
		{
			const bool bSame = &Ticket.Get() == &Stopped;
			const bool bSameParty = !Stopped.PartyId.IsEmpty() && Ticket->PartyId.Equals(Stopped.PartyId, ESearchCase::CaseSensitive);
			if (Ticket->IsQueued() && (bSame || bSameParty))
			{
				Ticket->Status = TEXT("cancelled");
				Ticket->CancelReason = Reason;
			}
		}
	}

	TArray<FTicketRef> MakeTickets(const FString& QueueId, const TArray<FString>& PlayerIds, const FString& PartyId, const FString& GameVersion)
	{
		const int32 Order = ++OrdersMade;
		TArray<FTicketRef> Made;
		for (const FString& PlayerId : PlayerIds)
		{
			const FTicketRef Ticket = MakeShared<FTicket>();
			Ticket->Id = FString::Printf(TEXT("01TICKET%018d"), ++IdsMade);
			Ticket->QueueId = QueueId;
			Ticket->PlayerId = PlayerId;
			Ticket->PartyId = PartyId;
			Ticket->PartySize = PlayerIds.Num();
			Ticket->GameVersion = GameVersion;
			Ticket->Order = Order;
			Made.Add(Ticket);
		}
		Tickets->Append(Made);
		return Made;
	}

	static FString VersionOf(const FFlockHttpRequest& Request)
	{
		const FString* Version = Request.Headers.Find(TEXT("X-Game-Version-ID"));
		return Version != nullptr ? Version->TrimStartAndEnd() : FString();
	}

	FFlockHttpResponse Serve(ERoute Route, const FString& Path, const FFlockHttpRequest& Request)
	{
		if (Route == ERoute::Other)
		{
			return Refused(404, TEXT("request.not_found"));
		}
		if (Route == ERoute::Queues)
		{
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FQueue& Queue : Queues)
			{
				const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("id"), Queue.Id);
				Row->SetStringField(TEXT("name"), Queue.Name);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			return Enveloped(MakeShared<FJsonValueArray>(Rows));
		}
		const FString Player = FFlockFakePartyServer::PlayerOf(Request);
		if (Player.IsEmpty())
		{
			return Refused(401, TEXT("player.missing_token"));
		}

		if (Route == ERoute::Create)
		{
			TSharedPtr<FJsonObject> Body;
			FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Request.JsonBody), Body);
			FString QueueId;
			const int32 Size = Body.IsValid() && Body->TryGetStringField(TEXT("queue_id"), QueueId) ? SizeOf(QueueId) : 0;
			if (Size == 0)
			{
				return Refused(404, TEXT("matchmaking.queue_not_found"));
			}
			TArray<FString> PlayerIds{ Player };
			FString PartyId;
			if (Body->HasField(TEXT("party_id")))
			{
				Body->TryGetStringField(TEXT("party_id"), PartyId);
				const FFlockFakePartyServer::FParty* Party = Parties->PartyById(PartyId);
				if (Party == nullptr || !Party->bActive)
				{
					return Refused(404, TEXT("party.not_found"));
				}
				if (!Party->LeaderPlayerId.Equals(Player, ESearchCase::CaseSensitive))
				{
					return Refused(403, TEXT("matchmaking.not_party_leader"));
				}
				PlayerIds = Party->Members;
				PartyId = Party->Id;
			}
			if (PlayerIds.Num() > Size)
			{
				return Refused(400, TEXT("matchmaking.party_too_large"));
			}
			for (const FString& PlayerId : PlayerIds)
			{
				if (QueuedTicketOf(PlayerId).IsValid())
				{
					return Refused(409, TEXT("matchmaking.already_queued"));
				}
			}
			TArray<TSharedPtr<FJsonValue>> Made;
			for (const FTicketRef& Ticket : MakeTickets(QueueId, PlayerIds, PartyId, VersionOf(Request)))
			{
				Made.Add(MakeShared<FJsonValueObject>(Describe(*Ticket, false)));
			}
			return Enveloped(MakeShared<FJsonValueArray>(Made));
		}
		if (Route == ERoute::Current)
		{
			TSharedPtr<FTicket> Mine = QueuedTicketOf(Player);
			if (!Mine.IsValid())
			{
				const TArray<FTicketRef> Theirs = TicketsOf(Player);
				Mine = Theirs.Num() > 0 ? TSharedPtr<FTicket>(Theirs.Last()) : TSharedPtr<FTicket>();
			}
			return Mine.IsValid() ? Enveloped(MakeShared<FJsonValueObject>(Describe(*Mine, true)))
				: Refused(404, TEXT("matchmaking.ticket_not_found"));
		}

		const FString TicketId = Path.RightChop(FCString::Strlen(TEXT("matchmaking/ticket/")));
		const FTicketRef* Own = Tickets->FindByPredicate([&](const FTicketRef& Ticket)
		{
			return Ticket->Id.Equals(TicketId, ESearchCase::CaseSensitive) && Ticket->PlayerId.Equals(Player, ESearchCase::CaseSensitive);
		});
		if (Own == nullptr)
		{
			return Refused(404, TEXT("matchmaking.ticket_not_found"));
		}
		if (Route == ERoute::ReadTicket)
		{
			return Enveloped(MakeShared<FJsonValueObject>(Describe(**Own, true)));
		}
		if (!(*Own)->IsQueued())
		{
			return Refused(409, TEXT("matchmaking.ticket_not_cancelable"));
		}
		StopSearch(**Own, TEXT("player"));
		return Enveloped(MakeShared<FJsonValueObject>(Describe(**Own, false)));
	}

	static void SetOrNull(const TSharedRef<FJsonObject>& Object, const TCHAR* Field, const FString& Value)
	{
		if (Value.IsEmpty())
		{
			Object->SetField(Field, MakeShared<FJsonValueNull>());
		}
		else
		{
			Object->SetStringField(Field, Value);
		}
	}

	/** A ticket as the routes answer it; the reads embed the match once there is one, which names its session. */
	static TSharedRef<FJsonObject> Describe(const FTicket& Ticket, bool bWithMatch)
	{
		const TSharedRef<FJsonObject> Described = MakeShared<FJsonObject>();
		Described->SetStringField(TEXT("id"), Ticket.Id);
		Described->SetStringField(TEXT("queue_id"), Ticket.QueueId);
		Described->SetStringField(TEXT("game_id"), TEXT("01KVCSANMD9V0CRW85ADE1WJA1"));
		Described->SetStringField(TEXT("player_id"), Ticket.PlayerId);
		Described->SetStringField(TEXT("status"), Ticket.Status);
		Described->SetObjectField(TEXT("attributes"), MakeShared<FJsonObject>());
		SetOrNull(Described, TEXT("party_id"), Ticket.PartyId);
		Described->SetNumberField(TEXT("party_size"), Ticket.PartySize);
		SetOrNull(Described, TEXT("match_id"), Ticket.Match.IsValid() ? Ticket.Match->Id : FString());
		SetOrNull(Described, TEXT("matched_at"), Ticket.Match.IsValid() ? FString(TEXT("2026-10-08T09:18:31.123456")) : FString());
		SetOrNull(Described, TEXT("cancel_reason"), Ticket.CancelReason);
		SetOrNull(Described, TEXT("game_version_id"), Ticket.GameVersion);
		Described->SetStringField(TEXT("created_at"), TEXT("2026-10-08T09:18:29.654321"));
		if (bWithMatch)
		{
			if (!Ticket.Match.IsValid())
			{
				Described->SetField(TEXT("match"), MakeShared<FJsonValueNull>());
			}
			else
			{
				const TSharedRef<FJsonObject> Match = MakeShared<FJsonObject>();
				Match->SetStringField(TEXT("id"), Ticket.Match->Id);
				Match->SetStringField(TEXT("queue_id"), Ticket.Match->QueueId);
				Match->SetStringField(TEXT("game_id"), TEXT("01KVCSANMD9V0CRW85ADE1WJA1"));
				Match->SetStringField(TEXT("status"), TEXT("ready"));
				TArray<TSharedPtr<FJsonValue>> Players;
				for (const FString& PlayerId : Ticket.Match->PlayerIds)
				{
					Players.Add(MakeShared<FJsonValueString>(PlayerId));
				}
				Match->SetArrayField(TEXT("player_ids"), Players);
				const TSharedRef<FJsonObject> Connection = MakeShared<FJsonObject>();
				if (!Ticket.Match->SessionId.IsEmpty())
				{
					Connection->SetStringField(TEXT("mode"), TEXT("p2p"));
					Connection->SetStringField(TEXT("session_id"), Ticket.Match->SessionId);
				}
				Match->SetObjectField(TEXT("connection_info"), Connection);
				Match->SetStringField(TEXT("created_at"), TEXT("2026-10-08T09:18:31.123456"));
				Described->SetObjectField(TEXT("match"), Match);
			}
		}
		return Described;
	}

	static FFlockHttpResponse Enveloped(const TSharedRef<FJsonValue>& Result)
	{
		const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetField(TEXT("code"), MakeShared<FJsonValueNull>());
		const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetField(TEXT("message"), MakeShared<FJsonValueNull>());
		Response->SetField(TEXT("code"), MakeShared<FJsonValueNull>());
		Body->SetObjectField(TEXT("error"), Error);
		Body->SetObjectField(TEXT("response"), Response);
		Body->SetField(TEXT("result"), Result);
		return FFlockFakePartyServer::Answer(200, Body);
	}
};
