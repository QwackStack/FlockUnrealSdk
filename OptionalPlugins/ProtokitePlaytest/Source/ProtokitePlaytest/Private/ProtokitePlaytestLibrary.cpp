// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "ProtokitePlaytestLibrary.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "ProtokitePlaytestSubsystem.h"

namespace
{
	UProtokitePlaytestSubsystem* FindPlaytestSubsystem(const UObject* WorldContextObject)
	{
		const UGameInstance* GameInstance = Cast<UGameInstance>(WorldContextObject);
		if (GameInstance == nullptr && GEngine != nullptr)
		{
			const UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
			GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;
		}
		return GameInstance != nullptr ? GameInstance->GetSubsystem<UProtokitePlaytestSubsystem>() : nullptr;
	}
}

EProtokitePlaytestStatus UProtokitePlaytestLibrary::GetPlaytestStatus(const UObject* WorldContextObject)
{
	const UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr ? Playtest->GetStatus() : EProtokitePlaytestStatus::TurnedOff;
}

bool UProtokitePlaytestLibrary::IsPlaytestReady(const UObject* WorldContextObject)
{
	return GetPlaytestStatus(WorldContextObject) == EProtokitePlaytestStatus::Ready;
}

FString UProtokitePlaytestLibrary::DescribePlaytestStatus(EProtokitePlaytestStatus Status)
{
	return ::DescribePlaytestStatus(Status);
}

EProtokitePlaytestConsentChoice UProtokitePlaytestLibrary::GetPlaytestConsent(const UObject* WorldContextObject)
{
	const UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	// With no playtest subsystem nothing collects anything, which is what an unanswered question means too.
	return Playtest != nullptr ? Playtest->GetPlaytestConsent() : EProtokitePlaytestConsentChoice::NotAnswered;
}

EProtokitePlaytestConsentChoice UProtokitePlaytestLibrary::GetPlayersConsentAnswer(const UObject* WorldContextObject)
{
	const UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr ? Playtest->GetPlayersConsentAnswer() : EProtokitePlaytestConsentChoice::NotAnswered;
}

FString UProtokitePlaytestLibrary::DescribePlaytestConsent(EProtokitePlaytestConsentChoice Choice)
{
	return ProtokitePlaytestConsent::Describe(Choice);
}

bool UProtokitePlaytestLibrary::SetPlaytestConsent(const UObject* WorldContextObject, EProtokitePlaytestConsentChoice Choice)
{
	UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->SetPlaytestConsent(Choice);
}

bool UProtokitePlaytestLibrary::AskForPlaytestConsent(const UObject* WorldContextObject)
{
	UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->AskForPlaytestConsent();
}

bool UProtokitePlaytestLibrary::IsConsentQuestionOpen(const UObject* WorldContextObject)
{
	const UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->IsConsentQuestionOpen();
}

bool UProtokitePlaytestLibrary::IsPlaytestFeatureEnabled(const UObject* WorldContextObject, const FString& FeatureName)
{
	const UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->IsPlaytestFeatureEnabled(FeatureName);
}

FString UProtokitePlaytestLibrary::GetPlaytestSessionId(const UObject* WorldContextObject)
{
	const UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr ? Playtest->GetPlaytestSessionId() : FString();
}

bool UProtokitePlaytestLibrary::EndPlaytestSession(const UObject* WorldContextObject)
{
	UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->EndPlaytestSession();
}

bool UProtokitePlaytestLibrary::RecordPlaytestEvent(const UObject* WorldContextObject, const FString& EventName,
	const FFlockCommandData& Properties)
{
	UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->RecordPlaytestEvent(EventName, Properties);
}

bool UProtokitePlaytestLibrary::StopVideoRecording(const UObject* WorldContextObject)
{
	UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->StopVideoRecording();
}

bool UProtokitePlaytestLibrary::IsRecordingVideo(const UObject* WorldContextObject)
{
	const UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->IsRecordingVideo();
}

bool UProtokitePlaytestLibrary::CanSendPlaytestRecording(const UObject* WorldContextObject)
{
	const UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->CanSendTheRecording();
}

bool UProtokitePlaytestLibrary::StopAndUploadPlaytestRecording(const UObject* WorldContextObject)
{
	UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->StopVideoRecordingAndUploadIt();
}

bool UProtokitePlaytestLibrary::CanOpenFeedbackForm(const UObject* WorldContextObject)
{
	const UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->CanOpenFeedbackForm();
}

bool UProtokitePlaytestLibrary::OpenFeedbackForm(const UObject* WorldContextObject)
{
	UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->OpenFeedbackForm();
}

bool UProtokitePlaytestLibrary::CloseFeedbackForm(const UObject* WorldContextObject)
{
	UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->CloseFeedbackForm();
}

bool UProtokitePlaytestLibrary::IsFeedbackFormOpen(const UObject* WorldContextObject)
{
	const UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->IsFeedbackFormOpen();
}

bool UProtokitePlaytestLibrary::GetFeedbackForm(const UObject* WorldContextObject, FProtokitePlaytestForm& Form)
{
	// The same test the built-in form opens on, so a game's own form is offered exactly when that one would be.
	const UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	if (Playtest == nullptr || !Playtest->CanOpenFeedbackForm())
	{
		Form = FProtokitePlaytestForm();
		return false;
	}
	Form = Playtest->GetPlaytestConfig().Form;
	return true;
}

bool UProtokitePlaytestLibrary::SendFeedbackFormAnswers(const UObject* WorldContextObject, const FProtokitePlaytestFormAnswers& Answers)
{
	UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->SendFilledInForm(Answers);
}

TArray<FProtokitePlaytestFormProblem> UProtokitePlaytestLibrary::FindFeedbackFormProblems(const FProtokitePlaytestFormAnswers& Answers,
	const FProtokitePlaytestForm& Form)
{
	return Answers.FindProblems(Form);
}

FProtokitePlaytestFormAnswers UProtokitePlaytestLibrary::SetFeedbackTextAnswer(const FProtokitePlaytestFormAnswers& Answers,
	const FString& FieldId, const FString& Text)
{
	FProtokitePlaytestFormAnswers Updated = Answers;
	Updated.SetText(FieldId, Text);
	return Updated;
}

FProtokitePlaytestFormAnswers UProtokitePlaytestLibrary::SetFeedbackRatingAnswer(const FProtokitePlaytestFormAnswers& Answers,
	const FString& FieldId, int32 Rating)
{
	FProtokitePlaytestFormAnswers Updated = Answers;
	Updated.SetRating(FieldId, Rating);
	return Updated;
}

FProtokitePlaytestFormAnswers UProtokitePlaytestLibrary::SetFeedbackCheckboxAnswer(const FProtokitePlaytestFormAnswers& Answers,
	const FString& FieldId, bool bChecked)
{
	FProtokitePlaytestFormAnswers Updated = Answers;
	Updated.SetChecked(FieldId, bChecked);
	return Updated;
}

FProtokitePlaytestFormAnswers UProtokitePlaytestLibrary::SetFeedbackChosenOption(const FProtokitePlaytestFormAnswers& Answers,
	const FString& FieldId, const FString& Option)
{
	FProtokitePlaytestFormAnswers Updated = Answers;
	Updated.SetChosenOption(FieldId, Option);
	return Updated;
}
