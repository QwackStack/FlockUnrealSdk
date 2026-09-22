// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "ProtokitePlaytestRecordingsFolder.h"
#include "Http/FlockFileUploader.h"

class FProtokiteClient;
class IFlockLogger;

/** What became of one recording's upload. */
struct FProtokitePlaytestRecordingUploadOutcome
{
	/** True only when storage answered the PUT with a 2xx. A link on its own is never this. */
	bool bUploaded = false;

	/** True when the recording was deleted after being uploaded. */
	bool bRecordingDeleted = false;

	/** How many times a link was asked for. Two means the first attempt failed and a fresh link was used. */
	int32 LinkRequests = 0;

	/** Empty when the recording was uploaded. */
	FString Error;
};

/**
 * Sends finished recordings to Protokite: a link for each, then the file itself, then the recording is deleted.
 *
 * **A link is not an upload.** Protokite counts a session as having a recording from the moment it issues a link, so
 * every success here is decided by the PUT's own 2xx and never by the link request's. Getting that backwards would
 * look right on the dashboard and lose the video.
 *
 * **A link is never kept.** It is signed for a few minutes, so one is asked for immediately before each attempt and a
 * retry asks for another rather than reusing the one that just failed.
 *
 * **A recording is deleted once it is uploaded, and kept otherwise**, so a launch that could not send it leaves it for
 * a later one to push. That is why a failure here is not an error a player ever sees.
 *
 * The run is held for the length of its upload: nothing else may delete a file being read off disk.
 */
class FProtokitePlaytestRecordingUploads : public TSharedFromThis<FProtokitePlaytestRecordingUploads>
{
public:
	FProtokitePlaytestRecordingUploads(const TSharedRef<FProtokiteClient>& InClient,
		const TSharedRef<IFlockFileUploader>& InUploader, const TSharedRef<IFlockLogger>& InLogger);

	/**
	 * The headers one recording's upload is asked for with: this launch's own API key, and the Game Version ID the
	 * **session** started with rather than this launch's.
	 *
	 * Pure so the swap is testable. Protokite finds a session's playtest from the version id, so sending this launch's
	 * would resolve a recording to the wrong playtest -- and every test run from a single launch, where the two ids
	 * happen to be equal, would pass anyway.
	 */
	static TMap<FString, FString> MakeUploadHeaders(const TMap<FString, FString>& LaunchHeaders,
		const FString& SessionGameVersionId);

	/**
	 * Uploads one finished recording whose run this already holds, then deletes it on success.
	 *
	 * The run is kept alive until OnDone, so the caller may let go of its own reference.
	 */
	void UploadOne(const TSharedRef<FProtokitePlaytestRecordingRun>& Run, const FProtokitePlaytestRecordingSession& Session,
		const TMap<FString, FString>& LaunchHeaders, TFunction<void(FProtokitePlaytestRecordingUploadOutcome)> OnDone);

	/**
	 * Takes hold of every recording an earlier launch left waiting and uploads them one after another, never at the
	 * same time: they share one connection and one disk budget, and a failure part-way should leave the rest for a
	 * later launch rather than half-sending all of them.
	 *
	 * A run another launch still holds is skipped, not waited for.
	 */
	void UploadEveryOneWaiting(const FString& RecordingsFolder, const TMap<FString, FString>& LaunchHeaders,
		TFunction<void(int32 UploadedCount, int32 LeftCount)> OnDone);

	/** How long a single recording may take to send. Zero means no limit, which is the default for a whole recording. */
	float UploadTimeoutSeconds = 0.f;

private:
	void AskForLinkAndSend(const TSharedRef<FProtokitePlaytestRecordingRun>& Run, const FProtokitePlaytestRecordingSession& Session,
		const TMap<FString, FString>& UploadHeaders, int32 AttemptsSoFar,
		TFunction<void(FProtokitePlaytestRecordingUploadOutcome)> OnDone);

	/** One retry, with a link of its own. A second failure leaves the recording for a later launch. */
	static constexpr int32 MaxAttempts = 2;

	TSharedRef<FProtokiteClient> Client;
	TSharedRef<IFlockFileUploader> Uploader;
	TSharedRef<IFlockLogger> Logger;
};
