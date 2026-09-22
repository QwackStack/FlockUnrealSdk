// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "ProtokitePlaytestRecordingUpload.h"

#include "Dom/JsonObject.h"

bool FProtokitePlaytestRecordingUploadLink::FromWireObject(const TSharedRef<FJsonObject>& Object,
	FProtokitePlaytestRecordingUploadLink& OutLink, FString& OutError)
{
	OutLink = FProtokitePlaytestRecordingUploadLink();

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
