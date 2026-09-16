// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestRecordingUploads.h"

#include "FlockLogger.h"
#include "FlockPlaytestRecordingUpload.h"
#include "FlockProtokiteClient.h"

namespace
{
	const FString GameVersionHeader = TEXT("X-Game-Version-ID");
}

FFlockPlaytestRecordingUploads::FFlockPlaytestRecordingUploads(const TSharedRef<FFlockProtokiteClient>& InClient,
	const TSharedRef<IFlockFileUploader>& InUploader, const TSharedRef<IFlockLogger>& InLogger)
	: Client(InClient)
	, Uploader(InUploader)
	, Logger(InLogger)
{
}

TMap<FString, FString> FFlockPlaytestRecordingUploads::MakeUploadHeaders(const TMap<FString, FString>& LaunchHeaders,
	const FString& SessionGameVersionId)
{
	TMap<FString, FString> Headers = LaunchHeaders;
	if (!SessionGameVersionId.IsEmpty())
	{
		// The key stays this launch's -- a saved session never holds one -- while the version becomes the session's.
		Headers.Add(GameVersionHeader, SessionGameVersionId);
	}
	return Headers;
}

void FFlockPlaytestRecordingUploads::UploadOne(const TSharedRef<FFlockPlaytestRecordingRun>& Run,
	const FFlockPlaytestRecordingSession& Session, const TMap<FString, FString>& LaunchHeaders,
	TFunction<void(FFlockPlaytestRecordingUploadOutcome)> OnDone)
{
	FFlockPlaytestRecordingUploadOutcome Refused;

	if (Session.IsEmpty() || Session.ProtokiteApiUrl.IsEmpty())
	{
		// No session means nowhere to upload to. The launch pass deletes such a recording; nothing to do here.
		Refused.Error = TEXT("The recording names no Protokite session to upload to.");
		OnDone(Refused);
		return;
	}

	AskForLinkAndSend(Run, Session, MakeUploadHeaders(LaunchHeaders, Session.FlockGameVersionId), 0, MoveTemp(OnDone));
}

void FFlockPlaytestRecordingUploads::AskForLinkAndSend(const TSharedRef<FFlockPlaytestRecordingRun>& Run,
	const FFlockPlaytestRecordingSession& Session, const TMap<FString, FString>& UploadHeaders, int32 AttemptsSoFar,
	TFunction<void(FFlockPlaytestRecordingUploadOutcome)> OnDone)
{
	const FString VideoFilePath = Run->GetVideoFilePath();
	const int32 Attempt = AttemptsSoFar + 1;

	// Held by value so the run outlives the whole chain: nothing may delete a file that is being read off disk, and the
	// lock is what says this launch owns it. Never captures this -- the subsystem can go away mid-upload.
	TWeakPtr<FFlockPlaytestRecordingUploads> WeakSelf = AsShared();

	Client->RequestRecordingUploadLink(Session.ProtokiteApiUrl, UploadHeaders, Session.ProtokiteSessionId,
		FlockPlaytestRecordingContentTypes::WebM,
		[WeakSelf, Run, Session, UploadHeaders, Attempt, VideoFilePath, OnDone]
		(TFlockResult<FFlockPlaytestRecordingUploadLink> Link)
		{
			const TSharedPtr<FFlockPlaytestRecordingUploads> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			FFlockPlaytestRecordingUploadOutcome Outcome;
			Outcome.LinkRequests = Attempt;

			if (!Link.IsSuccess())
			{
				Outcome.Error = FString::Printf(TEXT("Protokite gave nowhere to upload the recording to: %s"),
					*Link.Error.ToDisplayText());
				Self->Logger->LogDebug(Outcome.Error);
				OnDone(Outcome);
				return;
			}

			Self->Uploader->UploadFile(Link.Value.UploadUrl, VideoFilePath, FlockPlaytestRecordingContentTypes::WebM,
				Self->UploadTimeoutSeconds, nullptr,
				[WeakSelf, Run, Session, UploadHeaders, Attempt, OnDone](FFlockFileUploadOutcome Sent)
				{
					const TSharedPtr<FFlockPlaytestRecordingUploads> SelfAgain = WeakSelf.Pin();
					if (!SelfAgain.IsValid())
					{
						return;
					}

					FFlockPlaytestRecordingUploadOutcome Outcome;
					Outcome.LinkRequests = Attempt;

					// Only the PUT's own 2xx says the bytes arrived; the link said only where to put them.
					if (Sent.IsUploaded())
					{
						Outcome.bUploaded = true;

						TArray<FString> FilesLeft;
						Outcome.bRecordingDeleted = Run->DeleteEverything(FilesLeft);
						if (!Outcome.bRecordingDeleted)
						{
							// Uploaded but still on disk: a later launch finds it with no session left to send it to,
							// and deletes it then. Nothing is uploaded twice, because the run is gone from the waiting
							// list either way.
							SelfAgain->Logger->LogDebug(FString::Printf(
								TEXT("The recording was uploaded but %d of its files could not be deleted yet."), FilesLeft.Num()));
						}
						SelfAgain->Logger->LogInfo(FString::Printf(
							TEXT("Playtest recording uploaded for Protokite session %s (%lld bytes)."),
							*Session.ProtokiteSessionId, Sent.BytesSent));
						OnDone(Outcome);
						return;
					}

					Outcome.Error = Sent.ErrorDetail.IsEmpty()
						? FString::Printf(TEXT("The upload failed with status %d."), Sent.StatusCode)
						: FString::Printf(TEXT("The upload failed with status %d: %s"), Sent.StatusCode, *Sent.ErrorDetail);

					if (Attempt < MaxAttempts)
					{
						// A link lives only a few minutes and this one has now been spent, so the retry asks for
						// another rather than sending to the same address again.
						SelfAgain->Logger->LogDebug(FString::Printf(
							TEXT("%s Asking for a fresh link and trying once more."), *Outcome.Error));
						SelfAgain->AskForLinkAndSend(Run, Session, UploadHeaders, Attempt, OnDone);
						return;
					}

					// Kept on disk on purpose: a later launch pushes it.
					SelfAgain->Logger->LogDebug(FString::Printf(
						TEXT("%s The recording is kept for a later launch to upload."), *Outcome.Error));
					OnDone(Outcome);
				});
		});
}

void FFlockPlaytestRecordingUploads::UploadEveryOneWaiting(const FString& RecordingsFolder,
	const TMap<FString, FString>& LaunchHeaders, TFunction<void(int32, int32)> OnDone)
{
	const TArray<FFlockPlaytestRecordingWaitingToUpload> Waiting =
		FFlockPlaytestRecordingsFolder::FindRecordingsWaitingToUpload(RecordingsFolder);

	// Shared counters rather than captured locals: the steps run one after another through completions.
	const TSharedRef<int32> Uploaded = MakeShared<int32>(0);
	const TSharedRef<int32> Left = MakeShared<int32>(0);
	const TSharedRef<int32> Next = MakeShared<int32>(0);
	const TSharedRef<TArray<FFlockPlaytestRecordingWaitingToUpload>> ToUpload =
		MakeShared<TArray<FFlockPlaytestRecordingWaitingToUpload>>(Waiting);

	TWeakPtr<FFlockPlaytestRecordingUploads> WeakSelf = AsShared();

	// Declared first so it can hand itself to the next step: one at a time, never all at once.
	const TSharedRef<TFunction<void()>> UploadNext = MakeShared<TFunction<void()>>();
	*UploadNext = [WeakSelf, ToUpload, Uploaded, Left, Next, LaunchHeaders, UploadNext, OnDone]()
	{
		const TSharedPtr<FFlockPlaytestRecordingUploads> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}

		while (*Next < ToUpload->Num())
		{
			const FFlockPlaytestRecordingWaitingToUpload Recording = (*ToUpload)[(*Next)++];

			// Taking hold is what says no other launch is uploading this one. One it holds is left alone.
			const TSharedPtr<FFlockPlaytestRecordingRun> Run = FFlockPlaytestRecordingRun::ClaimEnded(Recording.RunFolder);
			if (!Run.IsValid())
			{
				++(*Left);
				continue;
			}

			Self->UploadOne(Run.ToSharedRef(), Recording.Session, LaunchHeaders,
				[Uploaded, Left, UploadNext](FFlockPlaytestRecordingUploadOutcome Outcome)
				{
					if (Outcome.bUploaded)
					{
						++(*Uploaded);
					}
					else
					{
						++(*Left);
					}
					(*UploadNext)();
				});
			return;
		}

		OnDone(*Uploaded, *Left);
	};

	(*UploadNext)();
}
