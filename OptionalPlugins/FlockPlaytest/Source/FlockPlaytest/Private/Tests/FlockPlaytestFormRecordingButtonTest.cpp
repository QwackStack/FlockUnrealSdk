// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "SFlockPlaytestFormWidget.h"

namespace
{
	FFlockPlaytestForm OneQuestionForm()
	{
		FFlockPlaytestFormField Field;
		Field.Id = TEXT("notes");
		Field.Type = FlockPlaytestFormFieldTypes::Text;
		FFlockPlaytestForm Form;
		Form.Id = TEXT("form-1");
		Form.Fields = {Field};
		return Form;
	}
}

/**
 * The recording button is offered only while there is a recording to send, and asking is the player's own choice --
 * opening the form stops nothing by itself (D7, as the owner reversed it).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestFormRecordingButtonTest,
	"Flock.Playtest.Form.OffersTheRecordingOnlyWhileOneIsRunning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestFormRecordingButtonTest::RunTest(const FString& Parameters)
{
	bool bCanSend = true;
	int32 TimesAsked = 0;
	bool bSendingBegan = true;

	const TSharedRef<SFlockPlaytestFormWidget> Widget = SNew(SFlockPlaytestFormWidget)
		.Form(OneQuestionForm())
		.CanSendRecording_Lambda([&bCanSend]() { return bCanSend; })
		.OnSendRecording_Lambda([&TimesAsked, &bSendingBegan]() { ++TimesAsked; return bSendingBegan; });

	TestEqual(TEXT("Opening the form asks for nothing on its own"), TimesAsked, 0);

	// Nothing to send: no recording, or one no playtest session started for, which cannot be uploaded at all.
	bCanSend = false;
	TestTrue(TEXT("Nothing is offered when the recording has nowhere to go"),
		Widget->GetSendRecordingVisibilityForTesting() == EVisibility::Collapsed);

	bCanSend = true;
	TestTrue(TEXT("It is offered when it has"),
		Widget->GetSendRecordingVisibilityForTesting() == EVisibility::Visible);

	Widget->SendTheRecordingForTesting();
	TestEqual(TEXT("Asking sends it"), TimesAsked, 1);

	// One recording a launch, so asking again could only send the same video: the offer goes and a note takes its place.
	TestTrue(TEXT("And the offer is gone"),
		Widget->GetSendRecordingVisibilityForTesting() == EVisibility::Collapsed);
	TestTrue(TEXT("Replaced by a word that it is on its way"),
		Widget->GetRecordingOnItsWayVisibilityForTesting() == EVisibility::Visible);
	return true;
}

/**
 * The note must never claim a video is on its way when sending did not begin. It said so unconditionally once, and a
 * player whose recording had no session to go to was told it had been sent while nothing happened at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestFormNoFalsePromiseTest,
	"Flock.Playtest.Form.SaysNothingIsOnItsWayWhenSendingDidNotBegin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestFormNoFalsePromiseTest::RunTest(const FString& Parameters)
{
	const TSharedRef<SFlockPlaytestFormWidget> Widget = SNew(SFlockPlaytestFormWidget)
		.Form(OneQuestionForm())
		.CanSendRecording_Lambda([]() { return true; })
		// Asked for, but sending could not begin.
		.OnSendRecording_Lambda([]() { return false; });

	Widget->SendTheRecordingForTesting();
	TestTrue(TEXT("The offer is gone, because it was asked for"),
		Widget->GetSendRecordingVisibilityForTesting() == EVisibility::Collapsed);
	TestTrue(TEXT("But nothing claims the video is on its way"),
		Widget->GetRecordingOnItsWayVisibilityForTesting() == EVisibility::Collapsed);
	return true;
}

/** A form on a playtest that records nothing never offers the button at all. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestFormNoRecordingButtonTest,
	"Flock.Playtest.Form.OffersNoRecordingWhenNothingIsRecorded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestFormNoRecordingButtonTest::RunTest(const FString& Parameters)
{
	// Nothing bound at all, which is how a form built without the recording half arrives.
	const TSharedRef<SFlockPlaytestFormWidget> Widget = SNew(SFlockPlaytestFormWidget).Form(OneQuestionForm());
	TestTrue(TEXT("Nothing is offered"),
		Widget->GetSendRecordingVisibilityForTesting() == EVisibility::Collapsed);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
