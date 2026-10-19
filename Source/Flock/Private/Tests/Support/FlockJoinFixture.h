// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DebugCameraController.h"
#include "GameFramework/PlayerController.h"
#include "Multiplayer/FlockJoinChecks.h"
#include "Multiplayer/FlockJoinChecksOnHost.h"
#include "Tests/Support/FlockConnectingFixture.h"

// A host checking joining players through Travel To Session over the connecting fixture, for the join verification and relay tests.
// Include only from guarded test files.

namespace FlockJoinVerificationTestHelpers
{
	using namespace FlockConnectingTestHelpers;

	/** What the engine was handed for one joining player: empty lets them in. */
	struct FJoinAnswer
	{
		bool bAnswered = false;
		int32 Times = 0;
		FString Refusal;

		bool LetIn() const { return bAnswered && Refusal.IsEmpty(); }

		/** The reason Flock's check gave, or a marker when it gave none (let in, unanswered, or words of another's). */
		FString RefusedFor() const
		{
			FString Reason;
			return bAnswered && FlockHostRefusals::TryRead(Refusal, Reason) ? Reason : FString(TEXT("<not refused by Flock's check>"));
		}
	};

	struct FJoining
	{
		uint64 Connection = 0;
		TSharedRef<FJoinAnswer> Answer = MakeShared<FJoinAnswer>();
	};

	const FString JoinersAddress = TEXT("198.51.100.20");

	/** The options a joining player's travel URL carries to the host, as the engine hands them over (measured: the token first). */
	inline FString WithToken(const FString& Token)
	{
		return FString::Printf(TEXT("?FlockJoinToken=%s?Name=Ann?SplitscreenCount=1"), *Token);
	}

	/** Stand-in player controllers the check never uses as players: the class defaults of two controller classes. */
	inline const APlayerController* HostsOwnController() { return GetDefault<APlayerController>(); }
	inline const APlayerController* JoinedController() { return GetDefault<ADebugCameraController>(); }

	struct FJoinFixture : FConnectingFixture
	{
		explicit FJoinFixture(int32 MaxRetries = 0)
			: FConnectingFixture(MaxRetries)
		{
		}

		FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
		/** Every game's-check call, in order. */
		TArray<FFlockJoiningPlayer> GameAsked;
		/** Every engine-checks call. */
		int32 EngineAsked = 0;

		FFlockJoinChecksOnHost& Checks() { return Provider->GetJoinChecks(); }

		/** This game (A) hosts a session through Travel To Session: its level listening on 7777 and its direct connection published. */
		UFlockMultiplayerSession* HostingAsA(FAutomationTestBase& Test)
		{
			SignIn(PlayerA);
			UFlockMultiplayerSession* Held = Hosted(Test);
			if (Held == nullptr)
			{
				return nullptr;
			}
			Served = Server->ById(Held->GetId()).ToSharedRef();
			Game->OwnPlayers.Add(HostsOwnController());
			const TSharedRef<FTravelAnswer> Hosting = Travel(Held);
			Test.TestTrue(TEXT("precondition: checking from before the level listens"), Checks().IsHostingForTesting());
			Game->Opens(7777);
			FramesUntil([&]() { return Hosting->bAnswered; });
			if (!Test.TestTrue(TEXT("precondition: hosting"), Hosting->Succeeded()
				&& Hosting->Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::StartedAsHost, ESearchCase::CaseSensitive)))
			{
				return nullptr;
			}
			return Held;
		}

		/** A player seated in the hosted session, and a token the server minted for them. */
		FString SeatedWithToken(const FString& PlayerId)
		{
			Server->Joins(Served, PlayerId);
			return Server->MintsJoinToken(Served, PlayerId);
		}

		/**
		 * One player's connection reaches the host and the game mode's PreLoginAsync hands it to Flock's check, as the engine asks it.
		 * AskedOptions is what the game mode passes, the connection's own options unless a test says otherwise; a relayed player's
		 * connection comes from 127.0.0.1 on LoopbackPort, one of the relay bridge's.
		 */
		FJoining Joins(const FString& Options, FFlockApproveJoiningPlayer GamesCheck = nullptr, TFunction<FString()> EngineChecks = nullptr,
			const TOptional<FString>& AskedOptions = TOptional<FString>(), const FString& Address = JoinersAddress, int32 LoopbackPort = 0)
		{
			FJoining Joining;
			Joining.Connection = Game->Connects(Options, Address, LoopbackPort);
			FFlockJoiningRequest Request;
			Request.Options = AskedOptions.Get(Options);
			Request.Address = Address;
			Request.RunEngineChecks = [this, EngineChecks]()
			{
				++EngineAsked;
				return EngineChecks ? EngineChecks() : FString();
			};
			const TSharedRef<FJoinAnswer> Answer = Joining.Answer;
			Request.Answer = [Answer](const FString& Refusal)
			{
				Answer->bAnswered = true;
				++Answer->Times;
				Answer->Refusal = Refusal;
			};
			Request.IsAskerAlive = []() { return true; };
			Request.ApproveJoiningPlayer = GamesCheck ? MoveTemp(GamesCheck) : FFlockApproveJoiningPlayer([this](const FFlockJoiningPlayer& Player, FString&)
			{
				GameAsked.Add(Player);
				return true;
			});
			Checks().Check(MoveTemp(Request));
			return Joining;
		}

		/** The connection plays through JoinedController from now on, as the engine hands a player controller to one let in. */
		void PlaysThrough(uint64 Connection) { Game->Controllers.Add(JoinedController(), Connection); }

		/** The token each verify request carried, in order. */
		TArray<FString> TokensVerified() const
		{
			TArray<FString> Tokens;
			for (const FFlockHttpRequest& Request : Server->RequestsTo(ERoute::Verify))
			{
				Tokens.Add(Field(BodyOf(Request), TEXT("token")));
			}
			return Tokens;
		}

		int32 ErrorsContaining(const FString& Needle) const
		{
			return Log->Errors.FilterByPredicate([&](const FString& Line) { return Line.Contains(Needle); }).Num();
		}
	};
}
