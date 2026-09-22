// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "ProtokitePlaytestFormAnswers.h"
#include "ProtokitePlaytestFormSpool.h"
#include "ProtokitePlaytestFormSubmission.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	FProtokitePlaytestFormField MakeSubmitField(const FString& Id, const TCHAR* Type, bool bRequired = false,
		const TArray<FString>& Options = {})
	{
		FProtokitePlaytestFormField Field;
		Field.Id = Id;
		Field.Type = Type;
		Field.Required = bRequired;
		Field.Options = Options;
		return Field;
	}

	/** A form with one question of every kind that behaves differently on the wire. */
	FProtokitePlaytestForm MixedForm()
	{
		FProtokitePlaytestForm Form;
		Form.Id = TEXT("form-1");
		Form.Fields = {
			MakeSubmitField(TEXT("score"), ProtokitePlaytestFormFieldTypes::Rating),
			MakeSubmitField(TEXT("contact_me"), ProtokitePlaytestFormFieldTypes::Checkbox),
			MakeSubmitField(TEXT("which"), ProtokitePlaytestFormFieldTypes::Select, false, {TEXT("Docks"), TEXT("Tower")}),
			MakeSubmitField(TEXT("notes"), ProtokitePlaytestFormFieldTypes::TextArea),
			MakeSubmitField(TEXT("left_blank"), ProtokitePlaytestFormFieldTypes::Text),
		};
		return Form;
	}

	TSharedPtr<FJsonObject> Parse(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	FString ToJsonString(const TSharedRef<FJsonObject>& Object)
	{
		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Object, Writer);
		return Json;
	}

	struct FSpoolTestFolder
	{
		FString Folder = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(),
			TEXT("ProtokitePlaytestTests"), FString::Printf(TEXT("Forms-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
		~FSpoolTestFolder() { IFileManager::Get().DeleteDirectory(*Folder, false, true); }
	};
}

/** Each kind goes on the wire as the server stores it, and an unanswered optional question is left out entirely. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormWireShapeTest,
	"Protokite.Playtest.Form.AnswersGoOnTheWireAsTheServerKeepsThem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormWireShapeTest::RunTest(const FString& Parameters)
{
	const FProtokitePlaytestForm Form = MixedForm();

	FProtokitePlaytestFormAnswers Answers;
	Answers.SetRating(TEXT("score"), 4);
	Answers.SetChecked(TEXT("contact_me"), false);
	Answers.SetChosenOption(TEXT("which"), TEXT("  Tower  "));
	Answers.SetText(TEXT("notes"), TEXT("  It crashed on the stairs.  "));
	// left_blank is never touched.

	const TSharedPtr<FJsonObject> Wire = Parse(ToJsonString(Answers.ToWireObject(Form)));
	if (!TestTrue(TEXT("It is an object"), Wire.IsValid()))
	{
		return false;
	}

	double Score = 0.0;
	TestTrue(TEXT("A rating is a number"), Wire->TryGetNumberField(TEXT("score"), Score));
	TestEqual(TEXT("With its value"), static_cast<int32>(Score), 4);

	bool bContact = true;
	TestTrue(TEXT("A tickbox is a bool"), Wire->TryGetBoolField(TEXT("contact_me"), bContact));
	TestFalse(TEXT("Unticked, and sent as false rather than left out"), bContact);

	FString Which;
	TestTrue(TEXT("A select is text"), Wire->TryGetStringField(TEXT("which"), Which));
	TestEqualSensitive(TEXT("Trimmed, as the server keeps it"), Which, FString(TEXT("Tower")));

	FString Notes;
	Wire->TryGetStringField(TEXT("notes"), Notes);
	TestEqual(TEXT("And so is a textarea, trimmed"), Notes, FString(TEXT("It crashed on the stairs.")));

	// The one that matters: an optional question nobody answered is absent, not an empty string, or the server would
	// record an answer the player never gave.
	TestFalse(TEXT("An unanswered optional question is left out"), Wire->HasField(TEXT("left_blank")));
	return true;
}

/** The request body names the session, which is what makes a second send replace the first rather than add one. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormBodyTest,
	"Protokite.Playtest.Form.TheRequestNamesTheSessionAndWhoFilledItIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormBodyTest::RunTest(const FString& Parameters)
{
	FProtokitePlaytestFormSubmission Submission;
	Submission.PlaytestSessionId = TEXT("01KX0SESSION");
	Submission.Identity.DeviceId = TEXT("device-1");
	Submission.AnswersJson = TEXT("{\"score\":5}");
	Submission.ProtokiteApiUrl = TEXT("http://127.0.0.1:8020");

	const TSharedPtr<FJsonObject> Body = Parse(Submission.ToJson());
	if (!TestTrue(TEXT("The body parses"), Body.IsValid()))
	{
		return false;
	}
	FString SessionId, DeviceId;
	Body->TryGetStringField(TEXT("session_id"), SessionId);
	Body->TryGetStringField(TEXT("device_id"), DeviceId);
	TestEqual(TEXT("It names the session"), SessionId, FString(TEXT("01KX0SESSION")));
	TestEqual(TEXT("And who filled it in"), DeviceId, FString(TEXT("device-1")));
	TestFalse(TEXT("And claims no Steam id it does not have"), Body->HasField(TEXT("steam_id")));

	TestTrue(TEXT("Naming a session, sending it again replaces rather than adds"), Submission.CanBeSentAgainSafely());

	// Without a session the server has nothing to match on, so every send makes another row and a retry must not happen.
	Submission.PlaytestSessionId.Empty();
	TestFalse(TEXT("With no session it must not be sent again"), Submission.CanBeSentAgainSafely());
	TestFalse(TEXT("And the body leaves the session out"), Parse(Submission.ToJson())->HasField(TEXT("session_id")));
	return true;
}

/** A form that could not be sent is kept, read back whole by a later launch, and forgotten once it is taken. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormSpoolTest,
	"Protokite.Playtest.Form.AFormThatCannotBeSentIsKeptForALaterLaunch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormSpoolTest::RunTest(const FString& Parameters)
{
	FSpoolTestFolder Scratch;
	const FProtokitePlaytestFormSpool Spool(Scratch.Folder);

	FProtokitePlaytestFormSubmission Submission;
	Submission.PlaytestSessionId = TEXT("01KX0SESSION");
	Submission.Identity.SteamId = TEXT("steam-9");
	Submission.AnswersJson = TEXT("{\"score\":3,\"notes\":\"the lift\"}");
	Submission.ProtokiteApiUrl = TEXT("http://127.0.0.1:8020");
	Submission.FlockGameVersionId = TEXT("the-session-version");

	FString Error;
	const FString KeptAt = Spool.Keep(Submission, Error);
	if (!TestFalse(FString::Printf(TEXT("It is kept (%s)"), *Error), KeptAt.IsEmpty()))
	{
		return false;
	}

	// A later launch, reading the folder fresh.
	const TArray<TPair<FString, FProtokitePlaytestFormSubmission>> Waiting = FProtokitePlaytestFormSpool(Scratch.Folder).FindWaiting();
	if (!TestEqual(TEXT("One is waiting"), Waiting.Num(), 1))
	{
		return false;
	}
	const FProtokitePlaytestFormSubmission& Read = Waiting[0].Value;
	TestEqual(TEXT("Its session"), Read.PlaytestSessionId, FString(TEXT("01KX0SESSION")));
	TestEqual(TEXT("Who filled it in"), Read.Identity.SteamId, FString(TEXT("steam-9")));
	TestEqual(TEXT("Where it goes"), Read.ProtokiteApiUrl, FString(TEXT("http://127.0.0.1:8020")));
	TestEqual(TEXT("And the version its session ran under"), Read.FlockGameVersionId, FString(TEXT("the-session-version")));
	TestTrue(TEXT("Its answers survived"), Read.AnswersJson.Contains(TEXT("the lift")));

	// Nothing secret is written to a player's disk.
	FString OnDisk;
	FFileHelper::LoadFileToString(OnDisk, *KeptAt);
	TestFalse(TEXT("It holds no API key"), OnDisk.Contains(TEXT("api_key"), ESearchCase::IgnoreCase));

	TestTrue(TEXT("Once taken, it stops waiting"), Spool.Forget(Waiting[0].Key));
	TestEqual(TEXT("And nothing is left"), FProtokitePlaytestFormSpool(Scratch.Folder).CountWaiting(), 0);
	return true;
}

/** One that can never be sent is dropped rather than tried at every launch for ever. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormSpoolDropsUnsendableTest,
	"Protokite.Playtest.Form.AKeptFormNobodyCanBeAttributedToIsDropped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormSpoolDropsUnsendableTest::RunTest(const FString& Parameters)
{
	FSpoolTestFolder Scratch;
	IFileManager::Get().MakeDirectory(*Scratch.Folder, true);

	// Neither a Steam id nor a device id: the server refuses it, so trying it again would never do anything.
	const FString Path = FPaths::Combine(Scratch.Folder, TEXT("20260916-120000-aaaaaaaa.json"));
	FFileHelper::SaveStringToFile(
		TEXT("{\"playtest_session_id\":\"s\",\"steam_id\":\"\",\"device_id\":\"\",")
		TEXT("\"protokite_api_url\":\"http://127.0.0.1:8020\",\"answers\":{\"a\":1}}"), *Path);

	const TArray<TPair<FString, FProtokitePlaytestFormSubmission>> Waiting = FProtokitePlaytestFormSpool(Scratch.Folder).FindWaiting();
	TestEqual(TEXT("It is not offered for sending"), Waiting.Num(), 0);
	TestFalse(TEXT("And it is gone, not left to be read again for ever"), IFileManager::Get().FileExists(*Path));
	return true;
}

/**
 * Protokite names the question a refusal is about inside its message and nowhere else, so the id is read out of the
 * sentence or the complaint cannot be shown against the question it belongs to.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormComplaintFieldTest,
	"Protokite.Playtest.Form.ReadsTheQuestionOutOfAServerComplaint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormComplaintFieldTest::RunTest(const FString& Parameters)
{
	// The server's own wording, copied from its validator.
	TestEqual(TEXT("A missing answer"),
		ProtokitePlaytestFindFieldIdInComplaint(TEXT("Missing required answer 'steps'")), FString(TEXT("steps")));
	TestEqual(TEXT("A bad option"),
		ProtokitePlaytestFindFieldIdInComplaint(TEXT("Invalid option for 'which_level'")), FString(TEXT("which_level")));
	TestEqual(TEXT("A bad rating"),
		ProtokitePlaytestFindFieldIdInComplaint(TEXT("Rating 'score' must be 1-5")), FString(TEXT("score")));

	// A refusal about the whole submission names no question, and must not be pinned on one.
	TestEqual(TEXT("A general refusal names nothing"),
		ProtokitePlaytestFindFieldIdInComplaint(TEXT("Provide steam_id or device_id so the response can be tied to a Flock player")),
		FString());
	TestEqual(TEXT("Nor does an empty message"), ProtokitePlaytestFindFieldIdInComplaint(FString()), FString());
	return true;
}

#endif // WITH_AUTOMATION_TESTS
