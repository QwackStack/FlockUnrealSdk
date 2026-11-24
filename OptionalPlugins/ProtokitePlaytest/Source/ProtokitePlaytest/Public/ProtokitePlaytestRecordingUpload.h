// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "ProtokitePlaytestRecordingUpload.generated.h"

/** The content type a recording is written and uploaded as. The link is signed for it, so the PUT must carry it. */
namespace ProtokitePlaytestRecordingContentTypes
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
struct PROTOKITEPLAYTEST_API FProtokitePlaytestRecordingUploadLink
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
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FProtokitePlaytestRecordingUploadLink& OutLink, FString& OutError);
};

/** Where a playtest recording's upload stands, as OnRecordingUploadChanged reports it. */
UENUM(BlueprintType)
enum class EProtokitePlaytestUploadState : uint8
{
	/** The upload began: a link is being asked for, and the file follows. */
	Started,

	/** Every byte arrived, and the recording is no longer kept on disk. BytesSent says how much was sent. */
	Uploaded,

	/** It did not go, or could not begin, and stays on disk for a later chance. WhyNot says why. */
	Kept,

	/** It waits on disk for a Wi-Fi network. Never raised in this plugin: it records only on 64-bit Windows, which has no such choice. */
	WaitingForWiFi,
};

/** One change in a playtest recording's upload, for this launch's recordings and those an earlier launch left. */
USTRUCT(BlueprintType)
struct PROTOKITEPLAYTEST_API FProtokitePlaytestUploadUpdate
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	EProtokitePlaytestUploadState State = EProtokitePlaytestUploadState::Started;

	/** The Protokite session the recording goes to. Empty while it waits for a session to start. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString PlaytestSessionId;

	/** True for a recording an earlier launch left on disk, sent by this one. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	bool bFromAnEarlierLaunch = false;

	/** How many bytes were sent. Set when State is Uploaded; 0 otherwise. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	int64 BytesSent = 0;

	/** Why it is kept or waiting. Set when State is Kept or Waiting For WiFi; empty otherwise. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString WhyNot;
};
