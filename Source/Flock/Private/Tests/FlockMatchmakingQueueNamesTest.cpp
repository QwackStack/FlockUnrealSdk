// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"
#include "Misc/FlockEngineCompat.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "FlockLogger.h"
#include "Http/FlockHttpClient.h"
#include "Misc/Base64.h"
#include "Multiplayer/FlockMatchmakingQueueNames.h"
#include "Providers/FlockMultiplayerProvider.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockRecordingLogger.h"
#include "Tests/Support/FlockTestSafeIndex.h"

namespace FlockMatchmakingQueueNamesTestHelpers
{
	inline FFlockMatchmakingQueue Queue(const FString& Id, const FString& Name)
	{
		FFlockMatchmakingQueue Made;
		Made.Id = Id;
		Made.Name = Name;
		return Made;
	}

	/** Reads of the queue list, each held until the test answers it. */
	struct FHeldReads
	{
		TArray<TFunction<void(TFlockResult<TArray<FFlockMatchmakingQueue>>)>> Waiting;

		void Answer(const TArray<FFlockMatchmakingQueue>& Queues)
		{
			Reply(TFlockResult<TArray<FFlockMatchmakingQueue>>::Ok(Queues));
		}

		void Reply(const TFlockResult<TArray<FFlockMatchmakingQueue>>& Result)
		{
			if (Waiting.Num() > 0)
			{
				TFunction<void(TFlockResult<TArray<FFlockMatchmakingQueue>>)> OnRead = Waiting[0];
				Waiting.RemoveAt(0);
				OnRead(Result);
			}
		}
	};

	struct FQueueNamesFixture
	{
		TSharedRef<FHeldReads> Reads = MakeShared<FHeldReads>();
		TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
		TSharedRef<FFlockMatchmakingQueueNames> Names;

		FQueueNamesFixture()
			: Names(MakeShared<FFlockMatchmakingQueueNames>(
				[Reads = Reads](TFunction<void(TFlockResult<TArray<FFlockMatchmakingQueue>>)> OnRead)
				{
					Reads->Waiting.Add(OnRead);
					return FFlockRequestHandle();
				},
				Log))
		{
		}

		/** The answer given at once, if any; a lookup answered later uses FindInto. */
		TOptional<TFlockResult<FString>> Find(const FString& Name)
		{
			const TSharedRef<TOptional<TFlockResult<FString>>> Answer = MakeShared<TOptional<TFlockResult<FString>>>();
			Names->FindId(Name, [Answer](TFlockResult<FString> Result) { *Answer = Result; });
			return *Answer;
		}

		void FindInto(const FString& Name, TOptional<TFlockResult<FString>>& Out, FFlockRequestHandle* OutHandle = nullptr)
		{
			const FFlockRequestHandle Handle = Names->FindId(Name, [&Out](TFlockResult<FString> Result) { Out = Result; });
			if (OutHandle != nullptr)
			{
				*OutHandle = Handle;
			}
		}
	};

	inline FString QueueListBody(const TArray<FFlockMatchmakingQueue>& Queues)
	{
		TArray<FString> Rows;
		for (const FFlockMatchmakingQueue& Each : Queues)
		{
			Rows.Add(FString::Printf(TEXT("{\"id\":\"%s\",\"name\":\"%s\",\"game_id\":\"g-1\",\"team_size\":2,\"teams_per_match\":2}"),
				*Each.Id, *Each.Name));
		}
		return FString::Printf(TEXT("{\"error\":null,\"response\":\"ok\",\"result\":[%s]}"), *FString::Join(Rows, TEXT(",")));
	}

	inline FString QueueTestJwt(const FString& PlayerId)
	{
		FString Payload = FBase64::Encode(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId,
			FDateTime::UtcNow().ToUnixTimestamp() + 3600));
		Payload.ReplaceInline(TEXT("+"), TEXT("-"));
		Payload.ReplaceInline(TEXT("/"), TEXT("_"));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Payload);
	}

	struct FMultiplayerProviderFixture
	{
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockAuthSession> Session;
		TSharedRef<FFlockMultiplayerProvider> Provider;

		FMultiplayerProviderFixture()
			: Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, MakeShared<FFlockMemoryTokenStore>(), MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") }, { TEXT("X-Game-Version-ID"), TEXT("ver-1") } }))
			, Provider(MakeShared<FFlockMultiplayerProvider>(Client, NoRetries(), MakeShared<FFlockNullLogger>(), Session, TEXT("http://x/v1"),
				/*PartyRefreshSeconds*/ 10.0))
		{
		}

		static FFlockRetryPolicy NoRetries()
		{
			FFlockRetryPolicy Policy;
			Policy.MaxRetries = 0;
			return Policy;
		}
	};
}

using namespace FlockMatchmakingQueueNamesTestHelpers;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockQueueNamesSharedReadTest, "Flock.Multiplayer.QueueNames.OneReadForEveryoneAskingAtOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockQueueNamesSharedReadTest::RunTest(const FString& Parameters)
{
	FQueueNamesFixture F;
	TOptional<TFlockResult<FString>> Duo;
	TOptional<TFlockResult<FString>> Solo;
	F.FindInto(TEXT("Duo"), Duo);
	F.FindInto(TEXT("Solo"), Solo);
	TestEqual(TEXT("one read for both"), F.Names->GetReadCountForTesting(), 1);
	TestFalse(TEXT("nobody is answered before the read"), Duo.IsSet() || Solo.IsSet());

	F.Reads->Answer({ Queue(TEXT("q-duo"), TEXT("Duo")), Queue(TEXT("q-solo"), TEXT("Solo")) });
	TestEqualSensitive(TEXT("the first gets its queue"), Duo.IsSet() ? Duo->Value : FString(), FString(TEXT("q-duo")));
	TestEqualSensitive(TEXT("the second gets its own"), Solo.IsSet() ? Solo->Value : FString(), FString(TEXT("q-solo")));

	// Kept for the launch: a name in the list is answered at once, with no read.
	const TOptional<TFlockResult<FString>> Again = F.Find(TEXT("Duo"));
	TestTrue(TEXT("answered at once from the list kept"), Again.IsSet() && Again->bSuccess);
	TestEqual(TEXT("no second read"), F.Names->GetReadCountForTesting(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockQueueNamesMissTest, "Flock.Multiplayer.QueueNames.ReadsAgainForANameNotInTheList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockQueueNamesMissTest::RunTest(const FString& Parameters)
{
	FQueueNamesFixture F;
	TOptional<TFlockResult<FString>> First;
	F.FindInto(TEXT("Duo"), First);
	F.Reads->Answer({ Queue(TEXT("q-duo"), TEXT("Duo")) });

	// A queue made in the dashboard after the first read.
	TOptional<TFlockResult<FString>> Newer;
	F.FindInto(TEXT("Squad"), Newer);
	TestEqual(TEXT("a name not in the list reads it again"), F.Names->GetReadCountForTesting(), 2);
	F.Reads->Answer({ Queue(TEXT("q-duo"), TEXT("Duo")), Queue(TEXT("q-squad"), TEXT("Squad")) });
	TestEqualSensitive(TEXT("and finds the newer queue"), Newer.IsSet() ? Newer->Value : FString(), FString(TEXT("q-squad")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockQueueNamesOldestTest, "Flock.Multiplayer.QueueNames.TwoQueuesOfOneNameUseTheOlder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockQueueNamesOldestTest::RunTest(const FString& Parameters)
{
	FQueueNamesFixture F;
	TOptional<TFlockResult<FString>> Found;
	F.FindInto(TEXT("Duo"), Found);
	// The list comes oldest first.
	F.Reads->Answer({ Queue(TEXT("q-old"), TEXT("Duo")), Queue(TEXT("q-new"), TEXT("Duo")), Queue(TEXT("q-lower"), TEXT("duo")) });
	TestEqualSensitive(TEXT("the older queue"), Found.IsSet() ? Found->Value : FString(), FString(TEXT("q-old")));
	TestEqual(TEXT("one warning"), F.Log->Warnings.Num(), 1);
	TestTrue(TEXT("naming both queues"), FFlockRecordingLogger::AnyContains(F.Log->Warnings, TEXT("q-new")));

	F.Find(TEXT("Duo"));
	TestEqual(TEXT("not warned again"), F.Log->Warnings.Num(), 1);

	const TOptional<TFlockResult<FString>> Lower = F.Find(TEXT("duo"));
	TestEqualSensitive(TEXT("a name differing in letter case is its own queue"), Lower.IsSet() ? Lower->Value : FString(), FString(TEXT("q-lower")));
	TestEqual(TEXT("and has only one queue, so no warning"), F.Log->Warnings.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockQueueNamesNotFoundTest, "Flock.Multiplayer.QueueNames.AMissingNameSaysWhatToFix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockQueueNamesNotFoundTest::RunTest(const FString& Parameters)
{
	auto Refusal = [](const TArray<FFlockMatchmakingQueue>& List, const FString& Name)
	{
		FQueueNamesFixture F;
		TOptional<TFlockResult<FString>> Found;
		F.FindInto(Name, Found);
		F.Reads->Answer(List);
		return Found.IsSet() ? Found->Error : FFlockError();
	};

	const FFlockError OtherCase = Refusal({ Queue(TEXT("q-duo"), TEXT("Duo")) }, TEXT("duo"));
	TestEqual(TEXT("a validation failure"), static_cast<int32>(OtherCase.Type), static_cast<int32>(EFlockErrorType::Validation));
	TestEqualSensitive(TEXT("coded as the backend codes it"), OtherCase.Code, FString(TEXT("matchmaking.queue_not_found")));
	TestEqual(TEXT("typed"), static_cast<int32>(OtherCase.ErrorCode), static_cast<int32>(EFlockErrorCode::MatchmakingQueueNotFound));
	TestTrue(TEXT("naming the name asked for"), OtherCase.Message.Contains(TEXT("\"duo\""), ESearchCase::CaseSensitive));
	TestTrue(TEXT("and the queue that differs only in letter case"), OtherCase.Hint.Contains(TEXT("\"Duo\""), ESearchCase::CaseSensitive));
	TestEqualSensitive(TEXT("as the call that failed"), OtherCase.Operation, FString(TEXT("Find matchmaking queue")));

	TArray<FFlockMatchmakingQueue> Full;
	for (int32 Index = 0; Index < FFlockMatchmakingQueueNames::MostQueuesListed; ++Index)
	{
		Full.Add(Queue(FString::Printf(TEXT("q-%d"), Index), FString::Printf(TEXT("Queue %d"), Index)));
	}
	const FFlockError FullList = Refusal(Full, TEXT("Newest"));
	TestTrue(TEXT("a full list says a newer queue may be missing from it"), FullList.Hint.Contains(TEXT("200 oldest"), ESearchCase::CaseSensitive));

	const FFlockError Plain = Refusal({ Queue(TEXT("q-duo"), TEXT("Duo")) }, TEXT("Squad"));
	TestTrue(TEXT("otherwise it says to make the queue"), Plain.Hint.Contains(TEXT("Create a queue"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("and does not mention the list's length"), Plain.Hint.Contains(TEXT("200"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockQueueNamesGiveUpTest, "Flock.Multiplayer.QueueNames.GivingUpEndsTheWaitAtOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockQueueNamesGiveUpTest::RunTest(const FString& Parameters)
{
	FQueueNamesFixture F;
	TOptional<TFlockResult<FString>> GaveUp;
	TOptional<TFlockResult<FString>> Stayed;
	FFlockRequestHandle Handle;
	F.FindInto(TEXT("Duo"), GaveUp, &Handle);
	F.FindInto(TEXT("Duo"), Stayed);
	Handle.Cancel();
	TestTrue(TEXT("answered at once"), GaveUp.IsSet());
	TestEqual(TEXT("as cancelled"), GaveUp.IsSet() ? static_cast<int32>(GaveUp->Error.Type) : -1, static_cast<int32>(EFlockErrorType::Cancelled));

	F.Reads->Answer({ Queue(TEXT("q-duo"), TEXT("Duo")) });
	TestEqual(TEXT("not answered a second time"), GaveUp.IsSet() ? static_cast<int32>(GaveUp->Error.Type) : -1, static_cast<int32>(EFlockErrorType::Cancelled));
	TestTrue(TEXT("whoever else waited is answered"), Stayed.IsSet() && Stayed->bSuccess);
	const TOptional<TFlockResult<FString>> Later = F.Find(TEXT("Duo"));
	TestTrue(TEXT("and the read is kept"), Later.IsSet() && Later->bSuccess);
	TestEqual(TEXT("with one read in all"), F.Names->GetReadCountForTesting(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockQueueNamesFailedReadTest, "Flock.Multiplayer.QueueNames.AFailedReadIsNotKept",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockQueueNamesFailedReadTest::RunTest(const FString& Parameters)
{
	FQueueNamesFixture F;
	TOptional<TFlockResult<FString>> First;
	F.FindInto(TEXT("Duo"), First);
	F.Reads->Reply(TFlockResult<TArray<FFlockMatchmakingQueue>>::Fail(FFlockError::Make(EFlockErrorType::Connection, TEXT("offline"))));
	TestEqual(TEXT("the read's own failure is the answer"), First.IsSet() ? static_cast<int32>(First->Error.Type) : -1,
		static_cast<int32>(EFlockErrorType::Connection));

	TOptional<TFlockResult<FString>> Second;
	F.FindInto(TEXT("Duo"), Second);
	TestEqual(TEXT("the next lookup reads again"), F.Names->GetReadCountForTesting(), 2);
	F.Reads->Answer({ Queue(TEXT("q-duo"), TEXT("Duo")) });
	TestTrue(TEXT("and finds it"), Second.IsSet() && Second->bSuccess);

	// A read that fails later leaves the list read before it in place.
	TOptional<TFlockResult<FString>> Newer;
	F.FindInto(TEXT("Squad"), Newer);
	F.Reads->Reply(TFlockResult<TArray<FFlockMatchmakingQueue>>::Fail(FFlockError::Make(EFlockErrorType::Connection, TEXT("offline"))));
	TestEqual(TEXT("a name the failed read was for gets its failure"), Newer.IsSet() ? static_cast<int32>(Newer->Error.Type) : -1,
		static_cast<int32>(EFlockErrorType::Connection));
	const TOptional<TFlockResult<FString>> Kept = F.Find(TEXT("Duo"));
	TestTrue(TEXT("a queue in the list read before is still answered at once"), Kept.IsSet() && Kept->bSuccess);
	TestEqual(TEXT("with no read of its own"), F.Names->GetReadCountForTesting(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockQueueNamesShutdownTest, "Flock.Multiplayer.QueueNames.ShutdownAnswersEveryWait",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockQueueNamesShutdownTest::RunTest(const FString& Parameters)
{
	for (const bool bShutDownMeanwhile : { false, true })
	{
		const FString Case = bShutDownMeanwhile ? TEXT("shut down while reading") : TEXT("control");
		FMultiplayerProviderFixture F;
		F.Fake->On(TEXT("matchmaking/queues"), FFlockFakeTransport::Ok(QueueListBody({ Queue(TEXT("q-duo"), TEXT("Duo")) })));
		F.Fake->bDeferred = true;
		int32 Answers = 0;
		EFlockErrorType Type = EFlockErrorType::None;
		F.Provider->FindQueueId(TEXT("Duo"), [&](TFlockResult<FString> Result)
		{
			++Answers;
			Type = Result.bSuccess ? EFlockErrorType::None : Result.Error.Type;
		});
		if (bShutDownMeanwhile)
		{
			F.Provider->Shutdown();
			TestEqual(FString::Printf(TEXT("%s: answered at once"), *Case), Answers, 1);
			TestEqual(FString::Printf(TEXT("%s: as cancelled"), *Case), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Cancelled));
		}
		F.Fake->FlushPending();
		TestEqual(FString::Printf(TEXT("%s: answered once in all"), *Case), Answers, 1);
		if (!bShutDownMeanwhile)
		{
			TestEqual(FString::Printf(TEXT("%s: with the queue"), *Case), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::None));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockQueueNamesWireTest, "Flock.Multiplayer.QueueNames.ReadsTheGamesListWithTheKeyAlone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockQueueNamesWireTest::RunTest(const FString& Parameters)
{
	FMultiplayerProviderFixture F;
	FString Error;
	F.Session->SetTokens(QueueTestJwt(TEXT("player-a")), TEXT("r-1"), Error);
	F.Fake->On(TEXT("matchmaking/queues"), FFlockFakeTransport::Ok(QueueListBody({ Queue(TEXT("q-duo"), TEXT("Duo")) })));

	TOptional<TFlockResult<FString>> Found;
	F.Provider->FindQueueId(TEXT("Duo"), [&Found](TFlockResult<FString> Result) { Found = Result; });
	TestEqualSensitive(TEXT("found through the enveloped list"), Found.IsSet() ? Found->Value : FString(), FString(TEXT("q-duo")));
	if (TestEqual(TEXT("one request"), F.Fake->Requests.Num(), 1))
	{
		const FFlockHttpRequest& Request = FlockTestAt(F.Fake->Requests, 0);
		TestEqualSensitive(TEXT("the whole list in one page"), Request.Url, FString(TEXT("http://x/v1/matchmaking/queues?limit=200")));
		TestEqualSensitive(TEXT("with the API key"), Request.Headers.FindRef(TEXT("X-Flock-API-Key")), FString(TEXT("k")));
		TestFalse(TEXT("and no player's token, signed in or not"), Request.Headers.Contains(TEXT("Authorization")));
	}

	// A refused key is the answer: there is no sign-in to refresh for a game-wide list.
	FMultiplayerProviderFixture Refused;
	Refused.Session->SetTokens(QueueTestJwt(TEXT("player-a")), TEXT("r-1"), Error);
	Refused.Fake->On(TEXT("matchmaking/queues"), FFlockFakeTransport::Status(401, TEXT("{\"detail\":\"Invalid API key\"}")));
	TOptional<TFlockResult<FString>> NotRead;
	Refused.Provider->FindQueueId(TEXT("Duo"), [&NotRead](TFlockResult<FString> Result) { NotRead = Result; });
	TestEqual(TEXT("a 401 is not refreshed"), Refused.Fake->CountTo(TEXT("token/refresh")), 0);
	TestEqual(TEXT("and is the answer"), NotRead.IsSet() ? NotRead->Error.StatusCode : 0, 401);
	TestTrue(TEXT("the player stays signed in"), Refused.Session->IsAuthenticated());

	// No name is refused before anything is sent.
	FMultiplayerProviderFixture Empty;
	TOptional<TFlockResult<FString>> NoName;
	Empty.Provider->FindQueueId(FString(), [&NoName](TFlockResult<FString> Result) { NoName = Result; });
	TestEqual(TEXT("no name: a validation failure"), NoName.IsSet() ? static_cast<int32>(NoName->Error.Type) : -1,
		static_cast<int32>(EFlockErrorType::Validation));
	TestEqual(TEXT("no name: nothing sent"), Empty.Fake->Requests.Num(), 0);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
