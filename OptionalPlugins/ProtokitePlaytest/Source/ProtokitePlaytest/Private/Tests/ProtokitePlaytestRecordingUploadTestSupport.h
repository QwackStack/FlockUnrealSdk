// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

/**
 * What the recording-upload tests share. One copy, because a unity build compiles the module's files together, and the
 * same names in two files' anonymous namespaces are then a redefinition.
 */
namespace ProtokitePlaytestRecordingUploadTesting
{
	inline const FString UploadRoute = TEXT("/recording-upload");
	inline const FString UploadSessionId = TEXT("01M2N94A0CM8JMH48XV1Z735YK");
	inline const FString SignedUploadUrl = TEXT("http://storage.local/put/recording.webm?signature=abc");

	/** The shape the route really answers: the link sits under `result`, not at the root. */
	inline FString EnvelopedLinkBody(const FString& UploadUrl = SignedUploadUrl)
	{
		return FString::Printf(
			TEXT("{\"error\":null,\"response\":null,\"result\":{\"upload_url\":\"%s\",\"bucket\":\"recordings\",\"key\":\"a/b.webm\"}}"),
			*UploadUrl);
	}
}

#endif
