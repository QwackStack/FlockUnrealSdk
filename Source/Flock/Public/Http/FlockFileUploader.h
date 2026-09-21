// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockLogger.h"
#include "Http/FlockHttpAdapter.h"

/**
 * Outcome of one file upload. Mirrors FFlockAssetDownloadOutcome, the other direction: there is no body to
 * hand back, because the bytes came off disk as they were sent.
 */
struct FFlockFileUploadOutcome
{
	EFlockHttpResult Result = EFlockHttpResult::ConnectionError;

	/** HTTP status when Result is Success; 0 otherwise. A 2xx here is the only thing that means "uploaded". */
	int32 StatusCode = 0;

	/** Transport error detail, or the first of the server's error document on a non-2xx. */
	FString ErrorDetail;

	/** Bytes the file held when it was sent. */
	int64 BytesSent = 0;

	/** A 2xx from the storage service, which is the only proof the bytes arrived. */
	bool IsUploaded() const { return Result == EFlockHttpResult::Success && StatusCode >= 200 && StatusCode < 300; }
};

/**
 * Transport seam for sending a file to storage, the counterpart of IFlockAssetDownloader and a separate seam
 * from IFlockHttpAdapter for the same reasons that one is.
 *
 * The JSON adapter speaks to the Flock API: bearer headers, an envelope to unwrap, a body that fits in an
 * FString. This one speaks to a presigned URL: no headers of ours beyond the content type the link was signed
 * for, no envelope, and a payload that must never be held in memory whole.
 *
 * Implementations must invoke OnComplete exactly once, on the game thread.
 */
class FLOCK_API IFlockFileUploader
{
public:
	virtual ~IFlockFileUploader() = default;

	/**
	 * Sends SourcePath to Url with PUT, streaming it off disk. ContentType must be the one the link was signed
	 * for, or storage refuses the upload. OnProgress reports (BytesSent, TotalBytes).
	 *
	 * A missing or unreadable file completes with a failure rather than sending an empty body: storage would
	 * accept the empty PUT, and the recording it stood for would be lost with the upload reported as done.
	 */
	virtual FFlockRequestHandle UploadFile(const FString& Url, const FString& SourcePath, const FString& ContentType,
		float TimeoutSeconds, TFunction<void(int64, int64)> OnProgress,
		TFunction<void(FFlockFileUploadOutcome)> OnComplete) = 0;
};

/**
 * The production uploader, over the engine HTTP module. Behind a factory for the same reason
 * FlockCreateHttpAssetDownloader is: the HTTP module is a private dependency, so no public header may name an
 * engine HTTP type.
 */
FLOCK_API TSharedRef<IFlockFileUploader> FlockCreateHttpFileUploader(const TSharedRef<IFlockLogger>& Logger);
