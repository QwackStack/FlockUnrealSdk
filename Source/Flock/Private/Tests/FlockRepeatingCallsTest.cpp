// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "FlockInitConfig.h"
#include "FlockSubsystem.h"
#include "HAL/PlatformProcess.h"
#include "Multiplayer/FlockRepeatingCalls.h"
#include "Providers/FlockMultiplayerProvider.h"
#include "Tests/Support/FlockRecordingLogger.h"
#include "Tests/Support/FlockTestSdk.h"

namespace FlockRepeatingCallsTestHelpers
{
	/** The requests one repeating call sent: each held until the test answers it, unless bAnswerAtOnce. */
	struct FHeldSends
	{
		int32 Sent = 0;
		int32 Cancelled = 0;
		TArray<TFunction<void(TFlockResult<int32>)>> Waiting;
		TOptional<TFlockResult<int32>> AnswerAtOnce;

		void Answer(const TFlockResult<int32>& Result)
		{
			if (Waiting.Num() == 0)
			{
				return;
			}
			TFunction<void(TFlockResult<int32>)> OnResult = Waiting[0];
			Waiting.RemoveAt(0);
			OnResult(Result);
		}
	};

	inline FFlockRetryHandler::FOperation<int32> SendInto(const TSharedRef<FHeldSends>& Held)
	{
		return [Held](TFunction<void(TFlockResult<int32>)> OnResult) -> FFlockRequestHandle
		{
			++Held->Sent;
			if (Held->AnswerAtOnce.IsSet())
			{
				OnResult(Held->AnswerAtOnce.GetValue());
				return FFlockRequestHandle();
			}
			Held->Waiting.Add(OnResult);
			const TSharedPtr<FFlockCancelToken> Token = MakeShared<FFlockCancelToken>();
			Token->Canceller = [Held]() { ++Held->Cancelled; };
			return FFlockRequestHandle(Token);
		};
	}

	inline TFlockResult<int32> Failure(EFlockErrorType Type, int32 Status = 0, const FString& Code = FString())
	{
		return TFlockResult<int32>::Fail(FFlockError::Make(Type, TEXT("test failure"), Status, FString(), Code));
	}

	/** A clock, a jitter and a sign-in the test moves by hand. */
	struct FRepeatingCallsFixture
	{
		double Now = 100.0;
		TArray<double> Rolls;
		int32 RollsDrawn = 0;
		int32 SignInNumber = 1;
		TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
		FFlockRepeatingCalls Calls;

		FRepeatingCallsFixture()
			: Calls([this]() { return SignInNumber; }, Log,
				[this]() { const double Roll = Rolls.IsValidIndex(RollsDrawn) ? Rolls[RollsDrawn] : 0.5; ++RollsDrawn; return Roll; },
				[this]() { return Now; })
		{
		}

		TSharedPtr<FFlockRepeatingCall> Start(const FString& Name, double Interval, const TSharedRef<FHeldSends>& Held,
			TFunction<void(const int32&)> UseAnswer = nullptr, TFunction<void(const FFlockError&)> Stopped = nullptr)
		{
			return Calls.Start<int32>(Name, Interval, SendInto(Held), MoveTemp(UseAnswer), MoveTemp(Stopped));
		}

		void At(double Time)
		{
			Now = Time;
			Calls.SendThoseDue();
		}
	};
}

using namespace FlockRepeatingCallsTestHelpers;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRepeatingCallsJitterTest, "Flock.Multiplayer.RepeatingCalls.WaitsAJitteredInterval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockRepeatingCallsJitterTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("the shortest wait is three quarters of the interval"), FFlockRepeatingCalls::JitteredWait(2.0, 0.0), 1.5);
	TestEqual(TEXT("the middle wait is the interval"), FFlockRepeatingCalls::JitteredWait(2.0, 0.5), 2.0);
	TestEqual(TEXT("the longest wait is one and a quarter of it"), FFlockRepeatingCalls::JitteredWait(2.0, 1.0), 2.5);

	FRepeatingCallsFixture F;
	F.Rolls = { 0.0, 1.0, 0.5 };
	const TSharedRef<FHeldSends> Held = MakeShared<FHeldSends>();
	Held->AnswerAtOnce = TFlockResult<int32>::Ok(1);
	TestTrue(TEXT("started"), F.Start(TEXT("search"), 2.0, Held).IsValid());

	F.At(100.0);
	TestEqual(TEXT("nothing is sent when it starts"), Held->Sent, 0);
	F.At(101.49);
	TestEqual(TEXT("nor before its first wait is over"), Held->Sent, 0);
	F.At(101.5);
	TestEqual(TEXT("sent once the first wait (rolled 0) is over"), Held->Sent, 1);

	// Each wait draws a roll of its own: this one rolled 1, so 2.5 s from the answer.
	F.At(103.99);
	TestEqual(TEXT("not before the second wait is over"), Held->Sent, 1);
	F.At(104.0);
	TestEqual(TEXT("sent once the second wait (rolled 1) is over"), Held->Sent, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRepeatingCallsOneAtATimeTest, "Flock.Multiplayer.RepeatingCalls.SendsOneRequestAtATime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockRepeatingCallsOneAtATimeTest::RunTest(const FString& Parameters)
{
	FRepeatingCallsFixture F;
	const TSharedRef<FHeldSends> Held = MakeShared<FHeldSends>();
	TArray<int32> Answers;
	const TSharedPtr<FFlockRepeatingCall> Call = F.Start(TEXT("heartbeat"), 1.0, Held, [&Answers](const int32& Value) { Answers.Add(Value); });

	F.At(101.0);
	TestEqual(TEXT("sent when due"), Held->Sent, 1);
	TestTrue(TEXT("waiting for its answer"), Call->IsWaitingForAnswer());
	F.At(150.0);
	TestEqual(TEXT("nothing more while the answer is on its way, however long it takes"), Held->Sent, 1);

	Held->Answer(TFlockResult<int32>::Ok(7));
	TestEqual(TEXT("the answer is used"), Answers.Num(), 1);
	TestFalse(TEXT("no longer waiting"), Call->IsWaitingForAnswer());
	F.At(150.99);
	TestEqual(TEXT("the next wait is counted from the answer"), Held->Sent, 1);
	F.At(151.0);
	TestEqual(TEXT("then sent again"), Held->Sent, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRepeatingCallsSignInTest, "Flock.Multiplayer.RepeatingCalls.EndsWithItsSignIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockRepeatingCallsSignInTest::RunTest(const FString& Parameters)
{
	FRepeatingCallsFixture F;
	const TSharedRef<FHeldSends> Held = MakeShared<FHeldSends>();
	int32 Answers = 0;
	int32 Stops = 0;
	const TSharedPtr<FFlockRepeatingCall> Call = F.Start(TEXT("party"), 1.0, Held,
		[&Answers](const int32&) { ++Answers; }, [&Stops](const FFlockError&) { ++Stops; });
	F.At(101.0);
	TestEqual(TEXT("sent under the first sign-in"), Held->Sent, 1);

	// A sign-out and another sign-in both move the sign-in number (Flock.Auth.SignIn.NumberMovesOnSignInAndSignOutOnly).
	F.SignInNumber += 1;
	TestFalse(TEXT("no longer running"), Call->IsRunning());
	Held->Answer(TFlockResult<int32>::Ok(1));
	TestEqual(TEXT("an answer landing afterwards is not used"), Answers, 0);
	TestEqual(TEXT("nor reported as a failure"), Stops, 0);

	F.At(500.0);
	TestEqual(TEXT("never sent for the next sign-in"), Held->Sent, 1);
	TestEqual(TEXT("and let go of"), F.Calls.GetCallCountForTesting(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRepeatingCallsSendMomentTest, "Flock.Multiplayer.RepeatingCalls.ChecksTheSignInAsEachIsSent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockRepeatingCallsSendMomentTest::RunTest(const FString& Parameters)
{
	// Two calls due in the same frame; the first one's answer signs the player out before the second is sent.
	for (const bool bSignOutInTheAnswer : { false, true })
	{
		FRepeatingCallsFixture F;
		const TSharedRef<FHeldSends> First = MakeShared<FHeldSends>();
		const TSharedRef<FHeldSends> Second = MakeShared<FHeldSends>();
		First->AnswerAtOnce = TFlockResult<int32>::Ok(1);
		F.Start(TEXT("first"), 1.0, First, [&F, bSignOutInTheAnswer](const int32&)
		{
			if (bSignOutInTheAnswer)
			{
				F.SignInNumber += 1;
			}
		});
		F.Start(TEXT("second"), 1.0, Second);
		F.At(101.0);
		TestEqual(TEXT("the first is sent"), First->Sent, 1);
		TestEqual(bSignOutInTheAnswer ? TEXT("the second is not sent once the sign-in has ended")
			: TEXT("control: the second is sent in the same frame"), Second->Sent, bSignOutInTheAnswer ? 0 : 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRepeatingCallsFailuresTest, "Flock.Multiplayer.RepeatingCalls.KeepsGoingOnlyThroughFailuresThatCanPass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockRepeatingCallsFailuresTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* Name;
		TFlockResult<int32> Result;
		bool bKeepsGoing;
	};
	const FCase Cases[] = {
		{ TEXT("no answer"), Failure(EFlockErrorType::Connection), true },
		{ TEXT("a timeout"), Failure(EFlockErrorType::Timeout), true },
		{ TEXT("an unreadable answer"), Failure(EFlockErrorType::Serialization, 200), true },
		{ TEXT("a 503"), Failure(EFlockErrorType::Network, 503), true },
		{ TEXT("a 500"), Failure(EFlockErrorType::Network, 500), true },
		{ TEXT("a 429"), Failure(EFlockErrorType::Network, 429), true },
		{ TEXT("a 408"), Failure(EFlockErrorType::Network, 408), true },
		{ TEXT("a 404"), Failure(EFlockErrorType::Network, 404, TEXT("multiplayer.session_not_found")), false },
		{ TEXT("a 422"), Failure(EFlockErrorType::Validation, 422), false },
		{ TEXT("a 401"), Failure(EFlockErrorType::Auth, 401), false },
		{ TEXT("a coded 403"), Failure(EFlockErrorType::Auth, 403, TEXT("multiplayer.not_host")), false },
		{ TEXT("a cancellation"), Failure(EFlockErrorType::Cancelled), false },
	};
	for (const FCase& Case : Cases)
	{
		TestEqual(FString::Printf(TEXT("%s: worth another try"), Case.Name),
			FFlockRepeatingCalls::IsWorthAnotherTry(Case.Result.Error), Case.bKeepsGoing);

		FRepeatingCallsFixture F;
		const TSharedRef<FHeldSends> Held = MakeShared<FHeldSends>();
		Held->AnswerAtOnce = Case.Result;
		TArray<FFlockError> Stops;
		const TSharedPtr<FFlockRepeatingCall> Call = F.Start(TEXT("search"), 1.0, Held, nullptr,
			[&Stops](const FFlockError& Error) { Stops.Add(Error); });
		F.At(101.0);
		F.At(103.0);
		F.At(105.0);
		if (Case.bKeepsGoing)
		{
			TestEqual(FString::Printf(TEXT("%s: sent again"), Case.Name), Held->Sent, 3);
			TestTrue(FString::Printf(TEXT("%s: still running"), Case.Name), Call->IsRunning());
			TestEqual(FString::Printf(TEXT("%s: not reported"), Case.Name), Stops.Num(), 0);
		}
		else
		{
			TestEqual(FString::Printf(TEXT("%s: not sent again"), Case.Name), Held->Sent, 1);
			TestFalse(FString::Printf(TEXT("%s: stopped"), Case.Name), Call->IsRunning());
			if (TestEqual(FString::Printf(TEXT("%s: reported once"), Case.Name), Stops.Num(), 1))
			{
				TestEqual(FString::Printf(TEXT("%s: with its own failure"), Case.Name), Stops[0].StatusCode, Case.Result.Error.StatusCode);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRepeatingCallsRetryAfterTest, "Flock.Multiplayer.RepeatingCalls.WaitsAsLongAsTheServerAsks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockRepeatingCallsRetryAfterTest::RunTest(const FString& Parameters)
{
	for (const float RetryAfter : { 30.f, 0.25f })
	{
		FRepeatingCallsFixture F;
		const TSharedRef<FHeldSends> Held = MakeShared<FHeldSends>();
		TFlockResult<int32> Limited = Failure(EFlockErrorType::Network, 429);
		Limited.Error.bHasRetryAfter = true;
		Limited.Error.RetryAfterSeconds = RetryAfter;
		Held->AnswerAtOnce = Limited;
		F.Start(TEXT("search"), 1.0, Held);
		F.At(101.0);

		// A longer ask is honoured; a shorter one keeps the call's own interval.
		const double NextSend = 101.0 + FMath::Max(1.0, static_cast<double>(RetryAfter));
		F.At(NextSend - 0.01);
		TestEqual(FString::Printf(TEXT("Retry-After %.2f: not before %.2f s"), RetryAfter, NextSend - 101.0), Held->Sent, 1);
		F.At(NextSend);
		TestEqual(FString::Printf(TEXT("Retry-After %.2f: sent at %.2f s"), RetryAfter, NextSend - 101.0), Held->Sent, 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRepeatingCallsStartTest, "Flock.Multiplayer.RepeatingCalls.RefusesASecondCallOfOneName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockRepeatingCallsStartTest::RunTest(const FString& Parameters)
{
	FRepeatingCallsFixture F;
	const TSharedRef<FHeldSends> Held = MakeShared<FHeldSends>();
	const TSharedPtr<FFlockRepeatingCall> First = F.Start(TEXT("search"), 1.0, Held);
	TestTrue(TEXT("the first starts"), First.IsValid());
	TestFalse(TEXT("a second of the same name is refused"), F.Start(TEXT("search"), 1.0, Held).IsValid());
	TestTrue(TEXT("and logged as an error"), FFlockRecordingLogger::AnyContains(F.Log->Errors, TEXT("already running")));
	TestTrue(TEXT("a name differing in letter case is another call"), F.Start(TEXT("Search"), 1.0, Held).IsValid());

	First->Stop();
	TestTrue(TEXT("once the first stops, its name is free"), F.Start(TEXT("search"), 1.0, Held).IsValid());

	TestFalse(TEXT("no name is refused"), F.Start(TEXT(""), 1.0, Held).IsValid());
	TestFalse(TEXT("no interval is refused"), F.Start(TEXT("zero"), 0.0, Held).IsValid());
	TestFalse(TEXT("nothing to send is refused"), F.Calls.Start<int32>(TEXT("empty"), 1.0, nullptr, nullptr, nullptr).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRepeatingCallsStopTest, "Flock.Multiplayer.RepeatingCalls.StopCancelsWhatIsOnItsWay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockRepeatingCallsStopTest::RunTest(const FString& Parameters)
{
	// One call stopped by the SDK feature that started it.
	{
		FRepeatingCallsFixture F;
		const TSharedRef<FHeldSends> Held = MakeShared<FHeldSends>();
		int32 Answers = 0;
		const TSharedPtr<FFlockRepeatingCall> Call = F.Start(TEXT("search"), 1.0, Held, [&Answers](const int32&) { ++Answers; });
		F.At(101.0);
		Call->Stop();
		TestEqual(TEXT("one call: its request is cancelled"), Held->Cancelled, 1);
		Held->Answer(TFlockResult<int32>::Ok(1));
		TestEqual(TEXT("one call: a late answer is not used"), Answers, 0);
		F.At(200.0);
		TestEqual(TEXT("one call: never sent again"), Held->Sent, 1);
	}
	// Every call, at Flock's shutdown.
	{
		FRepeatingCallsFixture F;
		const TSharedRef<FHeldSends> Waiting = MakeShared<FHeldSends>();
		const TSharedRef<FHeldSends> Idle = MakeShared<FHeldSends>();
		Idle->AnswerAtOnce = TFlockResult<int32>::Ok(1);
		F.Start(TEXT("heartbeat"), 1.0, Waiting);
		F.Start(TEXT("party"), 5.0, Idle);
		F.At(101.0);
		F.Calls.StopAll();
		TestEqual(TEXT("shutdown: the request on its way is cancelled"), Waiting->Cancelled, 1);
		TestEqual(TEXT("shutdown: every call is let go of"), F.Calls.GetCallCountForTesting(), 0);
		F.At(500.0);
		TestEqual(TEXT("shutdown: nothing more is sent"), Waiting->Sent + Idle->Sent, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRepeatingCallsTickerTest, "Flock.Multiplayer.RepeatingCalls.SentFromTheCoreTickerUntilShutdown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRepeatingCallsTickerTest::RunTest(const FString& Parameters)
{
	FFlockInitConfig Config;
	Config.ApiUrl = TEXT("https://api-flock.qwacks.com");
	Config.ApiKey = TEXT("secret");
	Config.GameId = TEXT("my-game");
	Config.GameVersion = TEXT("1.2.3");
	Config.GameVersionId = TEXT("ver-abc");

	const FFlockTestSdk Test;
	TestNull(TEXT("no multiplayer before Flock starts"), Test.Sdk->GetMultiplayerProvider());
	Test.Sdk->InitializeWithConfig(Config);
	FFlockMultiplayerProvider* Provider = Test.Sdk->GetMultiplayerProvider();
	if (!TestNotNull(TEXT("multiplayer is built when Flock starts"), Provider))
	{
		return false;
	}

	const TSharedRef<FHeldSends> Held = MakeShared<FHeldSends>();
	const TSharedPtr<FFlockRepeatingCall> Call = Provider->GetRepeatingCalls().Start<int32>(TEXT("search"), 0.001, SendInto(Held), nullptr, nullptr);
	TestTrue(TEXT("started"), Call.IsValid());
	for (int32 Frame = 0; Frame < 20 && Held->Sent == 0; ++Frame)
	{
		FPlatformProcess::Sleep(0.005f);
		FTSTicker::GetCoreTicker().Tick(0.005f);
	}
	TestEqual(TEXT("the game's own frames send it"), Held->Sent, 1);

	Test.Sdk->ShutdownSdk();
	TestEqual(TEXT("Flock's shutdown cancels the request on its way"), Held->Cancelled, 1);
	TestNull(TEXT("multiplayer is gone after shutdown"), Test.Sdk->GetMultiplayerProvider());
	Held->Answer(TFlockResult<int32>::Ok(1));
	for (int32 Frame = 0; Frame < 5; ++Frame)
	{
		FPlatformProcess::Sleep(0.005f);
		FTSTicker::GetCoreTicker().Tick(0.005f);
	}
	TestEqual(TEXT("nothing is sent after shutdown"), Held->Sent, 1);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
