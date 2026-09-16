// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Http/FlockFileUploader.h"

#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

namespace
{
	/** How much of a failing server's error document is worth keeping. Matches the HTTP client's trace cap. */
	constexpr int32 MaxErrorDetailChars = 512;

	/**
	 * Streams a file straight out of disk into a PUT.
	 *
	 * `SetContentAsStreamedFile` is what keeps the payload off the heap: the engine reads the file as it sends.
	 *
	 * **Where this deliberately differs from the downloader:** when a platform refuses the stream, the downloader
	 * falls back to buffering, because a memory cost beats an unavailable feature. Here it **fails instead**. The
	 * files this sends are whole recordings, capped at more than a gigabyte, so buffering one would cost the
	 * player's own session a chance of running out of memory -- and the caller's fallback is good, because a
	 * recording that was not uploaded is kept and pushed by a later launch. A failed upload is recoverable; an
	 * out-of-memory crash during play is not.
	 */
	class FFlockHttpFileUploader final : public IFlockFileUploader
	{
	public:
		explicit FFlockHttpFileUploader(const TSharedRef<IFlockLogger>& InLogger)
			: Logger(InLogger)
		{
		}

		virtual FFlockRequestHandle UploadFile(const FString& Url, const FString& SourcePath, const FString& ContentType,
			float TimeoutSeconds, TFunction<void(int64, int64)> OnProgress,
			TFunction<void(FFlockFileUploadOutcome)> OnComplete) override
		{
			const TSharedPtr<FFlockCancelToken> Token = MakeShared<FFlockCancelToken>();

			// Read the size before sending: it is what progress is measured against, and a file that is not there
			// has to fail here rather than as an empty PUT storage would accept.
			const int64 FileBytes = IFileManager::Get().FileSize(*SourcePath);
			if (FileBytes < 0)
			{
				CompleteWithFailure(OnComplete, FString::Printf(TEXT("There is no file to upload at '%s'"), *SourcePath));
				return FFlockRequestHandle(Token);
			}

			const FHttpRequestRef HttpRequest = FHttpModule::Get().CreateRequest();
			HttpRequest->SetURL(Url);
			HttpRequest->SetVerb(TEXT("PUT"));
			// The link is signed for one content type, and storage refuses a PUT that carries another.
			HttpRequest->SetHeader(TEXT("Content-Type"), ContentType);
			if (TimeoutSeconds > 0.f)
			{
				HttpRequest->SetTimeout(TimeoutSeconds);
			}

			if (!HttpRequest->SetContentAsStreamedFile(SourcePath))
			{
				Logger->LogDebug(TEXT("Upload: this platform will not stream a file, and the file is too big to hold in memory"));
				CompleteWithFailure(OnComplete, FString::Printf(
					TEXT("This platform cannot send '%s' without holding it in memory, so it was not uploaded"), *SourcePath));
				return FFlockRequestHandle(Token);
			}

			if (OnProgress)
			{
				HttpRequest->OnRequestProgress64().BindLambda(
					[OnProgress, FileBytes](FHttpRequestPtr /*Request*/, uint64 BytesSent, uint64 /*BytesReceived*/)
					{
						OnProgress(static_cast<int64>(BytesSent), FileBytes);
					});
			}

			const TWeakPtr<IHttpRequest, ESPMode::ThreadSafe> WeakRequest = HttpRequest;
			Token->Canceller = [WeakRequest]()
			{
				if (const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Pinned = WeakRequest.Pin())
				{
					Pinned->CancelRequest();
				}
			};

			HttpRequest->OnProcessRequestComplete().BindLambda(
				[OnComplete, Token, FileBytes]
				(FHttpRequestPtr RequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
				{
					FFlockFileUploadOutcome Outcome;
					Outcome.Result = Classify(Token->bCancelled, RequestPtr, Response, bConnectedSuccessfully);

					if (Outcome.Result == EFlockHttpResult::Success)
					{
						Outcome.StatusCode = Response->GetResponseCode();
						if (Outcome.IsUploaded())
						{
							Outcome.BytesSent = FileBytes;
						}
						else
						{
							// Storage explains a refusal in the body -- an expired signature or a content type
							// that does not match the one the link was signed for.
							Outcome.ErrorDetail = Response->GetContentAsString().Left(MaxErrorDetailChars);
						}
					}
					else if (RequestPtr.IsValid())
					{
						Outcome.ErrorDetail = LexToString(RequestPtr->GetFailureReason());
					}

					if (OnComplete)
					{
						OnComplete(Outcome);
					}
				});

			if (!HttpRequest->ProcessRequest())
			{
				// Never dispatched, so the completion above will not run.
				CompleteWithFailure(OnComplete, TEXT("Failed to start the upload"));
			}
			return FFlockRequestHandle(Token);
		}

	private:
		static void CompleteWithFailure(const TFunction<void(FFlockFileUploadOutcome)>& OnComplete, const FString& Detail)
		{
			FFlockFileUploadOutcome Outcome;
			Outcome.Result = EFlockHttpResult::ConnectionError;
			Outcome.ErrorDetail = Detail;
			if (OnComplete)
			{
				OnComplete(Outcome);
			}
		}

		static EFlockHttpResult Classify(bool bCancelled, const FHttpRequestPtr& RequestPtr,
			const FHttpResponsePtr& Response, bool bConnectedSuccessfully)
		{
			if (bCancelled)
			{
				return EFlockHttpResult::Cancelled;
			}
			if (bConnectedSuccessfully && Response.IsValid())
			{
				return EFlockHttpResult::Success;
			}

			const EHttpFailureReason Reason = RequestPtr.IsValid() ? RequestPtr->GetFailureReason() : EHttpFailureReason::Other;
			switch (Reason)
			{
			case EHttpFailureReason::TimedOut:
				return EFlockHttpResult::Timeout;
			case EHttpFailureReason::Cancelled:
				return EFlockHttpResult::Cancelled;
			default:
				return EFlockHttpResult::ConnectionError;
			}
		}

		TSharedRef<IFlockLogger> Logger;
	};
}

TSharedRef<IFlockFileUploader> FlockCreateHttpFileUploader(const TSharedRef<IFlockLogger>& Logger)
{
	return MakeShared<FFlockHttpFileUploader>(Logger);
}
