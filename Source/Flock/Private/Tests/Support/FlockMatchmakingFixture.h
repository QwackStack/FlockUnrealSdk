// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "Config/FlockConfig.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "FlockInitConfig.h"
#include "FlockSubsystem.h"
#include "Http/FlockHttpClient.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Multiplayer/FlockMatchmaking.h"
#include "Multiplayer/FlockMultiplayerSession.h"
#include "Multiplayer/FlockMultiplayerSessions.h"
#include "Multiplayer/FlockParties.h"
#include "Multiplayer/FlockParty.h"
#include "Providers/FlockMultiplayerProvider.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/Support/FlockFakeMatchmakingServer.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockPartyTestListener.h"
#include "Tests/Support/FlockRecordingLogger.h"
#include "Tests/Support/FlockTestSdk.h"
#include "UObject/StrongObjectPtr.h"

// The multiplayer stack over the matchmaking fake (with its party and session fakes), shared by the matchmaking tests and the
// multiplayer Blueprint node tests. Include only from guarded test files.

namespace FlockMatchmakingTestHelpers
{
	using ERoute = FFlockFakeMatchmakingServer::ERoute;
	using FServer = FFlockFakeMatchmakingServer;

	const FString PlayerA = TEXT("player-a");
	const FString PlayerB = TEXT("player-b");
	const FString PlayerC = TEXT("player-c");

	/** A JWT naming PlayerId as its subject; the fake servers read the requester from it. */
	inline FString MatchmakingJwt(const FString& PlayerId)
	{
		FString Payload = FBase64::Encode(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId,
			FDateTime::UtcNow().ToUnixTimestamp() + 3600));
		Payload.ReplaceInline(TEXT("+"), TEXT("-"));
		Payload.ReplaceInline(TEXT("/"), TEXT("_"));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Payload);
	}

	/** What a FindMatch call answered, how often, and the handle it returned; bAnswered stays false while it waits. */
	struct FSearchAnswer
	{
		bool bAnswered = false;
		int32 Answers = 0;
		TFlockResult<FFlockMatchmakingResult> Result;
		FFlockRequestHandle Handle;

		bool Succeeded() const { return bAnswered && Result.bSuccess; }
		bool FailedWith(EFlockErrorType Type) const { return bAnswered && !Result.bSuccess && Result.Error.Type == Type; }
		bool WasCancelled() const { return FailedWith(EFlockErrorType::Cancelled); }
		const FFlockMatchmakingResult& Value() const { return Result.Value; }
	};
	using FSearchAnswerRef = TSharedRef<FSearchAnswer>;

	inline FFlockOnMatchmakingResult Into(const FSearchAnswerRef& Answer, TFunction<void()> AlsoOnAnswer = nullptr)
	{
		return [Answer, AlsoOnAnswer](TFlockResult<FFlockMatchmakingResult> Result)
		{
			++Answer->Answers;
			Answer->bAnswered = true;
			Answer->Result = Result;
			if (AlsoOnAnswer)
			{
				AlsoOnAnswer();
			}
		};
	}

	/** What a party call answered, and its handle. */
	struct FPartyCall
	{
		bool bAnswered = false;
		TFlockResult<UFlockParty*> Result;
		FFlockRequestHandle Handle;

		bool Succeeded() const { return bAnswered && Result.bSuccess; }
		UFlockParty* Party() const { return Result.Value; }
	};
	using FPartyCallRef = TSharedRef<FPartyCall>;

	inline FFlockOnParty Into(const FPartyCallRef& Call)
	{
		return [Call](TFlockResult<UFlockParty*> Result)
		{
			Call->bAnswered = true;
			Call->Result = Result;
		};
	}

	inline FString Joined(const TArray<FString>& Ids) { return FString::Join(Ids, TEXT(",")); }

	inline TSharedPtr<FJsonObject> BodyOf(const FFlockHttpRequest& Request)
	{
		TSharedPtr<FJsonObject> Body;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Request.JsonBody), Body);
		return Body;
	}

	inline TSharedPtr<FJsonObject> JsonOf(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Json), Object);
		return Object;
	}

	inline int32 Containing(const TArray<FString>& Lines, const FString& Needle)
	{
		return Lines.FilterByPredicate([&](const FString& Line) { return Line.Contains(Needle, ESearchCase::CaseSensitive); }).Num();
	}

	/** The multiplayer stack as the provider builds it, over the matchmaking fake (with its party and session fakes), with time moved by hand. */
	struct FMatchmakingFixture
	{
		TSharedRef<double> Now = MakeShared<double>(1000.0);
		/** The jitter's roll for every wait drawn from now on: 0 the shortest (three quarters), 1 the longest. */
		TSharedRef<double> Roll = MakeShared<double>(0.5);
		TSharedRef<FServer> Server = MakeShared<FServer>();
		TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockAuthSession> Session;
		TSharedRef<FFlockMultiplayerProvider> Provider;
		TArray<TStrongObjectPtr<UFlockPartyTestListener>> Listeners;

		static FFlockRetryPolicy Retries(int32 MaxRetries)
		{
			FFlockRetryPolicy Policy;
			Policy.MaxRetries = MaxRetries;
			Policy.InitialDelaySeconds = 0.f;
			Policy.bUseJitter = false;
			return Policy;
		}

		explicit FMatchmakingFixture(double PartyRefreshSeconds = 0.0, int32 MaxRetries = 0)
			: Client(MakeShared<FFlockHttpClient>(Server, Log))
			, Session(MakeShared<FFlockAuthSession>(Client, MakeShared<FFlockMemoryTokenStore>(), Log, TEXT("http://x/v1"),
				TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") }, { TEXT("X-Game-Version-ID"), FFlockFakeSessionServer::GameBuild } }))
			, Provider(MakeShared<FFlockMultiplayerProvider>(Client, Retries(MaxRetries), Log, Session, TEXT("http://x/v1"), PartyRefreshSeconds))
		{
			const TSharedRef<double> Clock = Now;
			const TSharedRef<double> Rolled = Roll;
			Provider->GetRepeatingCalls().SetClockForTesting([Clock]() { return *Clock; }, [Rolled]() { return *Rolled; });
		}

		~FMatchmakingFixture()
		{
			Provider->Shutdown();
		}

		void SignIn(const FString& PlayerId)
		{
			FString Error;
			Session->SetTokens(MatchmakingJwt(PlayerId), TEXT("r-1"), Error);
		}

		void SignOut() { Session->ClearTokens(); }

		/** One frame of the provider: a search, party or session of an ended sign-in ends, then the calls due are sent. */
		void Frame() { Provider->TickForTesting(); }

		/** Moves time on and runs a frame; a check or refresh due by then is sent, and answered unless held. */
		void Advance(double Seconds)
		{
			*Now += Seconds;
			Frame();
		}

		FFlockMatchmaking& Matchmaking() { return Provider->GetMatchmakingForTesting(); }
		UFlockMultiplayerSession* HeldSession() { return Provider->GetSessionsForTesting().GetHeld(); }
		UFlockParty* HeldParty() { return Provider->GetPartiesForTesting().GetHeld(); }

		FSearchAnswerRef Find(const FString& QueueName, const FFlockMatchmakingOptions& Options = FFlockMatchmakingOptions(),
			TFunction<void()> AlsoOnAnswer = nullptr)
		{
			const FSearchAnswerRef Answer = MakeShared<FSearchAnswer>();
			Answer->Handle = Provider->FindMatch(QueueName, Options, Into(Answer, MoveTemp(AlsoOnAnswer)));
			return Answer;
		}

		FSearchAnswerRef FindAsParty(UFlockParty* Party, const FString& QueueName, TFunction<void()> AlsoOnAnswer = nullptr)
		{
			const FSearchAnswerRef Answer = MakeShared<FSearchAnswer>();
			if (Party != nullptr)
			{
				Answer->Handle = Party->FindMatch(QueueName, FFlockMatchmakingOptions(), Into(Answer, MoveTemp(AlsoOnAnswer)));
			}
			return Answer;
		}

		FPartyCallRef GetMine()
		{
			const FPartyCallRef Call = MakeShared<FPartyCall>();
			Call->Handle = Provider->GetMyParty(Into(Call));
			return Call;
		}

		FPartyCallRef Join(const FString& Code)
		{
			const FPartyCallRef Call = MakeShared<FPartyCall>();
			Call->Handle = Provider->JoinParty(Code, Into(Call));
			return Call;
		}

		/** The player's party as GetMyParty answers it, or null with the test failed. */
		UFlockParty* MyParty(FAutomationTestBase& Test)
		{
			const FPartyCallRef Mine = GetMine();
			Test.TestTrue(TEXT("precondition: the party was read"), Mine->Succeeded() && Mine->Party() != nullptr);
			return Mine->Party();
		}

		UFlockPartyTestListener* Listen(UFlockParty* Party)
		{
			UFlockPartyTestListener* Listener = NewObject<UFlockPartyTestListener>();
			Listeners.Emplace(Listener);
			if (Party != nullptr)
			{
				Listener->Listen(Party);
			}
			return Listener;
		}

		/** A party another player made, with the given members joined after them, on the server directly. */
		FString PartyOf(const FString& Leader, const TArray<FString>& Others)
		{
			const FString PartyId = Server->Parties->MadeBy(Leader).Id;
			for (const FString& Other : Others)
			{
				Server->Parties->Joins(PartyId, Other);
			}
			return PartyId;
		}

		FFlockFakePartyServer::FParty* PartyById(const FString& PartyId) { return Server->Parties->PartyById(PartyId); }

		/** The player's latest ticket on the server. */
		TSharedPtr<FServer::FTicket> TicketOf(const FString& PlayerId) const
		{
			const TArray<FServer::FTicketRef> Tickets = Server->TicketsOf(PlayerId);
			return Tickets.Num() > 0 ? TSharedPtr<FServer::FTicket>(Tickets.Last()) : TSharedPtr<FServer::FTicket>();
		}

		FString StatusOf(const FString& PlayerId) const
		{
			const TSharedPtr<FServer::FTicket> Ticket = TicketOf(PlayerId);
			return Ticket.IsValid() ? Ticket->Status : FString(TEXT("(no ticket)"));
		}
	};

	/** The real subsystem, initialized over the matchmaking fake and signed in as PlayerA, with analytics off so only multiplayer talks to it. */
	struct FSubsystemFixture
	{
		TSharedRef<FServer> Server = MakeShared<FServer>();
		FFlockTestSdk Test;
		bool bAnalyticsWas = false;

		FSubsystemFixture()
		{
			UFlockConfig* Settings = GetMutableDefault<UFlockConfig>();
			bAnalyticsWas = Settings->bAnalyticsEnabled;
			Settings->bAnalyticsEnabled = false;
			const TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
			Store->Stored.AccessToken = MatchmakingJwt(PlayerA);
			Store->Stored.RefreshToken = TEXT("r-1");
			Store->bHasTokens = true;
			Test->SetHttpAdapterForTesting(Server);
			Test->SetTokenStoreForTesting(Store);
			FFlockInitConfig Config;
			Config.ApiUrl = TEXT("https://api-flock.qwacks.com");
			Config.ApiKey = TEXT("secret");
			Config.GameId = TEXT("my-game");
			Config.GameVersion = TEXT("1.2.3");
			Config.GameVersionId = FFlockFakeSessionServer::GameBuild;
			Test->InitializeWithConfig(Config);
		}

		~FSubsystemFixture()
		{
			GetMutableDefault<UFlockConfig>()->bAnalyticsEnabled = bAnalyticsWas;
		}

		FFlockMultiplayerProvider* Provider() const { return Test->GetMultiplayerProvider(); }
	};

	// Real answers from the backend, captured by the matchmaking rules probe on 2026-10-08, nulls included.
	inline const TCHAR* MatchedRead = TEXT(R"BODY({"id":"01M4DD484KEYM8KCGGZZMK7S3S","queue_id":"01M46J2MG533AXD49SBPTEPBRM","game_id":"01KVCSANMD9V0CRW85ADE1WJA1","player_id":"01M46J2SEP5GZ15W8TV9N2Q6MY","status":"matched","attributes":{},"party_id":null,"party_size":1,"match_id":"01M4DD4GAJB0W526MC8ZJ1X3ZQ","matched_at":"2026-10-08T09:22:29.324253","cancel_reason":null,"game_version_id":"01KVCSB5ND68QBATA4468M0APS","created_at":"2026-10-08T09:22:20.947180","match":{"id":"01M4DD4GAJB0W526MC8ZJ1X3ZQ","queue_id":"01M46J2MG533AXD49SBPTEPBRM","game_id":"01KVCSANMD9V0CRW85ADE1WJA1","status":"ready","player_ids":["01M46J2QM0KSQ1X3QNF03RRT6V","01M46J2QXJHZQPWEVG4AJX6MMF","01M46J2SEP5GZ15W8TV9N2Q6MY"],"connection_info":{"mode":"p2p","session_id":"01M4DD4GAPBE2N9FDZVVWJFFYT"},"created_at":"2026-10-08T09:22:29.330408"}})BODY");
	inline const TCHAR* ExpiredRead = TEXT(R"BODY({"id":"01M4DD5QM53B9PVPJX2K654X45","queue_id":"01M46J2MG533AXD49SBPTEPBRM","game_id":"01KVCSANMD9V0CRW85ADE1WJA1","player_id":"01M46J4VCTQ54NSPP206GSA069","status":"expired","attributes":{},"party_id":null,"party_size":1,"match_id":null,"matched_at":null,"cancel_reason":null,"game_version_id":"01KVCSB5ND68QBATA4468M0APS","created_at":"2026-10-08T09:23:09.573881","match":null})BODY");
	inline const TCHAR* CancelAnswer = TEXT(R"BODY({"id":"01M4DD3MJ2WEPBZHMR6B46RH6B","queue_id":"01M46J2MG533AXD49SBPTEPBRM","game_id":"01KVCSANMD9V0CRW85ADE1WJA1","player_id":"01M46J2QM0KSQ1X3QNF03RRT6V","status":"cancelled","attributes":{},"party_id":null,"party_size":1,"match_id":null,"matched_at":null,"cancel_reason":"player","game_version_id":"01KVCSB5ND68QBATA4468M0APS","created_at":"2026-10-08T09:22:00.898408"})BODY");
	inline const TCHAR* PartyTicketsMade = TEXT(R"BODY({"error":{"code":null},"response":{"message":null,"code":null},"result":[{"id":"01M4DD403BPAZX7JV58263WCP0","queue_id":"01M46J2MG533AXD49SBPTEPBRM","game_id":"01KVCSANMD9V0CRW85ADE1WJA1","player_id":"01M46J2QM0KSQ1X3QNF03RRT6V","status":"queued","attributes":{},"party_id":"01M4DD3Z5XA9NQJ74SBD1BX698","party_size":2,"match_id":null,"matched_at":null,"cancel_reason":null,"game_version_id":"01KVCSB5ND68QBATA4468M0APS","created_at":"2026-10-08T09:22:12.715871"},{"id":"01M4DD403C8AVYGYQKZ87Y7BKQ","queue_id":"01M46J2MG533AXD49SBPTEPBRM","game_id":"01KVCSANMD9V0CRW85ADE1WJA1","player_id":"01M46J2QXJHZQPWEVG4AJX6MMF","status":"queued","attributes":{},"party_id":"01M4DD3Z5XA9NQJ74SBD1BX698","party_size":2,"match_id":null,"matched_at":null,"cancel_reason":null,"game_version_id":"01KVCSB5ND68QBATA4468M0APS","created_at":"2026-10-08T09:22:12.715871"}]})BODY");
}

#endif // WITH_AUTOMATION_TESTS
