// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockPlaytestConfig.h"
#include "Http/FlockJsonUtils.h"
#include "Tests/FlockPlaytestFakeTransport.h"

namespace
{
	/** Reads a config the way the Protokite client does: unwrap the envelope, then the config's own parse. */
	bool ReadConfig(const FString& Body, FFlockPlaytestConfig& OutConfig)
	{
		FString Error;
		return FFlockJsonUtils::UnwrapResultToStruct(Body, OutConfig, Error);
	}

	FString ConfigWithFeatures(const FString& FeaturesJson)
	{
		return FlockPlaytestFixtures::Envelope(FString::Printf(
			TEXT("{\"session_started_event\":\"session_started\",\"test_id\":\"t1\",\"features\":%s,\"form\":null}"), *FeaturesJson));
	}

	FString ConfigWithFields(const FString& FieldsJson)
	{
		return FlockPlaytestFixtures::Envelope(FString::Printf(
			TEXT("{\"session_started_event\":\"session_started\",\"test_id\":\"t1\",\"features\":{},")
			TEXT("\"form\":{\"id\":\"f1\",\"test_id\":\"t1\",\"game_id\":\"g1\",\"title\":\"Feedback\",\"fields\":%s,")
			TEXT("\"created_at\":\"2026-09-14T10:00:00Z\",\"updated_at\":\"2026-09-14T10:00:00Z\"}}"), *FieldsJson));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConfigReadsTheServersConfigTest,
	"Flock.Playtest.Config.ReadsTheServersConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConfigReadsTheServersConfigTest::RunTest(const FString& Parameters)
{
	FFlockPlaytestConfig Config;
	if (!TestTrue(TEXT("The config is read"), ReadConfig(FlockPlaytestFixtures::ConfigBody(), Config)))
	{
		return true;
	}

	TestEqual(TEXT("Test id"), Config.TestId, FString(FlockPlaytestFixtures::TestId));
	TestEqual(TEXT("Session started event"), Config.SessionStartedEvent, FString(TEXT("session_started")));
	TestEqual(TEXT("Flock game version id"), Config.FlockGameVersionId, FString(FlockPlaytestFixtures::GameVersionId));
	TestFalse(TEXT("Video recording is off"), Config.IsFeatureEnabled(FlockPlaytestFeatures::VideoRecording));
	TestTrue(TEXT("Exception capturing is on"), Config.IsFeatureEnabled(FlockPlaytestFeatures::ExceptionCapturing));
	TestFalse(TEXT("Heavy analytics is off"), Config.IsFeatureEnabled(FlockPlaytestFeatures::HeavyAnalytics));

	TestTrue(TEXT("The playtest has a form"), Config.HasForm());
	TestEqual(TEXT("Form title"), Config.Form.Title, FString(TEXT("Playtest feedback")));
	TestEqual(TEXT("A null description reads as empty"), Config.Form.Description, FString());
	TestTrue(TEXT("The form is published"), Config.Form.IsPublished);
	if (TestEqual(TEXT("Three questions"), Config.Form.Fields.Num(), 3))
	{
		const FFlockPlaytestFormField& Rating = Config.Form.Fields[0];
		TestEqual(TEXT("First question id"), Rating.Id, FString(TEXT("rating")));
		TestEqual(TEXT("First question kind"), Rating.Type, FString(FlockPlaytestFormFieldTypes::Rating));
		TestTrue(TEXT("First question is required"), Rating.Required);

		const FFlockPlaytestFormField& Category = Config.Form.Fields[1];
		TestEqual(TEXT("Select question kind"), Category.Type, FString(FlockPlaytestFormFieldTypes::Select));
		TestEqual(TEXT("Select question choices"), Category.Options.Num(), 4);

		const FFlockPlaytestFormField& Steps = Config.Form.Fields[2];
		TestFalse(TEXT("Third question is optional"), Steps.Required);
		TestEqual(TEXT("Third question guidance"), Steps.HelpText, FString(TEXT("What were you doing when it happened?")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConfigMissingFeatureIsOffTest,
	"Flock.Playtest.Config.MissingFeatureIsOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConfigMissingFeatureIsOffTest::RunTest(const FString& Parameters)
{
	// A playtest saved before a feature existed never mentions it, and must not have it switched on.
	FFlockPlaytestConfig Config;
	if (TestTrue(TEXT("Read"), ReadConfig(ConfigWithFeatures(TEXT("{\"video_recording\":true}")), Config)))
	{
		TestTrue(TEXT("The mentioned feature is on"), Config.IsFeatureEnabled(FlockPlaytestFeatures::VideoRecording));
		TestFalse(TEXT("An unmentioned feature is off"), Config.IsFeatureEnabled(FlockPlaytestFeatures::ExceptionCapturing));
		TestFalse(TEXT("Another unmentioned feature is off"), Config.IsFeatureEnabled(FlockPlaytestFeatures::HeavyAnalytics));
	}

	FFlockPlaytestConfig NoFeatures;
	if (TestTrue(TEXT("Read without features"), ReadConfig(FlockPlaytestFixtures::Envelope(
		TEXT("{\"session_started_event\":\"session_started\",\"test_id\":\"t1\"}")), NoFeatures)))
	{
		TestFalse(TEXT("No features member means every feature is off"),
			NoFeatures.IsFeatureEnabled(FlockPlaytestFeatures::VideoRecording));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConfigOnlyJsonTrueTurnsAFeatureOnTest,
	"Flock.Playtest.Config.OnlyJsonTrueTurnsAFeatureOn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConfigOnlyJsonTrueTurnsAFeatureOnTest::RunTest(const FString& Parameters)
{
	FFlockPlaytestConfig Config;
	if (TestTrue(TEXT("Read"), ReadConfig(ConfigWithFeatures(
		TEXT("{\"text_true\":\"true\",\"number_one\":1,\"null_value\":null,\"false_value\":false,\"real_true\":true}")), Config)))
	{
		TestFalse(TEXT("The text \"true\" is not a boolean"), Config.IsFeatureEnabled(TEXT("text_true")));
		TestFalse(TEXT("The number 1 is not a boolean"), Config.IsFeatureEnabled(TEXT("number_one")));
		TestFalse(TEXT("null is not a boolean"), Config.IsFeatureEnabled(TEXT("null_value")));
		TestFalse(TEXT("false is off"), Config.IsFeatureEnabled(TEXT("false_value")));
		TestTrue(TEXT("true is on"), Config.IsFeatureEnabled(TEXT("real_true")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConfigMissingTestIdFailsTheReadTest,
	"Flock.Playtest.Config.MissingTestIdFailsTheRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConfigMissingTestIdFailsTheReadTest::RunTest(const FString& Parameters)
{
	FFlockPlaytestConfig Config;
	TestFalse(TEXT("No test_id"), ReadConfig(FlockPlaytestFixtures::Envelope(
		TEXT("{\"session_started_event\":\"session_started\",\"features\":{\"video_recording\":true}}")), Config));
	TestFalse(TEXT("An empty test_id"), ReadConfig(FlockPlaytestFixtures::Envelope(
		TEXT("{\"session_started_event\":\"session_started\",\"test_id\":\"\"}")), Config));
	TestFalse(TEXT("A null test_id"), ReadConfig(FlockPlaytestFixtures::Envelope(
		TEXT("{\"session_started_event\":\"session_started\",\"test_id\":null}")), Config));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConfigNullFormMeansNoFormTest,
	"Flock.Playtest.Config.NullFormMeansNoForm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConfigNullFormMeansNoFormTest::RunTest(const FString& Parameters)
{
	FFlockPlaytestConfig NullForm;
	if (TestTrue(TEXT("Read with a null form"), ReadConfig(ConfigWithFeatures(TEXT("{}")), NullForm)))
	{
		TestFalse(TEXT("A null form is no form"), NullForm.HasForm());
	}

	FFlockPlaytestConfig NoFormMember;
	if (TestTrue(TEXT("Read without a form member"), ReadConfig(FlockPlaytestFixtures::Envelope(
		TEXT("{\"session_started_event\":\"session_started\",\"test_id\":\"t1\"}")), NoFormMember)))
	{
		TestFalse(TEXT("No form member is no form"), NoFormMember.HasForm());
	}

	FFlockPlaytestConfig FormWithoutId;
	if (TestTrue(TEXT("Read with a form that has no id"), ReadConfig(FlockPlaytestFixtures::Envelope(
		TEXT("{\"session_started_event\":\"session_started\",\"test_id\":\"t1\",\"form\":{\"title\":\"Feedback\",\"fields\":[]}}")), FormWithoutId)))
	{
		TestFalse(TEXT("A form that cannot take answers is no form"), FormWithoutId.HasForm());
	}

	FFlockPlaytestConfig WithForm;
	if (TestTrue(TEXT("Read with a form"), ReadConfig(ConfigWithFields(TEXT("[]")), WithForm)))
	{
		TestTrue(TEXT("A form with an id is a form"), WithForm.HasForm());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConfigQuestionIsRequiredUnlessTheServerSaysNotTest,
	"Flock.Playtest.Config.QuestionIsRequiredUnlessTheServerSaysNot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConfigQuestionIsRequiredUnlessTheServerSaysNotTest::RunTest(const FString& Parameters)
{
	FFlockPlaytestConfig Config;
	if (TestTrue(TEXT("Read"), ReadConfig(ConfigWithFields(
		TEXT("[{\"id\":\"a\",\"type\":\"text\",\"label\":\"A\"},")
		TEXT("{\"id\":\"b\",\"type\":\"text\",\"label\":\"B\",\"required\":false},")
		TEXT("{\"id\":\"c\",\"type\":\"text\",\"label\":\"C\",\"required\":\"false\"}]")), Config)))
	{
		if (TestEqual(TEXT("Three questions"), Config.Form.Fields.Num(), 3))
		{
			TestTrue(TEXT("No required member means required"), Config.Form.Fields[0].Required);
			TestFalse(TEXT("required false means optional"), Config.Form.Fields[1].Required);
			TestTrue(TEXT("The text \"false\" is not a boolean, so the default holds"), Config.Form.Fields[2].Required);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConfigFeatureNamesAreKeptAsSentTest,
	"Flock.Playtest.Config.FeatureNamesAreKeptAsSent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConfigFeatureNamesAreKeptAsSentTest::RunTest(const FString& Parameters)
{
	FFlockPlaytestConfig Config;
	if (TestTrue(TEXT("Read"), ReadConfig(ConfigWithFeatures(TEXT("{\"video_recording\":true}")), Config)))
	{
		TArray<FString> Names;
		Config.Features.GetKeys(Names);
		if (TestEqual(TEXT("One feature"), Names.Num(), 1))
		{
			TestTrue(TEXT("The name is exactly as the server sent it"),
				Names[0].Equals(TEXT("video_recording"), ESearchCase::CaseSensitive));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConfigUnknownQuestionKindIsKeptTest,
	"Flock.Playtest.Config.UnknownQuestionKindIsKept",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConfigUnknownQuestionKindIsKeptTest::RunTest(const FString& Parameters)
{
	FFlockPlaytestConfig Config;
	if (TestTrue(TEXT("Read"), ReadConfig(ConfigWithFields(
		TEXT("[{\"id\":\"mood\",\"type\":\"slider\",\"label\":\"Mood\"},")
		TEXT("{\"type\":\"text\",\"label\":\"A question without an id\"},")
		TEXT("{\"id\":\"notes\",\"type\":\"textarea\",\"label\":\"Notes\"}]")), Config)))
	{
		if (TestEqual(TEXT("The question without an id is left out, the others kept"), Config.Form.Fields.Num(), 2))
		{
			TestEqual(TEXT("A kind this plugin does not know is kept as sent"), Config.Form.Fields[0].Type, FString(TEXT("slider")));
			TestEqual(TEXT("The question after it is still read"), Config.Form.Fields[1].Id, FString(TEXT("notes")));
		}
	}
	return true;
}

#endif
