// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "ProtokitePlaytestFormSpool.h"

#include "ProtokitePlaytestLogger.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockTemporaryFiles.h"
#include "Misc/Paths.h"

namespace
{
	const FString FormFileExtension = TEXT(".json");
}

FString FProtokitePlaytestFormSpool::GetDefaultFolder()
{
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ProtokitePlaytest"), TEXT("FeedbackForms")));
}

FString FProtokitePlaytestFormSpool::Keep(const FProtokitePlaytestFormSubmission& Submission, FString& OutError) const
{
	IFileManager::Get().MakeDirectory(*Folder, /*Tree*/ true);

	// Named for when it was written and then made unique, so several forms from one launch keep their order and none
	// lands on another's name.
	const FString Name = FString::Printf(TEXT("%s-%s%s"),
		*FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S")),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8),
		*FormFileExtension);
	const FString Path = FPaths::Combine(Folder, Name);

	// Through the temporary-file rules, so a kill between writing and committing cannot leave half a form to be read
	// back as a whole one.
	if (!FFlockTemporaryFiles::SaveThenMove(Submission.ToSavedJson(), Path))
	{
		OutError = FString::Printf(TEXT("The feedback form could not be written to '%s'."), *Path);
		return FString();
	}
	return Path;
}

TArray<TPair<FString, FProtokitePlaytestFormSubmission>> FProtokitePlaytestFormSpool::FindWaiting() const
{
	TArray<TPair<FString, FProtokitePlaytestFormSubmission>> Waiting;

	TArray<FString> Names;
	IFileManager::Get().FindFiles(Names, *FPaths::Combine(Folder, TEXT("*") + FormFileExtension), /*Files*/ true, /*Directories*/ false);
	// Oldest first: the names start with the time they were written, so sorting them sorts by age.
	Names.Sort();

	for (const FString& Name : Names)
	{
		const FString Path = FPaths::Combine(Folder, Name);
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *Path))
		{
			continue;
		}

		FProtokitePlaytestFormSubmission Submission;
		FString Error;
		if (!FProtokitePlaytestFormSubmission::FromSavedJson(Json, Submission, Error))
		{
			// Unreadable or missing what the server needs: keeping it would mean trying it forever, so it goes.
			UE_LOG(LogProtokitePlaytest, Warning, TEXT("A kept feedback form is being dropped: %s"), *Error);
			IFileManager::Get().Delete(*Path, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
			continue;
		}
		Waiting.Add(TPair<FString, FProtokitePlaytestFormSubmission>(Path, Submission));
	}

	return Waiting;
}

bool FProtokitePlaytestFormSpool::Forget(const FString& FilePath) const
{
	return IFileManager::Get().Delete(*FilePath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
}

int32 FProtokitePlaytestFormSpool::CountWaiting() const
{
	TArray<FString> Names;
	IFileManager::Get().FindFiles(Names, *FPaths::Combine(Folder, TEXT("*") + FormFileExtension), /*Files*/ true, /*Directories*/ false);
	return Names.Num();
}
