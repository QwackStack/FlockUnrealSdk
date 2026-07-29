// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockLogger.h"
#include "Http/FlockHttpAdapter.h"

/**
 * Outcome of one binary transfer. Mirrors FFlockHttpResponse's transport classification, minus the body:
 * an asset download has no body to hand back, because the bytes went to disk as they arrived.
 */
struct FFlockAssetDownloadOutcome
{
	EFlockHttpResult Result = EFlockHttpResult::ConnectionError;

	/** HTTP status when Result is Success; 0 otherwise. */
	int32 StatusCode = 0;

	/** Transport error detail, or the first of the server's error document on a non-2xx. */
	FString ErrorDetail;

	/** Bytes committed to the target file. */
	int64 BytesWritten = 0;
};

/**
 * Transport seam for binary asset downloads — a second seam alongside IFlockHttpAdapter rather than an
 * extension of it.
 *
 * They carry genuinely different traffic. The JSON adapter speaks to the Flock API: bearer headers, an
 * envelope to unwrap, a response that fits in an FString, and a retry budget entangled with token
 * refresh. This one speaks to presigned object storage: no headers of ours, no envelope, a payload that
 * must never be held in memory whole, and progress reporting the JSON path has no concept of. Folding
 * the two would put a stream target and a progress delegate on every `player/login` call.
 *
 * Implementations must invoke OnComplete exactly once, on the game thread.
 */
class FLOCK_API IFlockAssetDownloader
{
public:
	virtual ~IFlockAssetDownloader() = default;

	/**
	 * Streams Url into TargetPath, creating or truncating it. OnProgress reports
	 * (BytesReceived, TotalBytes) where TotalBytes is 0 until the server declares a Content-Length —
	 * callers must tolerate a total they cannot yet divide by.
	 *
	 * On any non-success outcome the implementation leaves no partial file behind: a truncated download
	 * that survived would later read as a valid cache hit.
	 */
	virtual FFlockRequestHandle DownloadToFile(const FString& Url, const FString& TargetPath, float TimeoutSeconds,
		TFunction<void(int64, int64)> OnProgress,
		TFunction<void(FFlockAssetDownloadOutcome)> OnComplete) = 0;
};

/**
 * The production downloader, over the engine HTTP module. Kept behind this factory for the same reason
 * FFlockHttpClient::CreateDefault is: the HTTP module is a private dependency, so no public header may
 * name an engine HTTP type.
 */
FLOCK_API TSharedRef<IFlockAssetDownloader> FlockCreateHttpAssetDownloader(const TSharedRef<IFlockLogger>& Logger);
