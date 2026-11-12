// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"
#include "Misc/FlockEngineCompat.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockGameplayEvents.h"
#include "Analytics/FlockMatchResult.h"
#include "Blueprint/FlockLibrary.h"
#include "Config/FlockConfig.h"
#include "FlockInitConfig.h"
#include "FlockSubsystem.h"
#include "Http/FlockJsonUtils.h"
#include "Misc/Base64.h"
#include "Providers/FlockAnalyticsProvider.h"
#include "Providers/FlockAuthProvider.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockTestSdk.h"
#include "Tests/Support/FlockTestSpelling.h"
#include "UObject/UnrealType.h"

namespace FlockGameplayEventsTestHelpers
{
	/** The project settings these tests set, put back as they were when the fixture ends. */
	struct FSettingsAside
	{
		UFlockConfig* Settings = GetMutableDefault<UFlockConfig>();
		bool bEnabledWas = Settings->bAnalyticsEnabled;
		bool bExplicitConsentWas = Settings->bAnalyticsRequireExplicitConsent;
		bool bAutoStartWas = Settings->bAnalyticsAutoStartSession;
		bool bDefaultEventsWas = Settings->bAnalyticsSendDefaultEvents;

		~FSettingsAside()
		{
			Settings->bAnalyticsEnabled = bEnabledWas;
			Settings->bAnalyticsRequireExplicitConsent = bExplicitConsentWas;
			Settings->bAnalyticsAutoStartSession = bAutoStartWas;
			Settings->bAnalyticsSendDefaultEvents = bDefaultEventsWas;
		}
	};

	inline FString MakeJwt(const FString& PlayerId)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + 3600;
		FString Payload = FBase64::Encode(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp));
		Payload.ReplaceInline(TEXT("+"), TEXT("-"));
		Payload.ReplaceInline(TEXT("/"), TEXT("_"));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Payload);
	}

	/**
	 * A running SDK (its own folder, so the developer's saved consent answer and queues are never read or changed) with a
	 * fake server and player-a signed in. Every analytics setting these tests depend on is set here, never left to the
	 * project's.
	 */
	struct FFixture
	{
		FSettingsAside Aside;
		FFlockTestSdk Test;
		UFlockSubsystem* Sdk = Test.Sdk;
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();

		explicit FFixture(bool bSendDefaultEvents, bool bAnalyticsEnabled = true)
		{
			Aside.Settings->bAnalyticsEnabled = bAnalyticsEnabled;
			Aside.Settings->bAnalyticsRequireExplicitConsent = false;
			Aside.Settings->bAnalyticsAutoStartSession = false;
			Aside.Settings->bAnalyticsSendDefaultEvents = bSendDefaultEvents;

			Sdk->SetHttpAdapterForTesting(Fake);
			Sdk->SetTokenStoreForTesting(Store);
			Store->bHasTokens = true;
			Store->Stored.AccessToken = MakeJwt(TEXT("player-a"));
			Store->Stored.RefreshToken = TEXT("r-a");
			Store->Stored.AuthMethod = EFlockAuthMethod::Device;

			FFlockInitConfig Config;
			Config.ApiUrl = TEXT("https://api.example.test");
			Config.ApiKey = TEXT("secret");
			Config.GameId = TEXT("my-game");
			Config.GameVersion = TEXT("1.2.3");
			Config.GameVersionId = TEXT("ver-abc");
			Sdk->InitializeWithConfig(Config);
		}

		bool IsSignedIn() const { return Sdk->IsAuthenticated(); }

		/** Sends what is queued and answers every event the fake server received under EventName, in order. */
		TArray<TSharedPtr<FJsonObject>> SentAfterFlush(const FString& EventName) const
		{
			if (FFlockAnalyticsProvider* Analytics = Sdk->GetAnalyticsProvider())
			{
				Analytics->Flush();
			}
			TArray<TSharedPtr<FJsonObject>> Out;
			for (const FFlockHttpRequest& Request : Fake->Requests)
			{
				if (!Request.Url.Contains(TEXT("analytics/events")))
				{
					continue;
				}
				TSharedPtr<FJsonObject> Root;
				const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
				if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Request.JsonBody), Root) && Root.IsValid()
					&& Root->TryGetArrayField(TEXT("events"), Items))
				{
					for (const TSharedPtr<FJsonValue>& Item : *Items)
					{
						const TSharedPtr<FJsonObject> Event = FFlockJsonUtils::AsObjectOrNull(Item);
						FString Name;
						if (Event.IsValid() && Event->TryGetStringField(TEXT("event_name"), Name)
							&& Name.Equals(EventName, ESearchCase::CaseSensitive))
						{
							Out.Add(Event);
						}
					}
				}
			}
			return Out;
		}

		int32 AnalyticsRequests() const { return Fake->CountTo(TEXT("analytics/events")); }
	};

	/** The game's own properties of a sent event: all but the flock_ parameters the SDK adds to every event. */
	inline TMap<FString, TSharedPtr<FJsonValue>> GamesProperties(const TSharedPtr<FJsonObject>& Event)
	{
		TMap<FString, TSharedPtr<FJsonValue>> Out;
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (!Event.IsValid() || !Event->TryGetObjectField(TEXT("properties"), Properties))
		{
			return Out;
		}
		for (const FString& Name : FFlockJsonUtils::GetFieldNames(*Properties))
		{
			if (!Name.StartsWith(TEXT("flock_"), ESearchCase::CaseSensitive))
			{
				Out.Add(Name, (*Properties)->TryGetField(Name));
			}
		}
		return Out;
	}

	inline FString StringOf(const TSharedPtr<FJsonObject>& Event, const TCHAR* Key)
	{
		FString Value;
		return Event.IsValid() && FlockTestSpelling::HasMemberSpelled(Event, Key) && Event->TryGetStringField(Key, Value)
			? Value : FString(TEXT("<absent>"));
	}

	/** A property's expected wire value: a whole number or a text. */
	struct FExpected
	{
		FString Key;
		bool bNumber = false;
		int32 Number = 0;
		FString Text;
	};

	inline FExpected NumberProperty(const TCHAR* Key, int32 Value) { return { Key, true, Value, FString() }; }
	inline FExpected TextProperty(const TCHAR* Key, const TCHAR* Value) { return { Key, false, 0, Value }; }

	/** One typed call: its wire name and category, the call with every argument set, and the call with none. */
	struct FCall
	{
		FString EventName;
		FString Category;
		TFunction<bool(UFlockSubsystem*)> Send;
		TFunction<bool(UFlockSubsystem*)> SendFromBlueprint;
		TFunction<bool(UFlockSubsystem*)> SendNothing;
		TFunction<bool(UFlockSubsystem*)> SendNothingFromBlueprint;
		TArray<FExpected> Expected;
	};

	/** Every argument set, so each property's wire name and type is checked against the tracking plan's. */
	inline TArray<FCall> EveryCall()
	{
		using namespace FlockGameplayEvents;
		const int32 NotSet = BlueprintNumberNotSet;
		const FFlockCommandData None;
		TArray<FCall> Calls;
		Calls.Add({ TEXT("level_start"), TEXT("gameplay"),
			[](UFlockSubsystem* S) { return S->TrackLevelStart(3, TEXT("hard")); },
			[None](UFlockSubsystem* S) { return TrackLevelStartFromBlueprint(S, 3, TEXT("hard"), None); },
			[](UFlockSubsystem* S) { return S->TrackLevelStart(); },
			[None, NotSet](UFlockSubsystem* S) { return TrackLevelStartFromBlueprint(S, NotSet, FString(), None); },
			{ NumberProperty(TEXT("level"), 3), TextProperty(TEXT("difficulty"), TEXT("hard")) } });
		Calls.Add({ TEXT("level_complete"), TEXT("gameplay"),
			[](UFlockSubsystem* S) { return S->TrackLevelComplete(3, 1200, 95, 2); },
			[None](UFlockSubsystem* S) { return TrackLevelCompleteFromBlueprint(S, 3, 1200, 95, 2, None); },
			[](UFlockSubsystem* S) { return S->TrackLevelComplete(); },
			[None, NotSet](UFlockSubsystem* S) { return TrackLevelCompleteFromBlueprint(S, NotSet, NotSet, NotSet, NotSet, None); },
			{ NumberProperty(TEXT("level"), 3), NumberProperty(TEXT("score"), 1200), NumberProperty(TEXT("duration_seconds"), 95), NumberProperty(TEXT("stars"), 2) } });
		Calls.Add({ TEXT("level_fail"), TEXT("gameplay"),
			[](UFlockSubsystem* S) { return S->TrackLevelFail(3, TEXT("timeout"), 40); },
			[None](UFlockSubsystem* S) { return TrackLevelFailFromBlueprint(S, 3, TEXT("timeout"), 40, None); },
			[](UFlockSubsystem* S) { return S->TrackLevelFail(); },
			[None, NotSet](UFlockSubsystem* S) { return TrackLevelFailFromBlueprint(S, NotSet, FString(), NotSet, None); },
			{ NumberProperty(TEXT("level"), 3), TextProperty(TEXT("reason"), TEXT("timeout")), NumberProperty(TEXT("duration_seconds"), 40) } });
		Calls.Add({ TEXT("checkpoint_start"), TEXT("gameplay"),
			[](UFlockSubsystem* S) { return S->TrackCheckpoint(3, TEXT("bridge")); },
			[None](UFlockSubsystem* S) { return TrackCheckpointFromBlueprint(S, 3, TEXT("bridge"), None); },
			[](UFlockSubsystem* S) { return S->TrackCheckpoint(); },
			[None, NotSet](UFlockSubsystem* S) { return TrackCheckpointFromBlueprint(S, NotSet, FString(), None); },
			{ NumberProperty(TEXT("level"), 3), TextProperty(TEXT("checkpoint"), TEXT("bridge")) } });
		Calls.Add({ TEXT("tutorial_step"), TEXT("ui"),
			[](UFlockSubsystem* S) { return S->TrackTutorialStep(2, TEXT("jump")); },
			[None](UFlockSubsystem* S) { return TrackTutorialStepFromBlueprint(S, 2, TEXT("jump"), None); },
			[](UFlockSubsystem* S) { return S->TrackTutorialStep(); },
			[None, NotSet](UFlockSubsystem* S) { return TrackTutorialStepFromBlueprint(S, NotSet, FString(), None); },
			{ NumberProperty(TEXT("step"), 2), TextProperty(TEXT("step_name"), TEXT("jump")) } });
		Calls.Add({ TEXT("settings_change"), TEXT("ui"),
			[](UFlockSubsystem* S) { return S->TrackSettingsChange(TEXT("music_volume"), TEXT("0.5")); },
			[None](UFlockSubsystem* S) { return TrackSettingsChangeFromBlueprint(S, TEXT("music_volume"), TEXT("0.5"), None); },
			[](UFlockSubsystem* S) { return S->TrackSettingsChange(); },
			[None](UFlockSubsystem* S) { return TrackSettingsChangeFromBlueprint(S, FString(), FString(), None); },
			{ TextProperty(TEXT("setting"), TEXT("music_volume")), TextProperty(TEXT("value"), TEXT("0.5")) } });
		Calls.Add({ TEXT("match_start"), TEXT("gameplay"),
			[](UFlockSubsystem* S) { return S->TrackMatchStart(TEXT("ranked"), TEXT("harbour"), 4); },
			[None](UFlockSubsystem* S) { return TrackMatchStartFromBlueprint(S, TEXT("ranked"), TEXT("harbour"), 4, None); },
			[](UFlockSubsystem* S) { return S->TrackMatchStart(); },
			[None, NotSet](UFlockSubsystem* S) { return TrackMatchStartFromBlueprint(S, FString(), FString(), NotSet, None); },
			{ TextProperty(TEXT("mode"), TEXT("ranked")), TextProperty(TEXT("map"), TEXT("harbour")), NumberProperty(TEXT("team_size"), 4) } });
		Calls.Add({ TEXT("match_complete"), TEXT("gameplay"),
			[](UFlockSubsystem* S) { return S->TrackMatchComplete(TEXT("ranked"), FlockMatchResult::Win, 30, 600, 7); },
			[None](UFlockSubsystem* S) { return TrackMatchCompleteFromBlueprint(S, TEXT("ranked"), UFlockLibrary::MatchResultWin(), 30, 600, 7, None); },
			[](UFlockSubsystem* S) { return S->TrackMatchComplete(); },
			[None, NotSet](UFlockSubsystem* S) { return TrackMatchCompleteFromBlueprint(S, FString(), FString(), NotSet, NotSet, NotSet, None); },
			{ TextProperty(TEXT("mode"), TEXT("ranked")), TextProperty(TEXT("result"), TEXT("win")), NumberProperty(TEXT("score"), 30),
				NumberProperty(TEXT("duration_seconds"), 600), NumberProperty(TEXT("kills"), 7) } });
		return Calls;
	}

	/** The single event the call sent, checked against the plan: its name, category, player, and the game's properties exactly. */
	inline void CheckSentAsThePlanSays(FAutomationTestBase& Test, const FFixture& F, const FCall& Call, const FString& Context)
	{
		const TArray<TSharedPtr<FJsonObject>> Sent = F.SentAfterFlush(Call.EventName);
		Test.TestEqual(Context + TEXT(": sent once"), Sent.Num(), 1);
		if (Sent.Num() != 1)
		{
			return;
		}
		const TSharedPtr<FJsonObject>& Event = Sent[0];
		FlockEngineCompat::CheckEqualSpelledExactly(Test, *(Context + TEXT(": category")), *StringOf(Event, TEXT("event_category")), *Call.Category);
		FlockEngineCompat::CheckEqualSpelledExactly(Test, *(Context + TEXT(": player")), *StringOf(Event, TEXT("player_id")), TEXT("player-a"));
		const TMap<FString, TSharedPtr<FJsonValue>> Properties = GamesProperties(Event);
		Test.TestEqual(Context + TEXT(": exactly the plan's properties"), Properties.Num(), Call.Expected.Num());
		for (const FExpected& Expected : Call.Expected)
		{
			const TSharedPtr<FJsonObject>* Bag = nullptr;
			Event->TryGetObjectField(TEXT("properties"), Bag);
			const bool bSpelled = Bag != nullptr && FlockTestSpelling::HasMemberSpelled(*Bag, Expected.Key);
			Test.TestTrue(Context + TEXT(": spelled ") + Expected.Key, bSpelled);
			const TSharedPtr<FJsonValue>* Value = Properties.Find(Expected.Key);
			if (!bSpelled || Value == nullptr || !Value->IsValid())
			{
				continue;
			}
			if (Expected.bNumber)
			{
				Test.TestEqual(Context + TEXT(": a number, ") + Expected.Key, (int32)(*Value)->Type, (int32)EJson::Number);
				Test.TestEqual(Context + TEXT(": ") + Expected.Key, (*Value)->AsNumber(), (double)Expected.Number);
			}
			else
			{
				Test.TestEqual(Context + TEXT(": a text, ") + Expected.Key, (int32)(*Value)->Type, (int32)EJson::String);
				FlockEngineCompat::CheckEqualSpelledExactly(Test, *(Context + TEXT(": ") + Expected.Key), *(*Value)->AsString(), *Expected.Text);
			}
		}
		// A whole number goes out as one (3, not 3.0): the parsed value above cannot tell the two apart, the server's column can.
		FString Bodies;
		for (const FFlockHttpRequest& Request : F.Fake->Requests)
		{
			Bodies += Request.Url.Contains(TEXT("analytics/events")) ? Request.JsonBody : FString();
		}
		for (const FExpected& Expected : Call.Expected)
		{
			if (Expected.bNumber)
			{
				const FString Whole = FString::Printf(TEXT("\"%s\":%d"), *Expected.Key, Expected.Number);
				Test.TestTrue(Context + TEXT(": a whole number on the wire, ") + Expected.Key,
					Bodies.Contains(Whole + TEXT(","), ESearchCase::CaseSensitive) || Bodies.Contains(Whole + TEXT("}"), ESearchCase::CaseSensitive));
			}
		}
		const TSharedPtr<FJsonObject>* Bag = nullptr;
		Test.TestTrue(Context + TEXT(": the SDK adds its parameters, as to every event"),
			Event->TryGetObjectField(TEXT("properties"), Bag) && (*Bag)->HasField(TEXT("flock_sdk_version")));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameplayEventsWireTest, "Flock.Analytics.GameplayEvents.EachCallSendsThePlansNameCategoryAndProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockGameplayEventsWireTest::RunTest(const FString& Parameters)
{
	using namespace FlockGameplayEventsTestHelpers;
	for (const FCall& Call : EveryCall())
	{
		FFixture F(/*bSendDefaultEvents*/ false);
		TestTrue(Call.EventName + TEXT(": precondition, signed in"), F.IsSignedIn());
		TestTrue(Call.EventName + TEXT(": accepted"), Call.Send(F.Sdk));
		CheckSentAsThePlanSays(*this, F, Call, Call.EventName);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameplayEventsBlueprintWireTest, "Flock.Analytics.GameplayEvents.EachBlueprintNodeSendsThePlansNameCategoryAndProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockGameplayEventsBlueprintWireTest::RunTest(const FString& Parameters)
{
	using namespace FlockGameplayEventsTestHelpers;
	for (const FCall& Call : EveryCall())
	{
		FFixture F(/*bSendDefaultEvents*/ false);
		TestTrue(Call.EventName + TEXT(" node: accepted"), Call.SendFromBlueprint(F.Sdk));
		CheckSentAsThePlanSays(*this, F, Call, Call.EventName + TEXT(" node"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameplayEventsNoArgumentsTest, "Flock.Analytics.GameplayEvents.EachCallWithNoArgumentsSendsNoPropertiesOfTheGames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockGameplayEventsNoArgumentsTest::RunTest(const FString& Parameters)
{
	using namespace FlockGameplayEventsTestHelpers;
	for (const FCall& Call : EveryCall())
	{
		for (const bool bFromBlueprint : { false, true })
		{
			const FString Context = Call.EventName + (bFromBlueprint ? TEXT(" node, every pin at its default") : TEXT(", no arguments"));
			FFixture F(/*bSendDefaultEvents*/ false);
			TestTrue(Context + TEXT(": accepted"), bFromBlueprint ? Call.SendNothingFromBlueprint(F.Sdk) : Call.SendNothing(F.Sdk));
			const TArray<TSharedPtr<FJsonObject>> Sent = F.SentAfterFlush(Call.EventName);
			TestEqual(Context + TEXT(": sent once"), Sent.Num(), 1);
			if (Sent.Num() == 1)
			{
				TestEqualSensitive(*(Context + TEXT(": category")), *StringOf(Sent[0], TEXT("event_category")), *Call.Category);
				TestEqual(Context + TEXT(": nothing set, nothing sent, not nulls or zeros"), GamesProperties(Sent[0]).Num(), 0);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameplayEventsZeroIsAValueTest, "Flock.Analytics.GameplayEvents.OnlyTheArgumentsSetAreSentAndZeroIsAValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockGameplayEventsZeroIsAValueTest::RunTest(const FString& Parameters)
{
	using namespace FlockGameplayEventsTestHelpers;
	using FlockGameplayEvents::BlueprintNumberNotSet;
	struct FCase
	{
		const TCHAR* Name;
		TFunction<bool(UFlockSubsystem*)> Send;
		int32 ExpectedScore; // or BlueprintNumberNotSet when the score must be absent
	};
	const TArray<FCase> Cases = {
		{ TEXT("C++"), [](UFlockSubsystem* S) { return S->TrackLevelComplete(4, {}, {}, 0); }, BlueprintNumberNotSet },
		{ TEXT("node"), [](UFlockSubsystem* S) { return FlockGameplayEvents::TrackLevelCompleteFromBlueprint(S, 4,
			BlueprintNumberNotSet, BlueprintNumberNotSet, 0, FFlockCommandData()); }, BlueprintNumberNotSet },
		// Only the lowest integer means "not set" on a pin: a negative score is a value.
		{ TEXT("node, negative score"), [](UFlockSubsystem* S) { return FlockGameplayEvents::TrackLevelCompleteFromBlueprint(S, 4,
			-5, BlueprintNumberNotSet, 0, FFlockCommandData()); }, -5 },
	};
	for (const FCase& Case : Cases)
	{
		FFixture F(/*bSendDefaultEvents*/ false);
		TestTrue(FString(Case.Name) + TEXT(": accepted"), Case.Send(F.Sdk));
		const TArray<TSharedPtr<FJsonObject>> Sent = F.SentAfterFlush(TEXT("level_complete"));
		TestEqual(FString(Case.Name) + TEXT(": sent once"), Sent.Num(), 1);
		if (Sent.Num() != 1)
		{
			continue;
		}
		const TMap<FString, TSharedPtr<FJsonValue>> Properties = GamesProperties(Sent[0]);
		const bool bScore = Case.ExpectedScore != BlueprintNumberNotSet;
		TestEqual(FString(Case.Name) + TEXT(": only what was set"), Properties.Num(), bScore ? 3 : 2);
		const TSharedPtr<FJsonValue>* Stars = Properties.Find(TEXT("stars"));
		TestTrue(FString(Case.Name) + TEXT(": zero stars is a value, not unset"),
			Stars != nullptr && (*Stars)->Type == EJson::Number && (*Stars)->AsNumber() == 0.0);
		const TSharedPtr<FJsonValue>* Level = Properties.Find(TEXT("level"));
		TestTrue(FString(Case.Name) + TEXT(": level"), Level != nullptr && (*Level)->AsNumber() == 4.0);
		TestFalse(FString(Case.Name) + TEXT(": duration unset"), Properties.Contains(TEXT("duration_seconds")));
		const TSharedPtr<FJsonValue>* Score = Properties.Find(TEXT("score"));
		if (bScore)
		{
			TestTrue(FString(Case.Name) + TEXT(": the score set"), Score != nullptr && (*Score)->AsNumber() == Case.ExpectedScore);
		}
		else
		{
			TestNull(FString(Case.Name) + TEXT(": score unset"), Score);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameplayEventsNamedArgumentWinsTest, "Flock.Analytics.GameplayEvents.ANamedArgumentWinsOverTheSameExtraPropertyAndTheGamesMapIsNotChanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockGameplayEventsNamedArgumentWinsTest::RunTest(const FString& Parameters)
{
	using namespace FlockGameplayEventsTestHelpers;

	// The same key, spelled the same.
	{
		FFixture F(/*bSendDefaultEvents*/ false);
		const FFlockCommandData Extra = FFlockCommandData().Set(TEXT("kills"), 1).Set(TEXT("score"), 5).Set(TEXT("weapon"), TEXT("bow"));
		const FString ExtraBefore = Extra.ToJsonString();

		TestTrue(TEXT("accepted"), F.Sdk->TrackMatchComplete(FString(), FlockMatchResult::Draw, {}, {}, 7, Extra));

		const TArray<TSharedPtr<FJsonObject>> Sent = F.SentAfterFlush(TEXT("match_complete"));
		TestEqual(TEXT("sent once"), Sent.Num(), 1);
		if (Sent.Num() == 1)
		{
			const TMap<FString, TSharedPtr<FJsonValue>> Properties = GamesProperties(Sent[0]);
			TestEqual(TEXT("the extra properties and the named arguments"), Properties.Num(), 4);
			const TSharedPtr<FJsonValue>* Kills = Properties.Find(TEXT("kills"));
			TestTrue(TEXT("the named argument wins"), Kills != nullptr && (*Kills)->AsNumber() == 7.0);
			const TSharedPtr<FJsonValue>* Score = Properties.Find(TEXT("score"));
			TestTrue(TEXT("an extra property whose argument is unset is sent"), Score != nullptr && (*Score)->AsNumber() == 5.0);
			const TSharedPtr<FJsonValue>* Weapon = Properties.Find(TEXT("weapon"));
			TestTrue(TEXT("weapon"), Weapon != nullptr && (*Weapon)->AsString().Equals(TEXT("bow"), ESearchCase::CaseSensitive));
			const TSharedPtr<FJsonValue>* Result = Properties.Find(TEXT("result"));
			TestTrue(TEXT("result"), Result != nullptr && (*Result)->AsString().Equals(TEXT("draw"), ESearchCase::CaseSensitive));
		}
		TestEqualSensitive(TEXT("the game's map is never changed"), *Extra.ToJsonString(), *ExtraBefore);
	}

	// The same through the Blueprint node: its Extra Properties pin reaches the call.
	{
		FFixture F(/*bSendDefaultEvents*/ false);
		const FFlockCommandData Extra = FFlockCommandData().Set(TEXT("kills"), 1).Set(TEXT("score"), 5).Set(TEXT("weapon"), TEXT("bow"));
		const FString ExtraBefore = Extra.ToJsonString();
		const int32 NotSet = FlockGameplayEvents::BlueprintNumberNotSet;

		TestTrue(TEXT("node: accepted"), FlockGameplayEvents::TrackMatchCompleteFromBlueprint(F.Sdk, FString(),
			UFlockLibrary::MatchResultDraw(), NotSet, NotSet, 7, Extra));

		const TArray<TSharedPtr<FJsonObject>> Sent = F.SentAfterFlush(TEXT("match_complete"));
		TestEqual(TEXT("node: sent once"), Sent.Num(), 1);
		if (Sent.Num() == 1)
		{
			const TMap<FString, TSharedPtr<FJsonValue>> Properties = GamesProperties(Sent[0]);
			TestEqual(TEXT("node: the extra properties and the pins set"), Properties.Num(), 4);
			const TSharedPtr<FJsonValue>* Kills = Properties.Find(TEXT("kills"));
			TestTrue(TEXT("node: the pin wins"), Kills != nullptr && (*Kills)->AsNumber() == 7.0);
			const TSharedPtr<FJsonValue>* Score = Properties.Find(TEXT("score"));
			TestTrue(TEXT("node: an extra property whose pin is not set is sent"), Score != nullptr && (*Score)->AsNumber() == 5.0);
			const TSharedPtr<FJsonValue>* Weapon = Properties.Find(TEXT("weapon"));
			TestTrue(TEXT("node: weapon"), Weapon != nullptr && (*Weapon)->AsString().Equals(TEXT("bow"), ESearchCase::CaseSensitive));
		}
		TestEqualSensitive(TEXT("node: the game's map is never changed"), *Extra.ToJsonString(), *ExtraBefore);
	}

	// The same key in another letter case: this engine's JSON object holds one spelling, so the call's own replaces the
	// game's, and a warning names the one dropped.
	{
		AddExpectedMessagePlain(TEXT("Extra property 'Kills' is not sent"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		FFixture F(/*bSendDefaultEvents*/ false);
		const FFlockCommandData Extra = FFlockCommandData().Set(TEXT("Kills"), 1).Set(TEXT("weapon"), TEXT("bow"));
		const FString ExtraBefore = Extra.ToJsonString();

		TestTrue(TEXT("other spelling: accepted"), F.Sdk->TrackMatchComplete(FString(), FString(), {}, {}, 7, Extra));

		const TArray<TSharedPtr<FJsonObject>> Sent = F.SentAfterFlush(TEXT("match_complete"));
		TestEqual(TEXT("other spelling: sent once"), Sent.Num(), 1);
		if (Sent.Num() == 1)
		{
			const TSharedPtr<FJsonObject>* Bag = nullptr;
			Sent[0]->TryGetObjectField(TEXT("properties"), Bag);
			TestTrue(TEXT("other spelling: the call's spelling is sent"), Bag != nullptr && FlockTestSpelling::HasMemberSpelled(*Bag, TEXT("kills")));
			TestFalse(TEXT("other spelling: the game's is not"), Bag != nullptr && FlockTestSpelling::HasMemberSpelled(*Bag, TEXT("Kills")));
			const TMap<FString, TSharedPtr<FJsonValue>> Properties = GamesProperties(Sent[0]);
			const TSharedPtr<FJsonValue>* Kills = Properties.Find(TEXT("kills"));
			TestTrue(TEXT("other spelling: with the call's value"), Kills != nullptr && (*Kills)->AsNumber() == 7.0);
			TestEqual(TEXT("other spelling: kills and weapon"), Properties.Num(), 2);
		}
		TestEqualSensitive(TEXT("other spelling: the game's map is never changed"), *Extra.ToJsonString(), *ExtraBefore);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameplayEventsConsentTest, "Flock.Analytics.GameplayEvents.WithoutConsentACallIsRefusedAndNothingIsSent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockGameplayEventsConsentTest::RunTest(const FString& Parameters)
{
	using namespace FlockGameplayEventsTestHelpers;
	// The SDK's own folder holds this consent answer, so the developer's saved one is never touched.
	FFixture F(/*bSendDefaultEvents*/ false);
	F.Sdk->SetAnalyticsConsent(false);

	TestFalse(TEXT("refused"), F.Sdk->TrackLevelStart(1));
	TestFalse(TEXT("the node too"), FlockGameplayEvents::TrackLevelStartFromBlueprint(F.Sdk, 1, FString(), FFlockCommandData()));
	F.Sdk->SetAnalyticsConsent(true);

	TestEqual(TEXT("nothing sent"), F.SentAfterFlush(TEXT("level_start")).Num(), 0);

	// Control: with consent the same call is sent, so the count above is the refusal's.
	TestTrue(TEXT("control: accepted with consent"), F.Sdk->TrackLevelStart(1));
	TestEqual(TEXT("control: sent"), F.SentAfterFlush(TEXT("level_start")).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameplayEventsDefaultEventsSettingTest, "Flock.Analytics.GameplayEvents.TheCallsAreSentWithSendDefaultEventsOnAndOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockGameplayEventsDefaultEventsSettingTest::RunTest(const FString& Parameters)
{
	using namespace FlockGameplayEventsTestHelpers;
	for (const bool bSendDefaultEvents : { true, false })
	{
		const FString Context = bSendDefaultEvents ? TEXT("Send Default Events on") : TEXT("Send Default Events off");
		FFixture F(bSendDefaultEvents);
		TestTrue(Context + TEXT(": accepted"), F.Sdk->TrackMatchStart(TEXT("casual")));
		TestTrue(Context + TEXT(": the node accepted"),
			FlockGameplayEvents::TrackMatchStartFromBlueprint(F.Sdk, TEXT("casual"), FString(), FlockGameplayEvents::BlueprintNumberNotSet, FFlockCommandData()));
		TestEqual(Context + TEXT(": both sent"), F.SentAfterFlush(TEXT("match_start")).Num(), 2);
		// The setting was read: the SDK's own app_open goes with it on and not off.
		TestEqual(Context + TEXT(": control, the SDK's own app_open"), F.SentAfterFlush(TEXT("app_open")).Num(), bSendDefaultEvents ? 1 : 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameplayEventsAnalyticsOffTest, "Flock.Analytics.GameplayEvents.WithAnalyticsSwitchedOffACallReturnsFalseAndSendsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockGameplayEventsAnalyticsOffTest::RunTest(const FString& Parameters)
{
	using namespace FlockGameplayEventsTestHelpers;
	FFixture F(/*bSendDefaultEvents*/ true, /*bAnalyticsEnabled*/ false);
	TestNull(TEXT("precondition: analytics is off"), F.Sdk->GetAnalyticsProvider());
	for (const FCall& Call : EveryCall())
	{
		TestFalse(Call.EventName + TEXT(": false"), Call.Send(F.Sdk));
		TestFalse(Call.EventName + TEXT(" node: false"), Call.SendFromBlueprint(F.Sdk));
	}
	TestEqual(TEXT("nothing sent"), F.AnalyticsRequests(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameplayEventsMatchResultTest, "Flock.Analytics.GameplayEvents.TheMatchResultsAreThePlansWords",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockGameplayEventsMatchResultTest::RunTest(const FString& Parameters)
{
	TestEqualSensitive(TEXT("win"), FlockMatchResult::Win, TEXT("win"));
	TestEqualSensitive(TEXT("loss"), FlockMatchResult::Loss, TEXT("loss"));
	TestEqualSensitive(TEXT("draw"), FlockMatchResult::Draw, TEXT("draw"));
	TestEqualSensitive(TEXT("the win node"), *UFlockLibrary::MatchResultWin(), TEXT("win"));
	TestEqualSensitive(TEXT("the loss node"), *UFlockLibrary::MatchResultLoss(), TEXT("loss"));
	TestEqualSensitive(TEXT("the draw node"), *UFlockLibrary::MatchResultDraw(), TEXT("draw"));
	return true;
}

/**
 * Every number pin of the tracking plan's nodes starts at the value that means "not set", so a node dropped into a graph
 * sends only what the game wires; and a node with no SDK to reach answers false.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameplayEventsBlueprintPinsTest, "Flock.Analytics.GameplayEvents.EveryNumberPinStartsNotSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockGameplayEventsBlueprintPinsTest::RunTest(const FString& Parameters)
{
#if WITH_EDITORONLY_DATA
	const FString NotSet = LexToString(FlockGameplayEvents::BlueprintNumberNotSet);
#endif
	const TMap<FString, int32> NumberPinsPerNode = {
		{ TEXT("TrackLevelStart"), 1 }, { TEXT("TrackLevelComplete"), 4 }, { TEXT("TrackLevelFail"), 2 },
		{ TEXT("TrackCheckpoint"), 1 }, { TEXT("TrackTutorialStep"), 1 }, { TEXT("TrackSettingsChange"), 0 },
		{ TEXT("TrackMatchStart"), 1 }, { TEXT("TrackMatchComplete"), 3 },
	};
	for (const TPair<FString, int32>& Node : NumberPinsPerNode)
	{
		const UFunction* Function = UFlockLibrary::StaticClass()->FindFunctionByName(*Node.Key);
		TestNotNull(Node.Key, Function);
		if (Function == nullptr)
		{
			continue;
		}
		int32 NumberPins = 0;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (It->IsA<FIntProperty>() && !It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				++NumberPins;
				// Blueprint metadata is editor-only data: a packaged game compiles the tests without it, and has no graphs.
#if WITH_EDITORONLY_DATA
				const FString Default = Function->GetMetaData(*(TEXT("CPP_Default_") + It->GetName()));
				TestEqualSensitive(*(Node.Key + TEXT(".") + It->GetName() + TEXT(" starts not set")), *Default, *NotSet);
#endif
			}
		}
		TestEqual(Node.Key + TEXT(": its number pins"), NumberPins, Node.Value);
	}

	// No SDK to reach (no world): every node answers false.
	TestFalse(TEXT("level start, no SDK"), UFlockLibrary::TrackLevelStart(nullptr, 1, FString(), FFlockCommandData()));
	TestFalse(TEXT("match complete, no SDK"), UFlockLibrary::TrackMatchComplete(nullptr, FString(), FString(), 1, 1, 1, FFlockCommandData()));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
