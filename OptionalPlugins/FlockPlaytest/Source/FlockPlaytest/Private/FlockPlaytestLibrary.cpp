// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestLibrary.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FlockPlaytestSubsystem.h"

namespace
{
	UFlockPlaytestSubsystem* FindPlaytestSubsystem(const UObject* WorldContextObject)
	{
		const UGameInstance* GameInstance = Cast<UGameInstance>(WorldContextObject);
		if (GameInstance == nullptr && GEngine != nullptr)
		{
			const UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
			GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;
		}
		return GameInstance != nullptr ? GameInstance->GetSubsystem<UFlockPlaytestSubsystem>() : nullptr;
	}
}

EFlockPlaytestStatus UFlockPlaytestLibrary::GetPlaytestStatus(const UObject* WorldContextObject)
{
	const UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr ? Playtest->GetStatus() : EFlockPlaytestStatus::TurnedOff;
}

bool UFlockPlaytestLibrary::IsPlaytestReady(const UObject* WorldContextObject)
{
	return GetPlaytestStatus(WorldContextObject) == EFlockPlaytestStatus::Ready;
}

FString UFlockPlaytestLibrary::DescribePlaytestStatus(EFlockPlaytestStatus Status)
{
	return ::DescribePlaytestStatus(Status);
}

bool UFlockPlaytestLibrary::IsPlaytestFeatureEnabled(const UObject* WorldContextObject, const FString& FeatureName)
{
	const UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->IsPlaytestFeatureEnabled(FeatureName);
}

FString UFlockPlaytestLibrary::GetPlaytestSessionId(const UObject* WorldContextObject)
{
	const UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr ? Playtest->GetPlaytestSessionId() : FString();
}

bool UFlockPlaytestLibrary::EndPlaytestSession(const UObject* WorldContextObject)
{
	UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->EndPlaytestSession();
}

bool UFlockPlaytestLibrary::RecordPlaytestEvent(const UObject* WorldContextObject, const FString& EventName,
	const FFlockCommandData& Properties)
{
	UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->RecordPlaytestEvent(EventName, Properties);
}

bool UFlockPlaytestLibrary::StopVideoRecording(const UObject* WorldContextObject)
{
	UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->StopVideoRecording();
}

bool UFlockPlaytestLibrary::IsRecordingVideo(const UObject* WorldContextObject)
{
	const UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->IsRecordingVideo();
}

bool UFlockPlaytestLibrary::CanSendPlaytestRecording(const UObject* WorldContextObject)
{
	const UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->CanSendTheRecording();
}

bool UFlockPlaytestLibrary::StopAndUploadPlaytestRecording(const UObject* WorldContextObject)
{
	UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->StopVideoRecordingAndUploadIt();
}

bool UFlockPlaytestLibrary::CanOpenFeedbackForm(const UObject* WorldContextObject)
{
	const UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->CanOpenFeedbackForm();
}

bool UFlockPlaytestLibrary::OpenFeedbackForm(const UObject* WorldContextObject)
{
	UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->OpenFeedbackForm();
}

bool UFlockPlaytestLibrary::CloseFeedbackForm(const UObject* WorldContextObject)
{
	UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->CloseFeedbackForm();
}

bool UFlockPlaytestLibrary::IsFeedbackFormOpen(const UObject* WorldContextObject)
{
	const UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->IsFeedbackFormOpen();
}

bool UFlockPlaytestLibrary::GetFeedbackForm(const UObject* WorldContextObject, FFlockPlaytestForm& Form)
{
	// The same test the built-in form opens on, so a game's own form is offered exactly when that one would be.
	const UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	if (Playtest == nullptr || !Playtest->CanOpenFeedbackForm())
	{
		Form = FFlockPlaytestForm();
		return false;
	}
	Form = Playtest->GetPlaytestConfig().Form;
	return true;
}

bool UFlockPlaytestLibrary::SendFeedbackFormAnswers(const UObject* WorldContextObject, const FFlockPlaytestFormAnswers& Answers)
{
	UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystem(WorldContextObject);
	return Playtest != nullptr && Playtest->SendFilledInForm(Answers);
}

TArray<FFlockPlaytestFormProblem> UFlockPlaytestLibrary::FindFeedbackFormProblems(const FFlockPlaytestFormAnswers& Answers,
	const FFlockPlaytestForm& Form)
{
	return Answers.FindProblems(Form);
}

FFlockPlaytestFormAnswers UFlockPlaytestLibrary::SetFeedbackTextAnswer(const FFlockPlaytestFormAnswers& Answers,
	const FString& FieldId, const FString& Text)
{
	FFlockPlaytestFormAnswers Updated = Answers;
	Updated.SetText(FieldId, Text);
	return Updated;
}

FFlockPlaytestFormAnswers UFlockPlaytestLibrary::SetFeedbackRatingAnswer(const FFlockPlaytestFormAnswers& Answers,
	const FString& FieldId, int32 Rating)
{
	FFlockPlaytestFormAnswers Updated = Answers;
	Updated.SetRating(FieldId, Rating);
	return Updated;
}

FFlockPlaytestFormAnswers UFlockPlaytestLibrary::SetFeedbackCheckboxAnswer(const FFlockPlaytestFormAnswers& Answers,
	const FString& FieldId, bool bChecked)
{
	FFlockPlaytestFormAnswers Updated = Answers;
	Updated.SetChecked(FieldId, bChecked);
	return Updated;
}

FFlockPlaytestFormAnswers UFlockPlaytestLibrary::SetFeedbackChosenOption(const FFlockPlaytestFormAnswers& Answers,
	const FString& FieldId, const FString& Option)
{
	FFlockPlaytestFormAnswers Updated = Answers;
	Updated.SetChosenOption(FieldId, Option);
	return Updated;
}
