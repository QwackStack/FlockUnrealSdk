// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockPlaytestRecordingUpload.generated.h"

/** The content type a recording is written and uploaded as. The link is signed for it, so the PUT must carry it. */
namespace FlockPlaytestRecordingContentTypes
{
	constexpr const TCHAR* WebM = TEXT("video/webm");
}

/**
 * Protokite's answer to a request for somewhere to put a recording.
 *
 * **A link is not an upload.** Protokite counts the session as having a recording from the moment it issues one, so
 * reading this answer as success would show a recording on the session that no bytes ever reached. Only the PUT's own
 * 2xx says the recording arrived.
 *
 * The link is short-lived (about 500 seconds), which is why it is asked for only once a recording is finished and is
 * never kept for a later attempt: a retry asks for a fresh one.
 */
USTRUCT()
struct FLOCKPLAYTEST_API FFlockPlaytestRecordingUploadLink
{
	GENERATED_BODY()

	/** Where to PUT the recording. Presigned, so it carries its own credentials and none of ours. */
	UPROPERTY()
	FString UploadUrl;

	/** Where Protokite put it, for the log only; nothing is addressed by these. */
	UPROPERTY()
	FString Bucket;

	UPROPERTY()
	FString Key;

	/** Fails when upload_url is missing or empty: there is nowhere to send the recording, which is not a usable link. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPlaytestRecordingUploadLink& OutLink, FString& OutError);
};
