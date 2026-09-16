// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestRecordingUpload.h"

#include "Dom/JsonObject.h"

bool FFlockPlaytestRecordingUploadLink::FromWireObject(const TSharedRef<FJsonObject>& Object,
	FFlockPlaytestRecordingUploadLink& OutLink, FString& OutError)
{
	OutLink = FFlockPlaytestRecordingUploadLink();

	FString UploadUrl;
	Object->TryGetStringField(TEXT("upload_url"), UploadUrl);
	if (UploadUrl.TrimStartAndEnd().IsEmpty())
	{
		// Reading this as a usable answer would leave the session showing a recording that nothing could be sent to.
		OutError = TEXT("Protokite's answer names nowhere to upload the recording to.");
		return false;
	}

	OutLink.UploadUrl = UploadUrl;
	// The bucket and key are reported, not addressed by, so an answer without them is still usable.
	Object->TryGetStringField(TEXT("bucket"), OutLink.Bucket);
	Object->TryGetStringField(TEXT("key"), OutLink.Key);
	return true;
}
