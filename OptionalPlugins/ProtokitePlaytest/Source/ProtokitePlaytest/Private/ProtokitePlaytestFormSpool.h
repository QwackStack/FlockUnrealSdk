// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "ProtokitePlaytestFormSubmission.h"

/**
 * Filled-in feedback forms kept on disk until they can be sent.
 *
 * A player answering a form has done the work once; losing it because the network was down, or because they quit before
 * the reply came, would be worse than any delay. So a submission that does not get through is written down and a later
 * launch sends it -- the same bargain the recordings make.
 *
 * One file per submission under `Saved/ProtokitePlaytest/FeedbackForms/`, written through Flock's temporary-file rules so a
 * kill part-way through cannot leave half a form behind. A file is deleted only once the server has taken it, or once
 * it is read back and found to be unsendable.
 */
class FProtokitePlaytestFormSpool
{
public:
	/** Where submissions wait by default. */
	static FString GetDefaultFolder();

	explicit FProtokitePlaytestFormSpool(const FString& InFolder)
		: Folder(InFolder)
	{
	}

	/** Writes one down. Returns its path, or empty with the reason when it could not be written. */
	FString Keep(const FProtokitePlaytestFormSubmission& Submission, FString& OutError) const;

	/** Every submission waiting, oldest first, each with the file it came from. */
	TArray<TPair<FString, FProtokitePlaytestFormSubmission>> FindWaiting() const;

	/** Forgets one, once the server has taken it or once it is past saving. */
	bool Forget(const FString& FilePath) const;

	/** How many are waiting, without reading them. */
	int32 CountWaiting() const;

private:
	FString Folder;
};
