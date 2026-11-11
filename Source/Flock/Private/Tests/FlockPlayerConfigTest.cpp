// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "Containers/Ticker.h"
#include "FlockLogger.h"
#include "HAL/FileManager.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockSnapshotStore.h"
#include "Misc/Base64.h"
#include "Misc/FlockEngineCompat.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Providers/FlockConfigProvider.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockRecordingLogger.h"

// A signed-in player's config reads come back as the server resolves them for that player (segment and player overrides); with
// nobody signed in the game's read is unchanged. The player's and the game's answers are the local backend's own, captured
// verbatim (Owner-41's live run): A is in a segment with an override (lives 7) and has one of their own (enemy_speed 2.5); B has
// neither. The game has no patch, so the patch answer is built to the patch schema.
namespace FlockPlayerConfigTestHelpers
{
	const TCHAR* const ConfigId = TEXT("01M74VYNA2ZCPX6TY2W9HSTB3Y");
	const TCHAR* const PlayerRoute = TEXT("game_config/me/by-name/o41_difficulty");
	const TCHAR* const ConfigByIdRoute = TEXT("game_config/01M74VYNA2ZCPX6TY2W9HSTB3Y");
	const TCHAR* const PatchesRoute = TEXT("game_patch/config/01M74VYNA2ZCPX6TY2W9HSTB3Y");

	constexpr int32 PatchLives = 4;
	constexpr int32 ALives = 7;
	constexpr int32 BLives = 3;

	const TCHAR* const PlayerAAnswer = TEXT("{\"error\":{\"code\":null},\"response\":{\"message\":null,\"code\":null},\"result\":{\"id\":\"01M74VYNA2ZCPX6TY2W9HSTB3Y\",\"name\":\"o41_difficulty\",\"game_id\":\"01M5EM6PTHKSYB8ZFPSSRAJ7C2\",\"game_version_id\":\"01M5EM6PTY4M5PA1A93GQXQYDP\",\"schema\":[{\"type\":\"integer\",\"type_name\":\"difficulty\",\"field_name\":\"lives\"},{\"type\":\"float\",\"type_name\":\"difficulty\",\"field_name\":\"enemy_speed\"}],\"data\":[{\"type\":\"integer\",\"field_name\":\"lives\",\"type_name\":\"difficulty\",\"value\":7},{\"type\":\"float\",\"field_name\":\"enemy_speed\",\"type_name\":\"difficulty\",\"value\":2.5}],\"tag\":\"gameplay\",\"created_at\":\"2026-11-11T08:35:22.050969Z\",\"updated_at\":\"2026-11-11T08:35:22.050974Z\",\"patch_id\":null,\"override_ids\":[\"01M74VYQXNJFBNQDFPEM610DBZ\",\"01M74VYQYB469PKNGQQPPVDYV0\"]}}");
	const TCHAR* const PlayerBAnswer = TEXT("{\"error\":{\"code\":null},\"response\":{\"message\":null,\"code\":null},\"result\":{\"id\":\"01M74VYNA2ZCPX6TY2W9HSTB3Y\",\"name\":\"o41_difficulty\",\"game_id\":\"01M5EM6PTHKSYB8ZFPSSRAJ7C2\",\"game_version_id\":\"01M5EM6PTY4M5PA1A93GQXQYDP\",\"schema\":[{\"type\":\"integer\",\"type_name\":\"difficulty\",\"field_name\":\"lives\"},{\"type\":\"float\",\"type_name\":\"difficulty\",\"field_name\":\"enemy_speed\"}],\"data\":[{\"type\":\"integer\",\"value\":3,\"type_name\":\"difficulty\",\"field_name\":\"lives\"},{\"type\":\"float\",\"value\":1.5,\"type_name\":\"difficulty\",\"field_name\":\"enemy_speed\"}],\"tag\":\"gameplay\",\"created_at\":\"2026-11-11T08:35:22.050969Z\",\"updated_at\":\"2026-11-11T08:35:22.050974Z\",\"patch_id\":null,\"override_ids\":[]}}");
	const TCHAR* const ConfigBody = TEXT("{\"error\":{\"code\":null},\"response\":{\"message\":null,\"code\":null},\"result\":{\"id\":\"01M74VYNA2ZCPX6TY2W9HSTB3Y\",\"name\":\"o41_difficulty\",\"game_id\":\"01M5EM6PTHKSYB8ZFPSSRAJ7C2\",\"game_version_id\":\"01M5EM6PTY4M5PA1A93GQXQYDP\",\"schema\":[{\"type\":\"integer\",\"type_name\":\"difficulty\",\"field_name\":\"lives\"},{\"type\":\"float\",\"type_name\":\"difficulty\",\"field_name\":\"enemy_speed\"}],\"data\":[{\"type\":\"integer\",\"value\":3,\"type_name\":\"difficulty\",\"field_name\":\"lives\"},{\"type\":\"float\",\"value\":1.5,\"type_name\":\"difficulty\",\"field_name\":\"enemy_speed\"}],\"tag\":\"gameplay\",\"created_at\":\"2026-11-11T08:35:22.050969Z\",\"updated_at\":\"2026-11-11T08:35:22.050974Z\"}}");
	const TCHAR* const PatchesBody = TEXT("{\"error\":{\"code\":null},\"response\":{\"message\":null,\"code\":null},\"result\":[{\"id\":\"patch-1\",\"name\":\"November balance\",")
		TEXT("\"game_config_id\":\"01M74VYNA2ZCPX6TY2W9HSTB3Y\",\"data\":[{\"type\":\"integer\",\"value\":4,\"type_name\":\"difficulty\",\"field_name\":\"lives\"},")
		TEXT("{\"type\":\"float\",\"value\":1.5,\"type_name\":\"difficulty\",\"field_name\":\"enemy_speed\"}],")
		TEXT("\"created_at\":\"2026-11-06T10:00:00.000000Z\",\"updated_at\":\"2026-11-06T10:00:00.000000Z\"}]}");

	inline FString MakeJwt(const FString& PlayerId, int32 Variant)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + 3600 + Variant;
		FString Payload = FBase64::Encode(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp));
		Payload.ReplaceInline(TEXT("+"), TEXT("-"));
		Payload.ReplaceInline(TEXT("/"), TEXT("_"));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Payload);
	}

	const FString TokenA = MakeJwt(TEXT("player-a"), 1);
	const FString TokenB = MakeJwt(TEXT("player-b"), 2);

	inline FFlockHttpResponse Answer(int32 StatusCode, const FString& Body)
	{
		FFlockHttpResponse Response;
		Response.Result = EFlockHttpResult::Success;
		Response.StatusCode = StatusCode;
		Response.Body = Body;
		return Response;
	}

	inline FFlockHttpResponse Coded(int32 StatusCode, const FString& Code)
	{
		return Answer(StatusCode, FString::Printf(TEXT("{\"detail\":{\"code\":\"%s\",\"message\":\"test\"}}"), *Code));
	}

	/**
	 * The game's routes by URL fragment (first registered match, the last of a sequence sticking); the player's route by the
	 * bearer it carries unless a route is registered for it. One request matching a held fragment waits for ReleaseHeld.
	 */
	class FPlayerConfigServer : public IFlockHttpAdapter
	{
	public:
		TArray<FFlockHttpRequest> Requests;
		bool bOffline = false;

		FPlayerConfigServer& On(const FString& Fragment, const TArray<FFlockHttpResponse>& Answers)
		{
			for (FRoute& Route : Routes)
			{
				if (Route.Fragment.Equals(Fragment, ESearchCase::CaseSensitive))
				{
					Route.Answers = Answers;
					Route.Next = 0;
					return *this;
				}
			}
			Routes.Add({ Fragment, Answers, 0 });
			return *this;
		}

		void HoldNext(const FString& Fragment) { HeldFragment = Fragment; }

		bool ReleaseHeld()
		{
			const TFunction<void()> Run = MoveTemp(Held);
			Held = nullptr;
			if (!Run)
			{
				return false;
			}
			Run();
			return true;
		}

		int32 CountTo(const FString& Fragment) const
		{
			int32 Count = 0;
			for (const FFlockHttpRequest& Request : Requests)
			{
				Count += Request.Url.Contains(Fragment, ESearchCase::CaseSensitive) ? 1 : 0;
			}
			return Count;
		}

		/** The requests to Fragment, in order. */
		TArray<FFlockHttpRequest> AllTo(const FString& Fragment) const
		{
			return Requests.FilterByPredicate([&Fragment](const FFlockHttpRequest& Request) { return Request.Url.Contains(Fragment, ESearchCase::CaseSensitive); });
		}

		virtual FFlockRequestHandle SendAsync(const FFlockHttpRequest& Request, TFunction<void(FFlockHttpResponse)> OnComplete) override
		{
			Requests.Add(Request);
			const FFlockHttpResponse Response = AnswerTo(Request);
			if (!HeldFragment.IsEmpty() && Request.Url.Contains(HeldFragment, ESearchCase::CaseSensitive))
			{
				HeldFragment.Reset();
				Held = [OnComplete, Response]() { if (OnComplete) { OnComplete(Response); } };
				return FFlockRequestHandle();
			}
			if (OnComplete)
			{
				OnComplete(Response);
			}
			return FFlockRequestHandle();
		}

	private:
		struct FRoute
		{
			FString Fragment;
			TArray<FFlockHttpResponse> Answers;
			int32 Next = 0;
		};

		FFlockHttpResponse AnswerTo(const FFlockHttpRequest& Request)
		{
			if (bOffline)
			{
				FFlockHttpResponse Unreachable;
				Unreachable.Result = EFlockHttpResult::ConnectionError;
				return Unreachable;
			}
			for (FRoute& Route : Routes)
			{
				if (Request.Url.Contains(Route.Fragment, ESearchCase::CaseSensitive) && Route.Answers.Num() > 0)
				{
					const int32 Index = FMath::Min(Route.Next, Route.Answers.Num() - 1);
					Route.Next = Index + 1;
					return Route.Answers[Index];
				}
			}
			if (Request.Url.Contains(TEXT("game_config/me/by-name/"), ESearchCase::CaseSensitive))
			{
				const FString* Bearer = Request.Headers.Find(TEXT("Authorization"));
				if (Bearer != nullptr && Bearer->Equals(TEXT("Bearer ") + TokenA, ESearchCase::CaseSensitive))
				{
					return Answer(200, PlayerAAnswer);
				}
				if (Bearer != nullptr && Bearer->Equals(TEXT("Bearer ") + TokenB, ESearchCase::CaseSensitive))
				{
					return Answer(200, PlayerBAnswer);
				}
				return Coded(401, TEXT("player.missing_token"));
			}
			return Answer(200, TEXT("{}"));
		}

		TArray<FRoute> Routes;
		FString HeldFragment;
		TFunction<void()> Held;
	};

	inline FFlockRetryPolicy Retries(int32 MaxRetries)
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = MaxRetries;
		Policy.InitialDelaySeconds = 0.f;
		return Policy;
	}

	struct FFixture
	{
		FString Dir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("player_config_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		TSharedRef<FPlayerConfigServer> Server = MakeShared<FPlayerConfigServer>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockAuthSession> Session;
		TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
		TSharedRef<bool> bReachable = MakeShared<bool>(true);
		TSharedPtr<FFlockSnapshotStore> Snapshot;
		TSharedPtr<FFlockConfigProvider> Provider;

		explicit FFixture(int32 MaxRetries = 0)
			: Client(MakeShared<FFlockHttpClient>(Server, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, MakeShared<FFlockMemoryTokenStore>(), MakeShared<FFlockNullLogger>(),
				TEXT("https://test.invalid/v1"),
				TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("test-key") }, { TEXT("X-Game-Version-ID"), TEXT("test-gvid") } }))
		{
			Server->On(PatchesRoute, { Answer(200, PatchesBody) });
			Server->On(ConfigByIdRoute, { Answer(200, ConfigBody) });
			Snapshot = MakeShared<FFlockSnapshotStore>(Dir, MakeShared<FFlockNullLogger>(), TEXT("9.9.9"));
			Provider = MakeShared<FFlockConfigProvider>(Client, Retries(MaxRetries), Log, Session, TEXT("https://test.invalid/v1"),
				Snapshot, TEXT("test-gvid"));
			const TSharedRef<bool> Reachable = bReachable;
			Provider->SetReachabilityProbe([Reachable]() { return *Reachable; });
		}

		~FFixture()
		{
			IFileManager::Get().DeleteDirectory(*Dir, /*bRequireExists*/ false, /*bTree*/ true);
		}

		/** A new sign-in, even for the same player. */
		void SignIn(const FString& Token)
		{
			FString Error;
			Session->SetTokens(Token, TEXT("refresh"), Error);
		}

		void GoOffline()
		{
			*bReachable = false;
			Server->bOffline = true;
		}

		void GoOnline()
		{
			*bReachable = true;
			Server->bOffline = false;
		}

		/** The lives a config read gives; -1 when it failed (its error in OutError) or has not finished. */
		int32 Read(const FString& Id = ConfigId, FFlockError* OutError = nullptr)
		{
			int32 Lives = -1;
			Provider->ResolveConfigData(Id, [&Lives, OutError](TFlockResult<FFlockStructuredData> Result)
			{
				if (Result.bSuccess)
				{
					Result.Value.TryGetInt(TEXT("lives"), Lives);
				}
				else if (OutError != nullptr)
				{
					*OutError = Result.Error;
				}
			});
			return Lives;
		}

		int32 WarningsAbout(const FString& Fragment) const
		{
			int32 Count = 0;
			for (const FString& Line : Log->Warnings)
			{
				Count += Line.Contains(Fragment, ESearchCase::CaseSensitive) ? 1 : 0;
			}
			return Count;
		}
	};

	inline void TickRetries()
	{
		for (int32 Index = 0; Index < 8; ++Index)
		{
			FTSTicker::GetCoreTicker().Tick(1.f);
		}
	}

	inline FString HeaderOf(const FFlockHttpRequest& Request, const FString& Name)
	{
		const FString* Value = Request.Headers.Find(Name);
		return Value != nullptr ? *Value : FString();
	}
}

using namespace FlockPlayerConfigTestHelpers;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigSignedInTest, "Flock.Config.Player.SignedIn_ReadsThePlayersRoute_AndGetsTheirOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigSignedInTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TokenA);

	TestEqual(TEXT("the player's override, not the patch's value"), F.Read(), ALives);

	const TArray<FFlockHttpRequest> Sent = F.Server->AllTo(PlayerRoute);
	if (TestEqual(TEXT("the player's route was read"), Sent.Num(), 1))
	{
		const FFlockHttpRequest& Request = Sent[0];
		TestEqualSensitive(TEXT("a read"), Request.Method, FString(TEXT("GET")));
		TestEqualSensitive(TEXT("read by the config's name, which its id gave"), Request.Url, FString(TEXT("https://test.invalid/v1/")) + PlayerRoute);
		TestEqualSensitive(TEXT("as the player"), HeaderOf(Request, TEXT("Authorization")), TEXT("Bearer ") + TokenA);
		TestEqualSensitive(TEXT("the build's game version names the version read"), HeaderOf(Request, TEXT("X-Game-Version-ID")), FString(TEXT("test-gvid")));
		TestEqualSensitive(TEXT("the game's key"), HeaderOf(Request, TEXT("X-Flock-API-Key")), FString(TEXT("test-key")));
	}
	TestEqual(TEXT("the game's patch read is not needed"), F.Server->CountTo(PatchesRoute), 0);

	TestEqual(TEXT("again"), F.Read(), ALives);
	TestEqual(TEXT("kept for the sign-in: a second read sends nothing"), F.Server->CountTo(PlayerRoute), 1);
	TestEqual(TEXT("the name is asked once"), F.Server->CountTo(ConfigByIdRoute), 1);
	TestEqual(TEXT("nothing logged as an error"), F.Log->Errors.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigNobodyTest, "Flock.Config.Player.NobodySignedIn_ReadsTheGamesRoutes_AsBefore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigNobodyTest::RunTest(const FString& Parameters)
{
	FFixture F;
	TestEqual(TEXT("the patch's value"), F.Read(), PatchLives);

	TestEqual(TEXT("no player route without a player"), F.Server->CountTo(TEXT("game_config/me/")), 0);
	TestEqual(TEXT("a config with a patch needs no read of the config itself"), F.Server->CountTo(ConfigByIdRoute), 0);
	const TArray<FFlockHttpRequest> Sent = F.Server->AllTo(PatchesRoute);
	if (TestEqual(TEXT("the patches were read"), Sent.Num(), 1))
	{
		TestEqualSensitive(TEXT("the game's route"), Sent[0].Url, FString(TEXT("https://test.invalid/v1/")) + PatchesRoute);
		TestFalse(TEXT("the game's read carries no sign-in"), Sent[0].Headers.Contains(TEXT("Authorization")));
		TestEqualSensitive(TEXT("the game version"), HeaderOf(Sent[0], TEXT("X-Game-Version-ID")), FString(TEXT("test-gvid")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigSwitchTest, "Flock.Config.Player.ASwitchFromAToB_NeverServesAsValues_FromMemoryOrDisk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigSwitchTest::RunTest(const FString& Parameters)
{
	FFixture F;
	TestEqual(TEXT("signed out first, so the game's values are on disk"), F.Read(), PatchLives);

	F.SignIn(TokenA);
	TestEqual(TEXT("A's"), F.Read(), ALives);

	F.SignIn(TokenB);
	F.GoOffline();
	const int32 SentBefore = F.Server->Requests.Num();
	TestEqual(TEXT("B offline with no copy of their own reads the game's values, never A's"), F.Read(), PatchLives);
	TestEqual(TEXT("offline: nothing sent"), F.Server->Requests.Num(), SentBefore);

	F.GoOnline();
	TestEqual(TEXT("B online reads their own, not A's held value"), F.Read(), BLives);
	const TArray<FFlockHttpRequest> Sent = F.Server->AllTo(PlayerRoute);
	TestTrue(TEXT("as B"), Sent.Num() > 0 && HeaderOf(Sent.Last(), TEXT("Authorization")).Equals(TEXT("Bearer ") + TokenB, ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigOfflineCopyTest, "Flock.Config.Player.OfflineSignedIn_ServesThePlayersOwnCopy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigOfflineCopyTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TokenA);
	TestEqual(TEXT("A's"), F.Read(), ALives);

	// A new sign-in holds nothing in memory, so what answers offline is the copy on disk.
	F.SignIn(TokenA);
	F.GoOffline();
	const int32 SentBefore = F.Server->Requests.Num();
	TestEqual(TEXT("A's own copy"), F.Read(), ALives);
	TestEqual(TEXT("offline: nothing sent"), F.Server->Requests.Num(), SentBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigLateAnswerTest, "Flock.Config.Player.ALateAnswerForA_IsKeptNowhere_AndTheReadAnswersForB",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigLateAnswerTest::RunTest(const FString& Parameters)
{
	FFixture F;
	TestEqual(TEXT("signed out first, so the game's values are on disk"), F.Read(), PatchLives);

	F.SignIn(TokenA);
	F.Server->HoldNext(PlayerRoute);
	int32 LateLives = -1;
	F.Provider->ResolveConfigData(ConfigId, [&LateLives](TFlockResult<FFlockStructuredData> Result)
	{
		if (Result.bSuccess)
		{
			Result.Value.TryGetInt(TEXT("lives"), LateLives);
		}
	});
	TestEqual(TEXT("A's read waits for its answer"), LateLives, -1);
	F.SignIn(TokenB);
	TestTrue(TEXT("A's answer is let through"), F.Server->ReleaseHeld());

	TestEqual(TEXT("A's answer landed after B signed in, so the read answers for B"), LateLives, BLives);
	const TArray<FFlockHttpRequest> Sent = F.Server->AllTo(PlayerRoute);
	if (TestEqual(TEXT("A's read, then B's"), Sent.Num(), 2))
	{
		TestTrue(TEXT("the first as A"), HeaderOf(Sent[0], TEXT("Authorization")).Equals(TEXT("Bearer ") + TokenA, ESearchCase::CaseSensitive));
		TestTrue(TEXT("the second as B"), HeaderOf(Sent[1], TEXT("Authorization")).Equals(TEXT("Bearer ") + TokenB, ESearchCase::CaseSensitive));
	}

	F.SignIn(TokenA);
	F.GoOffline();
	TestEqual(TEXT("A's late answer was never written to A's copy on disk"), F.Read(), PatchLives);
	return true;
}

namespace FlockPlayerConfigTestHelpers
{
	/** A fresh client with retries on and no copy of the player's; the first try is held, then fails. Outcome: player reads sent after the held one, lives. */
	inline TPair<int32, int32> ReadWithAFailedFirstTry(bool bSignOutWhileHeld)
	{
		FFixture F(/*MaxRetries*/ 2);
		F.Server->On(PlayerRoute, { Answer(503, TEXT("{}")), Answer(200, PlayerAAnswer) });
		F.SignIn(TokenA);
		F.Server->HoldNext(PlayerRoute);
		int32 Lives = -1;
		F.Provider->ResolveConfigData(ConfigId, [&Lives](TFlockResult<FFlockStructuredData> Result)
		{
			if (Result.bSuccess)
			{
				Result.Value.TryGetInt(TEXT("lives"), Lives);
			}
		});
		if (bSignOutWhileHeld)
		{
			F.Session->ClearTokens();
		}
		const int32 Held = F.Server->CountTo(PlayerRoute);
		F.Server->ReleaseHeld();
		TickRetries();
		return TPair<int32, int32>(F.Server->CountTo(PlayerRoute) - Held, Lives);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigRetriesTest, "Flock.Config.Player.RetriesStop_OnceThePlayerSignsOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigRetriesTest::RunTest(const FString& Parameters)
{
	const TPair<int32, int32> Control = ReadWithAFailedFirstTry(false);
	TestEqual(TEXT("control: the same failure with the player still signed in is retried"), Control.Key, 1);
	TestEqual(TEXT("control: and the retry answers"), Control.Value, ALives);

	const TPair<int32, int32> SignedOut = ReadWithAFailedFirstTry(true);
	TestEqual(TEXT("nothing re-sent for the player once they signed out"), SignedOut.Key, 0);
	TestEqual(TEXT("the read answers as for nobody signed in"), SignedOut.Value, PatchLives);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigRefusalTest, "Flock.Config.Player.ARefusalOfThePlayersRead_FallsBackToTheGamesRead_ForTheSignIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigRefusalTest::RunTest(const FString& Parameters)
{
	const TCHAR* const Codes[] = { TEXT("game_config.player_no_game_version"), TEXT("game_config.player_not_found"), TEXT("game_config.config_not_found") };
	for (const TCHAR* Code : Codes)
	{
		FFixture F;
		F.Server->On(PlayerRoute, { Coded(404, Code) });
		// No pause after no answer, so "not asked again" can only be the refusal held for the sign-in.
		F.Provider->SetPlayerReadPauseAfterNoAnswerForTesting(0.0);
		F.SignIn(TokenA);

		TestEqual(FString::Printf(TEXT("[%s] the game's values, as before"), Code), F.Read(), PatchLives);
		TestEqual(FString::Printf(TEXT("[%s] again"), Code), F.Read(), PatchLives);
		TestEqual(FString::Printf(TEXT("[%s] not asked again during this sign-in"), Code), F.Server->CountTo(PlayerRoute), 1);
		TestEqual(FString::Printf(TEXT("[%s] the game's read is kept as before"), Code), F.Server->CountTo(PatchesRoute), 1);
		TestEqual(FString::Printf(TEXT("[%s] a fallback is no error"), Code), F.Log->Errors.Num(), 0);

		F.SignIn(TokenA);
		F.Read();
		TestEqual(FString::Printf(TEXT("[%s] a new sign-in asks again"), Code), F.Server->CountTo(PlayerRoute), 2);
	}
	return true;
}

// Only "no Game Version" is something the developer can fix in the build, so it alone warns, once a sign-in however many configs fall back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigWarningTest, "Flock.Config.Player.OnlyNoGameVersion_WarnsOncePerSignIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigWarningTest::RunTest(const FString& Parameters)
{
	const TPair<const TCHAR*, int32> Cases[] = {
		{ TEXT("game_config.player_no_game_version"), 1 }, { TEXT("game_config.player_not_found"), 0 }, { TEXT("game_config.config_not_found"), 0 } };
	for (const TPair<const TCHAR*, int32>& Case : Cases)
	{
		const FString Code = Case.Key;
		const int32 WarningsPerSignIn = Case.Value;
		FFixture F;
		F.Server->On(TEXT("game_config/me/by-name/"), { Coded(404, Code) });
		F.Server->On(TEXT("game_patch/config/cfg-speed"), { Answer(200, FString(PatchesBody).Replace(ConfigId, TEXT("cfg-speed"))) });
		F.Server->On(TEXT("game_config/cfg-speed"),
			{ Answer(200, FString(ConfigBody).Replace(ConfigId, TEXT("cfg-speed")).Replace(TEXT("\"o41_difficulty\""), TEXT("\"o41_speed\""))) });
		F.SignIn(TokenA);
		TestEqual(FString::Printf(TEXT("[%s] the game's values"), *Code), F.Read(), PatchLives);
		TestEqual(FString::Printf(TEXT("[%s] the other config's too"), *Code), F.Read(TEXT("cfg-speed")), PatchLives);
		TestEqual(FString::Printf(TEXT("[%s] two configs fell back"), *Code), F.Server->CountTo(TEXT("game_config/me/by-name/")), 2);
		TestEqual(FString::Printf(TEXT("[%s] warned %d time(s) for the sign-in"), *Code, WarningsPerSignIn), F.WarningsAbout(TEXT("Config values")), WarningsPerSignIn);

		F.SignIn(TokenA);
		F.Read();
		TestEqual(FString::Printf(TEXT("[%s] again for the next sign-in"), *Code), F.WarningsAbout(TEXT("Config values")), 2 * WarningsPerSignIn);
		if (WarningsPerSignIn > 0)
		{
			TestEqual(TEXT("names the cause"), F.WarningsAbout(TEXT("no Game Version")), 2);
			TestEqual(TEXT("names the fix"), F.WarningsAbout(TEXT("Tools > Flock > Resolve Game Version")), 2);
		}
		TestEqual(FString::Printf(TEXT("[%s] nothing else warned"), *Code), F.Log->Warnings.Num(), 2 * WarningsPerSignIn);
	}
	return true;
}

// The server resolves only gameplay and feature configs for a player; any other comes back as stored, without its patch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigTagsTest, "Flock.Config.Player.OnlyGameplayAndFeatureConfigs_AreReadAsThePlayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigTagsTest::RunTest(const FString& Parameters)
{
	const TPair<const TCHAR*, bool> Cases[] = { { TEXT("currency"), false }, { TEXT("achievement"), false }, { TEXT("asset"), false }, { TEXT("feature"), true } };
	for (const TPair<const TCHAR*, bool>& Case : Cases)
	{
		const FString Tag = Case.Key;
		const bool bAsThePlayer = Case.Value;
		FFixture F;
		F.Server->On(ConfigByIdRoute, { Answer(200, FString(ConfigBody).Replace(TEXT("\"tag\":\"gameplay\""), *FString::Printf(TEXT("\"tag\":\"%s\""), *Tag))) });
		F.SignIn(TokenA);
		TestEqual(FString::Printf(TEXT("[%s] %s"), *Tag, bAsThePlayer ? TEXT("the player's values") : TEXT("the patch's values, as signed out")),
			F.Read(), bAsThePlayer ? ALives : PatchLives);
		F.Read();
		TestEqual(FString::Printf(TEXT("[%s] player reads"), *Tag), F.Server->CountTo(PlayerRoute), bAsThePlayer ? 1 : 0);
		TestEqual(FString::Printf(TEXT("[%s] the tag is read once"), *Tag), F.Server->CountTo(ConfigByIdRoute), 1);
	}
	return true;
}

// A codeless 404 (a backend without the route) or a 403 will not change during the sign-in, so it is not sent again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigPermanentTest, "Flock.Config.Player.APermanentFailure_IsNotSentAgainDuringTheSignIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigPermanentTest::RunTest(const FString& Parameters)
{
	for (const int32 Status : { 404, 403 })
	{
		FFixture F;
		F.Server->On(PlayerRoute, { Answer(Status, TEXT("{\"detail\":\"Not Found\"}")) });
		// No pause after no answer, so "sent once" can only be the failure held for the sign-in.
		F.Provider->SetPlayerReadPauseAfterNoAnswerForTesting(0.0);
		F.SignIn(TokenA);
		TestEqual(FString::Printf(TEXT("[%d] the game's values"), Status), F.Read(), PatchLives);
		TestEqual(FString::Printf(TEXT("[%d] again"), Status), F.Read(), PatchLives);
		TestEqual(FString::Printf(TEXT("[%d] sent once"), Status), F.Server->CountTo(PlayerRoute), 1);
		TestEqual(FString::Printf(TEXT("[%d] a fallback is no error"), Status), F.Log->Errors.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigPauseTest, "Flock.Config.Player.NoAnswer_IsNotSentAgainUntilThePauseEnds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigPauseTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Server->On(PlayerRoute, { Answer(503, TEXT("{}")) });
	F.SignIn(TokenA);
	TestEqual(TEXT("the game's values"), F.Read(), PatchLives);
	TestEqual(TEXT("again"), F.Read(), PatchLives);
	TestEqual(TEXT("within the pause the game's values answer at once"), F.Server->CountTo(PlayerRoute), 1);

	F.Provider->SetPlayerReadPauseAfterNoAnswerForTesting(0.0);
	F.SignIn(TokenA);
	F.Read();
	F.Read();
	TestEqual(TEXT("sent again once the pause is over"), F.Server->CountTo(PlayerRoute), 3);
	TestEqual(TEXT("no answer is no error"), F.Log->Errors.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigSavedOnceTest, "Flock.Config.Player.OfflineReads_ReadThePlayersSavedCopyOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigSavedOnceTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TokenA);
	TestEqual(TEXT("A's"), F.Read(), ALives);

	F.SignIn(TokenA);
	F.GoOffline();
	TestEqual(TEXT("A's copy"), F.Read(), ALives);
	F.Snapshot->DeleteScope(TEXT("test-gvid/config"));
	TestEqual(TEXT("held in memory once read: the file is not read again"), F.Read(), ALives);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigAnotherConfigTest, "Flock.Config.Player.AnAnswerForAnotherConfig_IsNotUsed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigAnotherConfigTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Server->On(PlayerRoute, { Answer(200, FString(PlayerAAnswer).Replace(ConfigId, TEXT("01M74VYNA2ZCPX6TY2W9HSOTHR"))) });
	F.SignIn(TokenA);
	TestEqual(TEXT("another config's values are not this one's"), F.Read(), PatchLives);
	F.Read();
	TestEqual(TEXT("not asked again during the sign-in"), F.Server->CountTo(PlayerRoute), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigStaleCopyTest, "Flock.Config.Player.ASavedCopyTheServerNoLongerStandsBehind_IsDeleted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigStaleCopyTest::RunTest(const FString& Parameters)
{
	for (const bool bRefused : { true, false })
	{
		const TCHAR* const Why = bRefused ? TEXT("refused") : TEXT("another config");
		FFixture F;
		F.SignIn(TokenA);
		TestEqual(FString::Printf(TEXT("[%s] A's"), Why), F.Read(), ALives);

		F.Server->On(PlayerRoute, { bRefused ? Coded(404, TEXT("game_config.config_not_found"))
			: Answer(200, FString(PlayerAAnswer).Replace(ConfigId, TEXT("01M74VYNA2ZCPX6TY2W9HSOTHR"))) });
		F.SignIn(TokenA);
		TestEqual(FString::Printf(TEXT("[%s] the game's values"), Why), F.Read(), PatchLives);

		F.SignIn(TokenA);
		F.GoOffline();
		TestEqual(FString::Printf(TEXT("[%s] a later offline read does not serve the deleted copy"), Why), F.Read(), PatchLives);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigUnknownIdTest, "Flock.Config.Player.AnIdNoConfigHas_FailsAsBefore_WhenSignedIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigUnknownIdTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Server->On(ConfigByIdRoute, { Coded(404, TEXT("game_config.config_not_found")) });
	F.Server->On(PatchesRoute, { Coded(404, TEXT("game_patch.game_config_not_found")) });
	F.SignIn(TokenA);

	FFlockError Refused;
	TestEqual(TEXT("the read fails"), F.Read(ConfigId, &Refused), -1);
	TestEqual(TEXT("with the same failure as a player signed out gets"),
		static_cast<int32>(Refused.ErrorCode), static_cast<int32>(EFlockErrorCode::GamePatchGameConfigNotFound));
	TestEqual(TEXT("no name, so no player read"), F.Server->CountTo(PlayerRoute), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigNoAnswerCopyTest, "Flock.Config.Player.NoAnswer_ServesThePlayersOwnCopy_ElseTheGamesRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigNoAnswerCopyTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TokenA);
	TestEqual(TEXT("A's"), F.Read(), ALives);

	F.Server->On(PlayerRoute, { Answer(503, TEXT("{}")) });
	F.SignIn(TokenA);
	TestEqual(TEXT("A's own copy when the server does not answer"), F.Read(), ALives);

	F.SignIn(TokenB);
	TestEqual(TEXT("B has no copy, so the game's values"), F.Read(), PatchLives);
	return true;
}

// The player's route answering 200 with the envelope's `result` null: no answer, not a refusal, so it is tried again and asked
// again after the pause, the player's own copy or the game's read answering meanwhile.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigNoResultTest, "Flock.Config.Player.AnAnswerWithNoResult_IsNoAnswer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigNoResultTest::RunTest(const FString& Parameters)
{
	const TCHAR* const NoResult = TEXT("{\"error\":{\"code\":null},\"response\":{\"message\":null,\"code\":null},\"result\":null}");
	{
		FFixture F;
		F.SignIn(TokenA);
		TestEqual(TEXT("A's"), F.Read(), ALives);

		F.Server->On(PlayerRoute, { Answer(200, NoResult) });
		F.SignIn(TokenA);
		TestEqual(TEXT("A's own copy, as when the server does not answer"), F.Read(), ALives);
		TestEqual(TEXT("again"), F.Read(), ALives);
		TestEqual(TEXT("within the pause it is not sent again"), F.Server->CountTo(PlayerRoute), 2);

		// With no pause, each read asks again; a refusal would be held for the sign-in and sent once.
		F.Provider->SetPlayerReadPauseAfterNoAnswerForTesting(0.0);
		F.SignIn(TokenA);
		F.Read();
		F.Read();
		TestEqual(TEXT("sent again on each read, not held for the sign-in"), F.Server->CountTo(PlayerRoute), 4);

		F.SignIn(TokenB);
		TestEqual(TEXT("B has no copy, so the game's values"), F.Read(), PatchLives);
		TestEqual(TEXT("no answer is no error"), F.Log->Errors.Num(), 0);
	}
	{
		// With no copy of the player's, retries are on, as for any read the server did not answer.
		FFixture F(/*MaxRetries*/ 1);
		F.Server->On(PlayerRoute, { Answer(200, NoResult), Answer(200, PlayerAAnswer) });
		F.SignIn(TokenA);
		int32 Lives = -1;
		F.Provider->ResolveConfigData(ConfigId, [&Lives](TFlockResult<FFlockStructuredData> Result)
		{
			if (Result.bSuccess)
			{
				Result.Value.TryGetInt(TEXT("lives"), Lives);
			}
		});
		TickRetries();
		TestEqual(TEXT("tried again"), F.Server->CountTo(PlayerRoute), 2);
		TestEqual(TEXT("and the retry answers"), Lives, ALives);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigAfreshTest, "Flock.Config.Player.ClearCacheAndEachNewSignIn_ReadAfresh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigAfreshTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TokenA);
	F.Read();
	F.Read();
	TestEqual(TEXT("read once"), F.Server->CountTo(PlayerRoute), 1);

	F.Provider->ClearCache();
	F.Read();
	TestEqual(TEXT("ClearCache forgets the player's values"), F.Server->CountTo(PlayerRoute), 2);

	F.Session->ClearTokens();
	TestEqual(TEXT("signed out: the game's values"), F.Read(), PatchLives);
	F.SignIn(TokenA);
	TestEqual(TEXT("A's again"), F.Read(), ALives);
	TestEqual(TEXT("signing in again reads afresh"), F.Server->CountTo(PlayerRoute), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerConfigClearedOnTheWayTest, "Flock.Config.Player.AnAnswerOnItsWayWhenClearCacheRan_IsKeptNowhere",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerConfigClearedOnTheWayTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TokenA);
	F.Server->HoldNext(PlayerRoute);
	int32 Lives = -1;
	F.Provider->ResolveConfigData(ConfigId, [&Lives](TFlockResult<FFlockStructuredData> Result)
	{
		if (Result.bSuccess)
		{
			Result.Value.TryGetInt(TEXT("lives"), Lives);
		}
	});
	F.Provider->ClearCache();
	TestTrue(TEXT("the answer is let through"), F.Server->ReleaseHeld());
	TestEqual(TEXT("still the answer for the player who asked"), Lives, ALives);

	// ClearCache also emptied the game's copies, so offline there is nothing to answer with unless the answer was kept.
	F.GoOffline();
	FFlockError Failed;
	TestEqual(TEXT("kept neither in memory nor on disk"), F.Read(ConfigId, &Failed), -1);
	TestEqual(TEXT("the read fails for want of a connection"), static_cast<int32>(Failed.Type), static_cast<int32>(EFlockErrorType::Connection));

	F.GoOnline();
	TestEqual(TEXT("read again once online"), F.Read(), ALives);
	TestEqual(TEXT("sent twice"), F.Server->CountTo(PlayerRoute), 2);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
