// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "Http/FlockHttpClient.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Multiplayer/FlockDirectAddresses.h"
#include "Multiplayer/FlockMultiplayerSession.h"
#include "Multiplayer/FlockMultiplayerSessions.h"
#include "Multiplayer/FlockTravelToSession.h"
#include "Multiplayer/FlockSockets.h"
#include "Providers/FlockMultiplayerProvider.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/Support/FlockFakeSessionServer.h"
#include "Tests/Support/FlockFakeStunServer.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockRecordingLogger.h"
#include "Tests/Support/FlockScriptedGameTravel.h"
#include "UObject/StrongObjectPtr.h"

// The session stack as the provider builds it, over a fake session server, a STUN server on loopback and a scripted engine, for
// the tests of connecting players and of join verification. Include only from guarded test files.

namespace FlockConnectingTestHelpers
{
	using ERoute = FFlockFakeSessionServer::ERoute;

	const FString PlayerA = TEXT("player-a");
	const FString PlayerB = TEXT("player-b");
	const FString PlayerC = TEXT("player-c");
	const FString HostsPublicAddress = TEXT("203.0.113.7");

	inline FString ConnectingJwt(const FString& PlayerId)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + 3600;
		FString Payload = FBase64::Encode(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp));
		Payload.ReplaceInline(TEXT("+"), TEXT("-"));
		Payload.ReplaceInline(TEXT("/"), TEXT("_"));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Payload);
	}

	template <typename T>
	struct TAnswer
	{
		bool bAnswered = false;
		int32 Times = 0;
		TFlockResult<T> Result;

		bool Succeeded() const { return bAnswered && Result.bSuccess; }
		bool FailedAs(EFlockErrorType Type) const { return bAnswered && !Result.bSuccess && Result.Error.Type == Type; }
	};

	template <typename T>
	TFunction<void(TFlockResult<T>)> Into(const TSharedRef<TAnswer<T>>& Answer)
	{
		return [Answer](TFlockResult<T> Result)
		{
			Answer->bAnswered = true;
			++Answer->Times;
			Answer->Result = Result;
		};
	}

	using FSessionAnswer = TAnswer<UFlockMultiplayerSession*>;
	using FAddressAnswer = TAnswer<FFlockDirectAddress>;
	using FTravelAnswer = TAnswer<FFlockTravelToSessionResult>;

	/** A descriptor as a host publishes it; an empty LAN or public address is left out. */
	inline TSharedRef<FJsonObject> Direct(const TSharedPtr<FJsonValue>& Address, const TSharedPtr<FJsonValue>& Port,
		const FString& Lan = TEXT("192.168.1.20"), const FString& Public = HostsPublicAddress)
	{
		const TSharedRef<FJsonObject> Connection = MakeShared<FJsonObject>();
		Connection->SetStringField(TEXT("mode"), TEXT("direct"));
		Connection->SetField(TEXT("address"), Address);
		Connection->SetField(TEXT("port"), Port);
		if (!Lan.IsEmpty())
		{
			Connection->SetStringField(TEXT("lan_address"), Lan);
		}
		if (!Public.IsEmpty())
		{
			Connection->SetStringField(TEXT("public_address"), Public);
		}
		return Connection;
	}

	inline TSharedPtr<FJsonValue> Text(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
	inline TSharedPtr<FJsonValue> Number(double Value) { return MakeShared<FJsonValueNumber>(Value); }

	inline TSharedPtr<FJsonObject> BodyOf(const FFlockHttpRequest& Request)
	{
		TSharedPtr<FJsonObject> Body;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Request.JsonBody), Body);
		return Body;
	}

	using FScriptedGameTravel = FFlockScriptedGameTravel;

	/** The session stack as the provider builds it, over a fake session server and a STUN server on loopback, time moved by hand. */
	struct FConnectingFixture
	{
		TSharedRef<double> Now = MakeShared<double>(1000.0);
		TSharedRef<FFlockFakeSessionServer> Server = MakeShared<FFlockFakeSessionServer>();
		TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockAuthSession> Session;
		TSharedRef<FFlockMultiplayerProvider> Provider;
		TUniquePtr<FFlockFakeStunServer> Stun = MakeUnique<FFlockFakeStunServer>();
		TSharedRef<FScriptedGameTravel> Game = MakeShared<FScriptedGameTravel>();
		TArray<TStrongObjectPtr<UFlockMultiplayerSession>> Kept;
		/** The object a game hands Travel To Session; null makes the engine side refuse, as for no world. */
		bool bHasWorld = true;
		/** The engine sides made so far, and the one (counted from 1) the factory refuses as if it found no world; 0 refuses none. */
		int32 GameSidesMade = 0;
		int32 RefuseGameSide = 0;

		/** MaxRetries above 0 lets a refused request be tried again, through the core ticker: see FlockTestRetries::Pump. */
		explicit FConnectingFixture(int32 MaxRetries = 0)
			: Client(MakeShared<FFlockHttpClient>(Server, Log))
			, Session(MakeShared<FFlockAuthSession>(Client, MakeShared<FFlockMemoryTokenStore>(), Log, TEXT("http://x/v1"),
				TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") }, { TEXT("X-Game-Version-ID"), FFlockFakeSessionServer::GameBuild } }))
			, Provider(MakeShared<FFlockMultiplayerProvider>(Client, Retries(MaxRetries), Log, Session, TEXT("http://x/v1"), 0.0))
		{
			FFlockSockets::ResetForTesting();
			const TSharedRef<double> Clock = Now;
			Provider->GetRepeatingCalls().SetClockForTesting([Clock]() { return *Clock; }, []() { return 0.5; });
			Sessions().SetConnectionWaitSecondsForTesting(0.2);
			Sessions().GetDirectAddressesForTesting().SetStunWaitForTesting(0.5);
			Server->StunUrls.Add(Stun->Url());
			const TSharedRef<FScriptedGameTravel> Scripted = Game;
			bool* HasWorld = &bHasWorld;
			int32* Made = &GameSidesMade;
			const int32* Refused = &RefuseGameSide;
			Sessions().GetTravels().SetGameTravelForTesting([Scripted, HasWorld, Made, Refused](UObject*, FString& OutRefusal) -> TSharedPtr<IFlockGameTravel>
			{
				if (!*HasWorld || ++*Made == *Refused)
				{
					OutRefusal = TEXT("Travel To Session needs the game's world: pass an object in it, such as an actor or the game instance.");
					return nullptr;
				}
				return Scripted;
			});
		}

		~FConnectingFixture()
		{
			// The test's game ends with it: no level of its stays open to players past the test.
			Game->State = FFlockGameNetState();
			Provider->Shutdown();
			FFlockSockets::ResetForTesting();
		}

		static FFlockRetryPolicy Retries(int32 MaxRetries)
		{
			FFlockRetryPolicy Policy;
			Policy.MaxRetries = MaxRetries;
			Policy.InitialDelaySeconds = 0.f;
			Policy.bUseJitter = false;
			return Policy;
		}

		FFlockMultiplayerSessions& Sessions() { return Provider->GetSessionsForTesting(); }

		void SignIn(const FString& PlayerId)
		{
			FString Error;
			Session->SetTokens(ConnectingJwt(PlayerId), TEXT("r-1"), Error);
		}

		/** One frame: the STUN server answers what has arrived, then the provider runs. */
		void Frame()
		{
			Stun->Serve();
			Provider->TickForTesting();
		}

		void Advance(double Seconds)
		{
			*Now += Seconds;
			Frame();
		}

		/** Runs frames, a millisecond apart in real time for the loopback to carry the STUN answer, until Done or about two seconds pass. */
		bool FramesUntil(TFunctionRef<bool()> Done)
		{
			for (int32 Index = 0; Index < 2000 && !Done(); ++Index)
			{
				Frame();
				FPlatformProcess::Sleep(0.001f);
			}
			return Done();
		}

		UFlockMultiplayerSession* Hosted(FAutomationTestBase& Test)
		{
			const TSharedRef<FSessionAnswer> Made = MakeShared<FSessionAnswer>();
			Provider->HostSession(TOptional<int32>(), FFlockCommandData(), true, Into(Made));
			Test.TestTrue(TEXT("precondition: the session was hosted"), Made->Succeeded() && Made->Result.Value != nullptr);
			if (Made->Result.Value != nullptr)
			{
				Kept.Emplace(Made->Result.Value);
			}
			return Made->Result.Value;
		}

		/** A session A hosts, joined by this game as B. */
		UFlockMultiplayerSession* JoinedAsB(FAutomationTestBase& Test, FFlockFakeSessionServer::FSessionRef& OutServed)
		{
			OutServed = Server->HostedBy(PlayerA);
			SignIn(PlayerB);
			const TSharedRef<FSessionAnswer> Made = MakeShared<FSessionAnswer>();
			Provider->JoinSession(OutServed->Code, Into(Made));
			Test.TestTrue(TEXT("precondition: the session was joined"), Made->Succeeded() && Made->Result.Value != nullptr);
			if (Made->Result.Value != nullptr)
			{
				Kept.Emplace(Made->Result.Value);
			}
			return Made->Result.Value;
		}

		TSharedRef<FSessionAnswer> Publish(UFlockMultiplayerSession* Of, int32 Port)
		{
			const TSharedRef<FSessionAnswer> Answer = MakeShared<FSessionAnswer>();
			Of->PublishDirectConnection(Port, Into(Answer));
			return Answer;
		}

		TSharedRef<FSessionAnswer> Wait(UFlockMultiplayerSession* Of, float Seconds, FFlockRequestHandle* OutHandle = nullptr)
		{
			const TSharedRef<FSessionAnswer> Answer = MakeShared<FSessionAnswer>();
			const FFlockRequestHandle Handle = Of->WaitForConnection(Seconds, Into(Answer));
			if (OutHandle != nullptr)
			{
				*OutHandle = Handle;
			}
			return Answer;
		}

		TSharedRef<FTravelAnswer> Travel(UFlockMultiplayerSession* Of, const FFlockTravelToSessionOptions& Options = FFlockTravelToSessionOptions(),
			FFlockRequestHandle* OutHandle = nullptr)
		{
			const TSharedRef<FTravelAnswer> Answer = MakeShared<FTravelAnswer>();
			const FFlockRequestHandle Handle = Of->TravelToSession(nullptr, Options, Into(Answer));
			if (OutHandle != nullptr)
			{
				*OutHandle = Handle;
			}
			return Answer;
		}

		/** The connection the last publish sent. */
		TSharedPtr<FJsonObject> LastPublished() const
		{
			const TArray<FFlockHttpRequest> Sent = Server->RequestsTo(ERoute::Publish);
			const TSharedPtr<FJsonObject> Body = Sent.Num() > 0 ? BodyOf(Sent.Last()) : nullptr;
			const TSharedPtr<FJsonObject>* Connection = nullptr;
			return Body.IsValid() && Body->TryGetObjectField(TEXT("connection_info"), Connection) && Connection != nullptr ? *Connection : nullptr;
		}

		static FString Field(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
		{
			FString Value;
			if (Object.IsValid())
			{
				Object->TryGetStringField(Name, Value);
			}
			return Value;
		}

		int32 WarningsContaining(const FString& Needle) const
		{
			return Log->Warnings.FilterByPredicate([&](const FString& Line) { return Line.Contains(Needle); }).Num();
		}
	};
}
