// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"
#include "Misc/FlockEngineCompat.h"

#if WITH_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FlockSubsystem.h"
#include "Misc/ScopeExit.h"
#include "Multiplayer/FlockGameModes.h"
#include "Multiplayer/FlockJoinChecksOnHost.h"
#include "Providers/FlockMultiplayerProvider.h"
#include "Tests/Support/FlockConnectingFixture.h"
#include "Tests/Support/FlockMultiplayerNodeTestListener.h"

namespace FlockGameModesTestHelpers
{
	using namespace FlockConnectingTestHelpers;

	const FString JoinersAddress = TEXT("198.51.100.20");

	/** The running game's world (under -game), or null in the editor's pass. */
	inline UWorld* RunningGameWorld()
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::Game && Context.World() != nullptr && Context.OwningGameInstance != nullptr)
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	/**
	 * What a game mode's PreLoginAsync answered for one joining player: the engine hands its options and address to the game
	 * mode as it does for a connection, and the answer is a refusal's words, or empty for let in.
	 */
	inline TOptional<FString> AskedToLetIn(AGameModeBase* GameMode, FScriptedGameTravel& Game, const FString& Options)
	{
		TOptional<FString> Answer;
		Game.Connects(Options, JoinersAddress);
		GameMode->PreLoginAsync(Options, JoinersAddress, FUniqueNetIdRepl(),
			AGameModeBase::FOnPreLoginCompleteDelegate::CreateLambda([&Answer](const FString& Refusal) { Answer = Refusal; }));
		return Answer;
	}

	/** A spelling read letter for letter. A helper outside a test's RunTest cannot use TestEqualSensitive: below UE 5.5 it is a macro on the test itself. */
	inline void ExpectSpelled(FAutomationTestBase& Test, const TCHAR* What, const FString& Actual, const FString& Expected)
	{
		Test.TestTrue(*FString::Printf(TEXT("%s (expected '%s', was '%s')"), What, *Expected, *Actual), Actual.Equals(Expected, ESearchCase::CaseSensitive));
	}

	inline FString ReasonIn(const TOptional<FString>& Answer)
	{
		FString Reason;
		return Answer.IsSet() && FlockHostRefusals::TryRead(Answer.GetValue(), Reason) ? Reason : FString(TEXT("<not a host's refusal>"));
	}

	/**
	 * The game mode a game would make from TGameMode, in a world of its own whose game instance is the running game's, so its
	 * PreLoginAsync reaches the running game's Flock as a hosting game's does. That Flock's check hosts a session of the test's
	 * connecting fixture (its server and its engine side), and stops when the test ends.
	 */
	template <typename TGameMode>
	bool RunWithTheRunningGamesFlock(FAutomationTestBase& Test, TFunctionRef<void(TGameMode&, FConnectingFixture&, const FFlockFakeSessionServer::FSessionRef&)> Body)
	{
		UWorld* Running = RunningGameWorld();
		UFlockSubsystem* Sdk = Running != nullptr ? UFlockSubsystem::Get(Running) : nullptr;
		FFlockMultiplayerProvider* Multiplayer = Sdk != nullptr ? Sdk->GetMultiplayerProvider() : nullptr;
		if (Multiplayer == nullptr)
		{
			Test.AddInfo(TEXT("No running game with Flock here (the editor's pass): a Flock game mode's call runs under -game."));
			return true;
		}

		FConnectingFixture F;
		F.SignIn(PlayerA);
		UFlockMultiplayerSession* Hosted = F.Hosted(Test);
		if (Hosted == nullptr)
		{
			return false;
		}
		const FFlockFakeSessionServer::FSessionRef Served = F.Server->ById(Hosted->GetId()).ToSharedRef();
		F.Game->Opens(7777);
		FFlockJoinChecksOnHost& Checks = Multiplayer->GetJoinChecks();
		Checks.StartHosting(Hosted, F.Game);
		ON_SCOPE_EXIT
		{
			Checks.StopHosting();
		};

		UGameInstance* RunningInstance = Running->GetGameInstance();
		const FWorldContext* RunningContext = RunningInstance->GetWorldContext();
		{
			UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("FlockGameModesTest"));
			// A world context of its own (the engine asks for one while it makes the game mode) that owns no game instance: one
			// owning the running game's would take that instance's world context over, and leave it with none once destroyed.
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			ON_SCOPE_EXIT
			{
				World->SetGameInstance(nullptr);
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			};
			World->SetGameInstance(RunningInstance);
			FURL Url;
			Url.AddOption(*FString::Printf(TEXT("game=%s"), *TGameMode::StaticClass()->GetPathName()));
			if (!Test.TestTrue(TEXT("precondition: the engine made the game mode"), World->SetGameMode(Url)))
			{
				return false;
			}
			TGameMode* GameMode = Cast<TGameMode>(World->GetAuthGameMode());
			if (!Test.TestNotNull(TEXT("precondition: of the Flock game mode's class"), GameMode))
			{
				return false;
			}
			// As loading a level does (and with it the game mode's InitGame): an actor's Blueprint event runs only in a world whose
			// actors are initialized, and UE 5.4 sends Approve Joining Player through the Blueprint event even with no override.
			World->InitializeActorsForPlay(Url);
			if (!Test.TestTrue(TEXT("precondition: its actors initialized, as a hosting game's level is"), World->AreActorsInitialized()))
			{
				return false;
			}
			Test.TestTrue(TEXT("precondition: it reaches the running game's Flock"), UFlockSubsystem::Get(GameMode) == Sdk);
			Body(*GameMode, F, Served);
		}
		Test.TestTrue(TEXT("the running game is left as it was: its game instance's world and world context"),
			RunningInstance->GetWorldContext() == RunningContext && RunningInstance->GetWorld() == Running);
		return true;
	}

	template <typename TGameMode>
	void LetsInOnlyWhatFlocksCheckAndTheGameApprove(FAutomationTestBase& Test, TGameMode& GameMode, FConnectingFixture& F,
		const FFlockFakeSessionServer::FSessionRef& Served)
	{
		const TOptional<FString> NoToken = AskedToLetIn(&GameMode, *F.Game, TEXT("?Name=Ann"));
		ExpectSpelled(Test, TEXT("no token: refused by Flock's check, not let in by the engine's own"), ReasonIn(NoToken),
			FString(FlockTravelToSessionRefusedReasons::NoJoinToken));
		Test.TestEqual(TEXT("the game's own check not asked"), GameMode.Asked, 0);

		F.Server->Joins(Served, PlayerB);
		GameMode.bApproves = false;
		GameMode.Reason = TEXT("banned");
		const FString Token = F.Server->MintsJoinToken(Served, PlayerB);
		const TOptional<FString> Banned = AskedToLetIn(&GameMode, *F.Game, FString::Printf(TEXT("?FlockJoinToken=%s?Name=Bo"), *Token));
		ExpectSpelled(Test, TEXT("a good token the game refuses: the game's reason"), ReasonIn(Banned), FString(TEXT("banned")));
		Test.TestEqual(TEXT("the game asked once"), GameMode.Asked, 1);
		ExpectSpelled(Test, TEXT("about the player Flock verified"), GameMode.LastAsked.PlayerId, PlayerB);
		Test.TestFalse(TEXT("with the options, not the token"), GameMode.LastAsked.Options.Contains(TEXT("FlockJoinToken"), ESearchCase::CaseSensitive));

		GameMode.bApproves = true;
		GameMode.Reason.Reset();
		const TOptional<FString> Approved = AskedToLetIn(&GameMode, *F.Game, FString::Printf(TEXT("?FlockJoinToken=%s?Name=Bo"), *Token));
		Test.TestTrue(TEXT("the game approves: let in"), Approved.IsSet() && Approved.GetValue().IsEmpty());
		Test.TestEqual(TEXT("asked again"), GameMode.Asked, 2);
	}
}

using namespace FlockGameModesTestHelpers;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameModeBaseTest, "Flock.Multiplayer.GameModes.FlockGameModeBase_LetsInOnlyWhatFlocksCheckAndTheGameApprove",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FFlockGameModeBaseTest::RunTest(const FString& Parameters)
{
	return RunWithTheRunningGamesFlock<AFlockTestGameModeBase>(*this,
		[this](AFlockTestGameModeBase& GameMode, FConnectingFixture& F, const FFlockFakeSessionServer::FSessionRef& Served)
		{
			LetsInOnlyWhatFlocksCheckAndTheGameApprove(*this, GameMode, F, Served);
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameModeTest, "Flock.Multiplayer.GameModes.FlockGameMode_LetsInOnlyWhatFlocksCheckAndTheGameApprove",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FFlockGameModeTest::RunTest(const FString& Parameters)
{
	return RunWithTheRunningGamesFlock<AFlockTestGameMode>(*this,
		[this](AFlockTestGameMode& GameMode, FConnectingFixture& F, const FFlockFakeSessionServer::FSessionRef& Served)
		{
			LetsInOnlyWhatFlocksCheckAndTheGameApprove(*this, GameMode, F, Served);
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameModesDefaultTest, "Flock.Multiplayer.GameModes.ApproveJoiningPlayer_LetsEveryoneInUntilOverridden",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FFlockGameModesDefaultTest::RunTest(const FString& Parameters)
{
	FFlockJoiningPlayer Player;
	Player.PlayerId = TEXT("player-b");
	FString Reason;
	TestTrue(TEXT("Flock Game Mode Base"), GetMutableDefault<AFlockGameModeBase>()->ApproveJoiningPlayer(Player, Reason));
	TestTrue(TEXT("Flock Game Mode"), GetMutableDefault<AFlockGameMode>()->ApproveJoiningPlayer(Player, Reason));
	TestTrue(TEXT("no reason given"), Reason.IsEmpty());
	return true;
}

#endif // WITH_AUTOMATION_TESTS
