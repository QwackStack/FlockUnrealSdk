// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Http/FlockHttpAdapter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tests/Support/FlockFakePartyServer.h"

/**
 * The multiplayer session routes as the backend answers them, measured against it
 * (Libraries/Unreal/multiplayer-sessions/probe_session_rules.py and probe_backend_fixes.py): every route answers the whole
 * session with the heartbeat interval and every seat in seniority order (a player seated again keeps one row, now the least
 * senior), each seat carrying its own connection fields; a player holds one seat a game, so hosting or joining gives the old
 * one up; the host leaving hands hosting to the longest seated or closes the session as host_left; a change of host clears
 * the address, reopens the session and moves host_epoch but not connection_epoch; a publish always moves connection_epoch; a
 * session keeps to its host's Game Version unless told otherwise; the address is blanked for a reader without a seat; a
 * player who never sat in a session reads 404 session_not_found, and so do the host-only and token routes for a player
 * without a seat; only leave answers not_a_participant. The requester is the JWT's subject. Include only from guarded test files.
 */
class FFlockFakeSessionServer : public IFlockHttpAdapter
{
public:
	enum class ERoute : uint8 { Host, Join, Current, Heartbeat, Read, Leave, End, MakeHost, Publish, JoinToken, Verify, RelayCredentials, Other };

	struct FSeat
	{
		FString PlayerId;
		FString Status = TEXT("joined");
	};

	struct FSession
	{
		FString Id;
		FString Code;
		FString Status = TEXT("open");
		FString EndedReason;
		FString HostId;
		int32 HostEpoch = 1;
		int32 MaxPlayers = 8;
		TSharedRef<FJsonObject> Connection = MakeShared<FJsonObject>();
		int32 ConnectionEpoch = 0;
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		FString GameVersion = GameBuild;
		bool bSameVersionOnly = true;
		TArray<FSeat> Seats;

		bool IsLive() const { return Status != TEXT("ended"); }

		bool IsSeated(const FString& PlayerId) const
		{
			return Seats.ContainsByPredicate([&](const FSeat& Seat)
			{
				return Seat.PlayerId.Equals(PlayerId, ESearchCase::CaseSensitive) && Seat.Status == TEXT("joined");
			});
		}

		bool EverSeated(const FString& PlayerId) const
		{
			return Seats.ContainsByPredicate([&](const FSeat& Seat) { return Seat.PlayerId.Equals(PlayerId, ESearchCase::CaseSensitive); });
		}

		TArray<FString> Seated() const
		{
			TArray<FString> Players;
			for (const FSeat& Seat : Seats)
			{
				if (Seat.Status == TEXT("joined"))
				{
					Players.Add(Seat.PlayerId);
				}
			}
			return Players;
		}
	};
	using FSessionRef = TSharedRef<FSession>;

	/** The Game Version the test client sends; other players are on it unless a test says otherwise. */
	static constexpr const TCHAR* GameBuild = TEXT("ver-1");

	/** What every session answer says about heartbeats; unset leaves the interval out, as a server from before it did. */
	TOptional<int32> HeartbeatSeconds = 20;

	/**
	 * The STUN servers the relay route lists, answered as the backend does with no session (measured: one entry of "stun:" urls,
	 * no login, ttl 600, relay_paused false; Libraries/Unreal/multiplayer-connecting/wire/relay.no_session.json).
	 */
	TArray<FString> StunUrls;

	/**
	 * The relay servers a seat's logins list, as the backend answers a session_id the player is seated in (measured 2026-10-17:
	 * one entry holding the server's udp and tcp urls and a login "<expiry>:<game>:<player>", then the STUN entry;
	 * Libraries/Unreal/multiplayer-relay/results/relay-rules.txt). Empty lists STUN alone, as for a game with the relay off.
	 */
	TArray<FString> RelayUrls;
	/** The password every relay login carries; the fake relay checks the same one. */
	FString RelayPassword = TEXT("minted-for-the-test");
	/** The studio's relay is paused: STUN alone, relay_paused true. */
	bool bRelayPaused = false;

	TArray<FSessionRef> Sessions;
	TArray<FFlockHttpRequest> Requests;

	/** Holds the next request of Route; it is answered (as when it arrived) by Release, or as cancelled by a cancel. */
	TSharedRef<FFlockFakePartyServer::FHeld> HoldNext(ERoute Route)
	{
		const TSharedRef<FFlockFakePartyServer::FHeld> Held = MakeShared<FFlockFakePartyServer::FHeld>();
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

	/** The player each request of Route went out as, in order. */
	TArray<FString> PlayersOf(ERoute Route) const
	{
		TArray<FString> Players;
		for (const FFlockHttpRequest& Request : Requests)
		{
			if (RouteOf(Request) == Route)
			{
				Players.Add(FFlockFakePartyServer::PlayerOf(Request));
			}
		}
		return Players;
	}

	/** The requests of Route, in order. */
	TArray<FFlockHttpRequest> RequestsTo(ERoute Route) const
	{
		return Requests.FilterByPredicate([&](const FFlockHttpRequest& Request) { return RouteOf(Request) == Route; });
	}

	TSharedPtr<FSession> ById(const FString& SessionId) const
	{
		const FSessionRef* Found = Sessions.FindByPredicate([&](const FSessionRef& Session) { return Session->Id.Equals(SessionId, ESearchCase::CaseSensitive); });
		return Found != nullptr ? TSharedPtr<FSession>(*Found) : TSharedPtr<FSession>();
	}

	// ---- what other players (or this player on another device, or the server's own sweep) do ----

	FSessionRef HostedBy(const FString& PlayerId, int32 MaxPlayers = 8)
	{
		ReleaseSeatsOf(PlayerId);
		++SessionsMade;
		const FSessionRef Session = MakeShared<FSession>();
		Session->Id = FString::Printf(TEXT("01SESSION%017d"), SessionsMade);
		Session->Code = FString::Printf(TEXT("S%05d"), SessionsMade + 10000);
		Session->HostId = PlayerId;
		Session->MaxPlayers = MaxPlayers;
		Session->Seats.Add(FSeat{ PlayerId });
		Sessions.Add(Session);
		return Session;
	}

	/** The session a match makes: every matched player seated in order, the first hosting, and each one's other seat given up. */
	FSessionRef SeatedByMatch(const TArray<FString>& PlayerIds, const FString& GameVersion = GameBuild)
	{
		for (const FString& PlayerId : PlayerIds)
		{
			ReleaseSeatsOf(PlayerId);
		}
		++SessionsMade;
		const FSessionRef Session = MakeShared<FSession>();
		Session->Id = FString::Printf(TEXT("01SESSION%017d"), SessionsMade);
		Session->Code = FString::Printf(TEXT("S%05d"), SessionsMade + 10000);
		Session->HostId = PlayerIds.Num() > 0 ? PlayerIds[0] : FString();
		Session->MaxPlayers = FMath::Max(8, PlayerIds.Num());
		Session->GameVersion = GameVersion;
		for (const FString& PlayerId : PlayerIds)
		{
			Session->Seats.Add(FSeat{ PlayerId });
		}
		Sessions.Add(Session);
		return Session;
	}

	/** A player seated again keeps one row, now the least senior, as the backend resets the old row. */
	void Joins(const FSessionRef& Session, const FString& PlayerId)
	{
		ReleaseSeatsOf(PlayerId);
		Session->Seats.RemoveAll([&](const FSeat& Seat) { return Seat.PlayerId.Equals(PlayerId, ESearchCase::CaseSensitive); });
		Session->Seats.Add(FSeat{ PlayerId });
	}

	void Leaves(const FSessionRef& Session, const FString& PlayerId) { GivesUp(*Session, PlayerId, TEXT("left")); }

	/** The server's sweep after it stopped hearing from the player. */
	void Drops(const FSessionRef& Session, const FString& PlayerId) { GivesUp(*Session, PlayerId, TEXT("dropped")); }

	void Ends(const FSessionRef& Session, const FString& Reason = TEXT("ended_by_host")) { Close(*Session, Reason); }

	void Publishes(const FSessionRef& Session, const TSharedRef<FJsonObject>& Connection)
	{
		Session->Connection = Connection;
		++Session->ConnectionEpoch;
		Session->Status = TEXT("active");
	}

	void MakesHost(const FSessionRef& Session, const FString& PlayerId) { HandOver(*Session, PlayerId); }

	/**
	 * A join token for PlayerId in Session, as the server mints one for a seated player. Each is kept as minted, as the backend's
	 * signature binds it to its session and player, so another session's token and an expired one are told apart from a good one.
	 */
	FString MintsJoinToken(const FSessionRef& Session, const FString& PlayerId)
	{
		const FString Token = FString::Printf(TEXT("token-%s-%s-%d"), *Session->Id, *PlayerId, JoinTokens.Num() + 1);
		JoinTokens.Add(Token, FMintedJoinToken{ Session->Id, PlayerId, false });
		return Token;
	}

	/** Every token minted so far is past its 60 s. */
	void ExpireJoinTokens()
	{
		for (TPair<FString, FMintedJoinToken>& Minted : JoinTokens)
		{
			Minted.Value.bExpired = true;
		}
	}

	static FFlockHttpResponse Refused(int32 Status, const FString& Code) { return FFlockFakePartyServer::Refused(Status, Code); }

	static ERoute RouteOf(const FFlockHttpRequest& Request)
	{
		FString Path;
		if (!Request.Url.Split(TEXT("/v1/"), nullptr, &Path))
		{
			return ERoute::Other;
		}
		Path.Split(TEXT("?"), &Path, nullptr);
		if (Path == TEXT("multiplayer/relay-credentials"))
		{
			return Request.Method == TEXT("POST") ? ERoute::RelayCredentials : ERoute::Other;
		}
		const FString Prefix = TEXT("multiplayer/sessions");
		if (!Path.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			return ERoute::Other;
		}
		if (Path == Prefix)
		{
			return Request.Method == TEXT("POST") ? ERoute::Host : ERoute::Other;
		}
		if (Path == Prefix + TEXT("/join"))
		{
			return ERoute::Join;
		}
		if (Path == Prefix + TEXT("/current"))
		{
			return ERoute::Current;
		}
		const TCHAR* Endings[] = { TEXT("/heartbeat"), TEXT("/leave"), TEXT("/end"), TEXT("/host"), TEXT("/connection-info"),
			TEXT("/join-token"), TEXT("/verify-join-token") };
		const ERoute Routes[] = { ERoute::Heartbeat, ERoute::Leave, ERoute::End, ERoute::MakeHost, ERoute::Publish, ERoute::JoinToken,
			ERoute::Verify };
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Endings); ++Index)
		{
			if (Path.EndsWith(Endings[Index], ESearchCase::CaseSensitive))
			{
				return Routes[Index];
			}
		}
		return Request.Method == TEXT("GET") ? ERoute::Read : ERoute::Other;
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

		TArray<TSharedRef<FFlockFakePartyServer::FHeld>>* Holds = HoldQueue.Find(Route);
		if (Holds != nullptr && Holds->Num() > 0)
		{
			const TSharedRef<FFlockFakePartyServer::FHeld> Held = (*Holds)[0];
			Holds->RemoveAt(0);
			Held->bArrived = true;
			Held->OnComplete = OnComplete;
			Held->Response = Response;
			// A cancel answers a held request at once, as the engine's HTTP module does.
			const TSharedRef<FFlockCancelToken> Token = MakeShared<FFlockCancelToken>();
			const TWeakPtr<FFlockFakePartyServer::FHeld> WeakHeld = Held;
			Token->Canceller = [WeakHeld]()
			{
				const TSharedPtr<FFlockFakePartyServer::FHeld> Pinned = WeakHeld.Pin();
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
	TMap<ERoute, TArray<TSharedRef<FFlockFakePartyServer::FHeld>>> HoldQueue;
	TMap<ERoute, TArray<FFlockHttpResponse>> AnswerQueue;
	int32 SessionsMade = 0;

	struct FMintedJoinToken
	{
		FString SessionId;
		FString PlayerId;
		bool bExpired = false;
	};
	TMap<FString, FMintedJoinToken> JoinTokens;

	/** Every change of host: the old host's address is cleared and the session is open again until the new host publishes. */
	static void HandOver(FSession& Session, const FString& PlayerId)
	{
		Session.HostId = PlayerId;
		++Session.HostEpoch;
		Session.Connection = MakeShared<FJsonObject>();
		Session.Status = TEXT("open");
	}

	static void Close(FSession& Session, const FString& Reason)
	{
		Session.Status = TEXT("ended");
		Session.EndedReason = Reason;
		for (FSeat& Seat : Session.Seats)
		{
			if (Seat.Status == TEXT("joined"))
			{
				Seat.Status = TEXT("left");
			}
		}
	}

	/** A seat given up: the host's is handed to the longest seated of the others, and a session nobody is left in closes. */
	static void GivesUp(FSession& Session, const FString& PlayerId, const FString& How)
	{
		FSeat* Seat = Session.Seats.FindByPredicate([&](const FSeat& Each)
		{
			return Each.PlayerId.Equals(PlayerId, ESearchCase::CaseSensitive) && Each.Status == TEXT("joined");
		});
		if (Seat == nullptr)
		{
			return;
		}
		Seat->Status = How;
		const TArray<FString> Left = Session.Seated();
		if (Session.HostId.Equals(PlayerId, ESearchCase::CaseSensitive))
		{
			if (Left.Num() == 0)
			{
				Close(Session, TEXT("host_left"));
				return;
			}
			HandOver(Session, Left[0]);
		}
		else if (Left.Num() == 0)
		{
			Close(Session, TEXT("empty"));
		}
	}

	void ReleaseSeatsOf(const FString& PlayerId)
	{
		for (const FSessionRef& Session : Sessions)
		{
			if (Session->IsLive() && Session->IsSeated(PlayerId))
			{
				GivesUp(*Session, PlayerId, TEXT("left"));
			}
		}
	}

	static FFlockHttpResponse Enveloped(const TSharedRef<FJsonObject>& Result)
	{
		const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetField(TEXT("code"), MakeShared<FJsonValueNull>());
		const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetField(TEXT("message"), MakeShared<FJsonValueNull>());
		Response->SetField(TEXT("code"), MakeShared<FJsonValueNull>());
		Body->SetObjectField(TEXT("error"), Error);
		Body->SetObjectField(TEXT("response"), Response);
		Body->SetObjectField(TEXT("result"), Result);
		return FFlockFakePartyServer::Answer(200, Body);
	}

	/** Every route answers the whole session; the address is blanked for a reader without a seat, as the backend does. */
	TSharedRef<FJsonObject> Describe(const FSession& Session, const FString& Reader) const
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("id"), Session.Id);
		Object->SetStringField(TEXT("game_id"), TEXT("01KVCSANMD9V0CRW85ADE1WJA1"));
		Object->SetField(TEXT("match_id"), MakeShared<FJsonValueNull>());
		Object->SetStringField(TEXT("status"), Session.Status);
		if (Session.EndedReason.IsEmpty())
		{
			Object->SetField(TEXT("ended_reason"), MakeShared<FJsonValueNull>());
		}
		else
		{
			Object->SetStringField(TEXT("ended_reason"), Session.EndedReason);
		}
		Object->SetStringField(TEXT("host_player_id"), Session.HostId);
		Object->SetNumberField(TEXT("host_epoch"), Session.HostEpoch);
		Object->SetStringField(TEXT("join_code"), Session.Code);
		Object->SetNumberField(TEXT("max_players"), Session.MaxPlayers);
		Object->SetObjectField(TEXT("connection_info"), Session.IsSeated(Reader) ? Session.Connection : MakeShared<FJsonObject>());
		Object->SetNumberField(TEXT("connection_epoch"), Session.ConnectionEpoch);
		Object->SetObjectField(TEXT("data"), Session.Data);
		Object->SetStringField(TEXT("game_version_id"), Session.GameVersion);
		Object->SetBoolField(TEXT("same_version_only"), Session.bSameVersionOnly);
		Object->SetNumberField(TEXT("participant_timeout_seconds"), 90);
		Object->SetNumberField(TEXT("host_timeout_seconds"), 60);
		if (HeartbeatSeconds.IsSet())
		{
			Object->SetNumberField(TEXT("heartbeat_interval_seconds"), HeartbeatSeconds.GetValue());
		}
		Object->SetStringField(TEXT("last_host_seen_at"), TEXT("2026-10-13T16:02:43.754626Z"));
		Object->SetStringField(TEXT("expires_at"), TEXT("2026-10-13T20:02:43.754626Z"));
		Object->SetField(TEXT("ended_at"), MakeShared<FJsonValueNull>());
		Object->SetStringField(TEXT("created_at"), TEXT("2026-10-13T16:02:43.763750Z"));
		Object->SetStringField(TEXT("updated_at"), TEXT("2026-10-13T16:02:43.763750Z"));
		TArray<TSharedPtr<FJsonValue>> Participants;
		for (const FSeat& Seat : Session.Seats)
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("player_id"), Seat.PlayerId);
			Row->SetStringField(TEXT("status"), Seat.Status);
			Row->SetStringField(TEXT("joined_at"), TEXT("2026-10-13T16:02:43.754626Z"));
			Row->SetField(TEXT("left_at"), MakeShared<FJsonValueNull>());
			Row->SetStringField(TEXT("last_seen_at"), TEXT("2026-10-13T16:02:43.754626Z"));
			Row->SetNumberField(TEXT("relay_credential_mints"), 0);
			Row->SetObjectField(TEXT("connection_info"), MakeShared<FJsonObject>());
			Row->SetNumberField(TEXT("connection_epoch"), 0);
			Participants.Add(MakeShared<FJsonValueObject>(Row));
		}
		Object->SetArrayField(TEXT("participants"), Participants);
		return Object;
	}

	static FString VersionOf(const FFlockHttpRequest& Request)
	{
		const FString* Version = Request.Headers.Find(TEXT("X-Game-Version-ID"));
		return Version != nullptr ? Version->TrimStartAndEnd() : FString();
	}

	FFlockHttpResponse Serve(ERoute Route, const FFlockHttpRequest& Request)
	{
		if (Route == ERoute::Other)
		{
			return Refused(404, TEXT("request.not_found"));
		}
		const FString Player = FFlockFakePartyServer::PlayerOf(Request);
		if (Player.IsEmpty())
		{
			return Refused(401, TEXT("player.missing_token"));
		}
		TSharedPtr<FJsonObject> Body;
		if (Request.bHasBody)
		{
			FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Request.JsonBody), Body);
		}
		if (Route == ERoute::RelayCredentials)
		{
			FString Named;
			bool bSeated = false;
			if (Body.IsValid() && Body->TryGetStringField(TEXT("session_id"), Named) && !Named.IsEmpty())
			{
				const TSharedPtr<FSession> Seat = ById(Named);
				if (!Seat.IsValid() || !Seat->IsLive() || !Seat->IsSeated(Player))
				{
					return Refused(404, TEXT("multiplayer.session_not_found"));
				}
				bSeated = true;
			}
			auto UrlsOf = [](const TArray<FString>& Listed)
			{
				TArray<TSharedPtr<FJsonValue>> Urls;
				for (const FString& Url : Listed)
				{
					Urls.Add(MakeShared<FJsonValueString>(Url));
				}
				return Urls;
			};
			TArray<TSharedPtr<FJsonValue>> Servers;
			// The relay is for players in a game: only a seat draws it, and a paused studio gets STUN alone.
			if (bSeated && !bRelayPaused && RelayUrls.Num() > 0)
			{
				const TSharedRef<FJsonObject> Relay = MakeShared<FJsonObject>();
				Relay->SetArrayField(TEXT("urls"), UrlsOf(RelayUrls));
				Relay->SetStringField(TEXT("username"), TEXT("1760000000:game:") + Player);
				Relay->SetStringField(TEXT("credential"), RelayPassword);
				Servers.Add(MakeShared<FJsonValueObject>(Relay));
			}
			const TSharedRef<FJsonObject> Stun = MakeShared<FJsonObject>();
			Stun->SetArrayField(TEXT("urls"), UrlsOf(StunUrls));
			Stun->SetField(TEXT("username"), MakeShared<FJsonValueNull>());
			Stun->SetField(TEXT("credential"), MakeShared<FJsonValueNull>());
			Servers.Add(MakeShared<FJsonValueObject>(Stun));
			const TSharedRef<FJsonObject> Credentials = MakeShared<FJsonObject>();
			Credentials->SetNumberField(TEXT("ttl"), 600);
			Credentials->SetArrayField(TEXT("ice_servers"), Servers);
			// Said with or without a session, as the backend's route does (a studio past due is paused for every call).
			Credentials->SetBoolField(TEXT("relay_paused"), bRelayPaused);
			return Enveloped(Credentials);
		}
		FString SessionId;
		{
			FString Path;
			Request.Url.Split(TEXT("/v1/multiplayer/sessions/"), nullptr, &Path);
			Path.Split(TEXT("/"), &SessionId, nullptr);
			if (SessionId.IsEmpty())
			{
				SessionId = Path;
			}
		}
		const TSharedPtr<FSession> Session = ById(SessionId);
		const bool bLive = Session.IsValid() && Session->IsLive();
		const bool bSeated = Session.IsValid() && Session->IsSeated(Player);

		switch (Route)
		{
		case ERoute::Host:
		{
			int32 Size = 8;
			if (Body.IsValid() && Body->HasField(TEXT("max_players")))
			{
				if (!Body->TryGetNumberField(TEXT("max_players"), Size) || Size < 2 || Size > 16)
				{
					return Refused(422, TEXT("request.validation_failed"));
				}
			}
			const FSessionRef Made = HostedBy(Player, Size);
			const TSharedPtr<FJsonObject>* Data = nullptr;
			if (Body.IsValid() && Body->TryGetObjectField(TEXT("data"), Data) && Data != nullptr)
			{
				Made->Data = (*Data).ToSharedRef();
			}
			Made->GameVersion = VersionOf(Request);
			bool bSameVersionOnly = true;
			if (Body.IsValid())
			{
				Body->TryGetBoolField(TEXT("same_version_only"), bSameVersionOnly);
			}
			Made->bSameVersionOnly = bSameVersionOnly;
			return Enveloped(Describe(*Made, Player));
		}
		case ERoute::Current:
		{
			const FSessionRef* Mine = Sessions.FindByPredicate([&](const FSessionRef& Each) { return Each->IsLive() && Each->IsSeated(Player); });
			return Mine == nullptr ? Refused(404, TEXT("multiplayer.session_not_found")) : Enveloped(Describe(**Mine, Player));
		}
		case ERoute::Heartbeat:
			if (!bLive || !bSeated)
			{
				return Refused(404, TEXT("multiplayer.session_not_found"));
			}
			return Enveloped(Describe(*Session, Player));
		case ERoute::Join:
		{
			FString Code;
			if (Body.IsValid())
			{
				Body->TryGetStringField(TEXT("join_code"), Code);
			}
			Code = Code.TrimStartAndEnd().ToUpper();
			const FSessionRef* Joining = Sessions.FindByPredicate([&](const FSessionRef& Each) { return Each->IsLive() && Each->Code == Code; });
			if (Joining == nullptr)
			{
				return Refused(404, TEXT("multiplayer.invalid_join_code"));
			}
			const FSessionRef Target = *Joining;
			if (Target->IsSeated(Player))
			{
				return Enveloped(Describe(*Target, Player));
			}
			if (Target->bSameVersionOnly && !Target->GameVersion.Equals(VersionOf(Request), ESearchCase::CaseSensitive))
			{
				return Refused(409, TEXT("multiplayer.version_mismatch"));
			}
			if (Target->Seated().Num() >= Target->MaxPlayers)
			{
				return Refused(409, TEXT("multiplayer.session_full"));
			}
			Joins(Target, Player);
			return Enveloped(Describe(*Target, Player));
		}
		case ERoute::Read:
			if (!Session.IsValid() || !Session->EverSeated(Player))
			{
				return Refused(404, TEXT("multiplayer.session_not_found"));
			}
			return Enveloped(Describe(*Session, Player));
		case ERoute::Leave:
			if (!bLive)
			{
				return Refused(404, TEXT("multiplayer.session_not_found"));
			}
			if (!bSeated)
			{
				return Refused(404, TEXT("multiplayer.not_a_participant"));
			}
			GivesUp(*Session, Player, TEXT("left"));
			return Enveloped(Describe(*Session, Player));
		default:
			break;
		}

		// The host-only and token routes: a player without a seat is told the session is not found.
		if (!bLive || !bSeated)
		{
			return Refused(404, TEXT("multiplayer.session_not_found"));
		}
		if (Route == ERoute::JoinToken)
		{
			const TSharedRef<FJsonObject> Token = MakeShared<FJsonObject>();
			Token->SetStringField(TEXT("token"), MintsJoinToken(Session.ToSharedRef(), Player));
			Token->SetNumberField(TEXT("expires_in"), 60);
			return Enveloped(Token);
		}
		if (!Session->HostId.Equals(Player, ESearchCase::CaseSensitive))
		{
			return Refused(403, TEXT("multiplayer.not_host"));
		}
		switch (Route)
		{
		case ERoute::End:
			Close(*Session, TEXT("ended_by_host"));
			return Enveloped(Describe(*Session, Player));
		case ERoute::MakeHost:
		{
			FString Target;
			if (Body.IsValid())
			{
				Body->TryGetStringField(TEXT("player_id"), Target);
			}
			if (Target.Equals(Player, ESearchCase::CaseSensitive))
			{
				return Enveloped(Describe(*Session, Player));
			}
			if (!Session->IsSeated(Target))
			{
				return Refused(404, TEXT("multiplayer.target_not_a_participant"));
			}
			HandOver(*Session, Target);
			return Enveloped(Describe(*Session, Player));
		}
		case ERoute::Publish:
		{
			const TSharedPtr<FJsonObject>* Connection = nullptr;
			FString Mode;
			if (!Body.IsValid() || !Body->TryGetObjectField(TEXT("connection_info"), Connection) || Connection == nullptr
				|| !(*Connection)->TryGetStringField(TEXT("mode"), Mode) || Mode.TrimStartAndEnd().IsEmpty() || Mode.Len() > 32)
			{
				return Refused(422, TEXT("request.validation_failed"));
			}
			Publishes(Session.ToSharedRef(), (*Connection).ToSharedRef());
			return Enveloped(Describe(*Session, Player));
		}
		case ERoute::Verify:
		{
			FString Token;
			if (Body.IsValid())
			{
				Body->TryGetStringField(TEXT("token"), Token);
			}
			if (Token.IsEmpty())
			{
				return Refused(422, TEXT("request.validation_failed"));
			}
			// The backend's one answer for a made-up token, another session's, an expired one and one whose player gave up their seat.
			const FMintedJoinToken* Minted = JoinTokens.Find(Token);
			if (Minted == nullptr || !Minted->SessionId.Equals(Session->Id, ESearchCase::CaseSensitive) || Minted->bExpired
				|| !Session->IsSeated(Minted->PlayerId))
			{
				return Refused(400, TEXT("multiplayer.invalid_join_token"));
			}
			const TSharedRef<FJsonObject> Verified = MakeShared<FJsonObject>();
			Verified->SetStringField(TEXT("session_id"), Session->Id);
			Verified->SetStringField(TEXT("player_id"), Minted->PlayerId);
			return Enveloped(Verified);
		}
		default:
			return Refused(404, TEXT("request.not_found"));
		}
	}
};
