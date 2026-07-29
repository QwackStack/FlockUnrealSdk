// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Assets/FlockAssetDownloader.h"

#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"

namespace
{
	/** How much of a failing server's error document is worth keeping. Matches the HTTP client's trace cap. */
	constexpr int32 MaxErrorDetailChars = 512;

	/**
	 * Streams a download straight into a file archive.
	 *
	 * The one thing this buys over the JSON adapter is that the payload never exists in memory as a whole:
	 * IHttpRequest::SetResponseBodyReceiveStream hands the engine the FArchive to write into as bytes
	 * arrive. Canonical buffers the entire asset into a managed byte[] and then writes it out, so a 300 MB
	 * asset costs 300 MB of heap before it costs any disk. Here it costs the disk write and a socket buffer.
	 *
	 * When a platform's HTTP implementation refuses the stream, we fall back to the buffered path rather
	 * than failing — a memory cost is better than an unavailable feature.
	 */
	class FFlockHttpAssetDownloader final : public IFlockAssetDownloader
	{
	public:
		explicit FFlockHttpAssetDownloader(const TSharedRef<IFlockLogger>& InLogger)
			: Logger(InLogger)
		{
		}

		virtual FFlockRequestHandle DownloadToFile(const FString& Url, const FString& TargetPath, float TimeoutSeconds,
			TFunction<void(int64, int64)> OnProgress,
			TFunction<void(FFlockAssetDownloadOutcome)> OnComplete) override
		{
			const TSharedPtr<FFlockCancelToken> Token = MakeShared<FFlockCancelToken>();
			const TSharedRef<IFlockLogger> Log = Logger;

			// Held shared so the completion can Close() it before the file is read back or moved. The engine
			// keeps its own reference for the duration of the transfer.
			TSharedPtr<FArchive> Writer = MakeShareable(IFileManager::Get().CreateFileWriter(*TargetPath));
			if (!Writer.IsValid())
			{
				FFlockAssetDownloadOutcome Outcome;
				Outcome.Result = EFlockHttpResult::ConnectionError;
				Outcome.ErrorDetail = FString::Printf(TEXT("Couldn't open '%s' for writing"), *TargetPath);
				if (OnComplete)
				{
					OnComplete(Outcome);
				}
				return FFlockRequestHandle(Token);
			}

			const FHttpRequestRef HttpRequest = FHttpModule::Get().CreateRequest();
			HttpRequest->SetURL(Url);
			HttpRequest->SetVerb(TEXT("GET"));
			if (TimeoutSeconds > 0.f)
			{
				HttpRequest->SetTimeout(TimeoutSeconds);
			}

			const bool bStreaming = HttpRequest->SetResponseBodyReceiveStream(Writer.ToSharedRef());
			if (!bStreaming)
			{
				// Nothing will be written through the archive; close it now so the buffered path can
				// replace the file wholesale without fighting an open handle.
				Writer->Close();
				Writer.Reset();
				Log->LogDebug(TEXT("Asset download: response streaming unavailable, buffering in memory"));
			}

			if (OnProgress)
			{
				HttpRequest->OnRequestProgress64().BindLambda(
					[OnProgress](FHttpRequestPtr Request, uint64 /*BytesSent*/, uint64 BytesReceived)
					{
						// Content-Length lands with the headers, so the first ticks legitimately report a
						// total of 0. Callers show indeterminate progress until it arrives.
						int64 Total = 0;
						if (Request.IsValid())
						{
							if (const FHttpResponsePtr Response = Request->GetResponse())
							{
								Total = static_cast<int64>(Response->GetContentLength());
							}
						}
						OnProgress(static_cast<int64>(BytesReceived), Total);
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
				[OnComplete, Token, Writer, TargetPath, bStreaming, Log]
				(FHttpRequestPtr RequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
				{
					// Flush and release the handle before anything reads, moves, or deletes the file.
					if (Writer.IsValid())
					{
						Writer->Close();
					}

					FFlockAssetDownloadOutcome Outcome;
					Outcome.Result = Classify(Token->bCancelled, RequestPtr, Response, bConnectedSuccessfully);

					if (Outcome.Result == EFlockHttpResult::Success)
					{
						Outcome.StatusCode = Response->GetResponseCode();
						const bool bOk = Outcome.StatusCode >= 200 && Outcome.StatusCode < 300;

						if (bOk && !bStreaming)
						{
							// Buffered fallback: the payload is in the response, so write it out here.
							const TArray<uint8>& Content = Response->GetContent();
							if (!FFileHelper::SaveArrayToFile(Content, *TargetPath))
							{
								Outcome.Result = EFlockHttpResult::ConnectionError;
								Outcome.ErrorDetail = FString::Printf(TEXT("Couldn't write '%s'"), *TargetPath);
							}
						}

						if (!bOk)
						{
							// The error document streamed into the target file, so read the detail back out
							// of it rather than from a response body that streaming left empty.
							Outcome.ErrorDetail = bStreaming ? ReadErrorDetail(TargetPath) : Response->GetContentAsString();
							Outcome.ErrorDetail = Outcome.ErrorDetail.Left(MaxErrorDetailChars);
						}
					}
					else if (RequestPtr.IsValid())
					{
						Outcome.ErrorDetail = LexToString(RequestPtr->GetFailureReason());
					}

					const bool bKeepFile = Outcome.Result == EFlockHttpResult::Success
						&& Outcome.StatusCode >= 200 && Outcome.StatusCode < 300;
					if (bKeepFile)
					{
						Outcome.BytesWritten = FMath::Max<int64>(IFileManager::Get().FileSize(*TargetPath), 0);
					}
					else
					{
						// A partial or error-document file left on disk would later read as a cache hit.
						IFileManager::Get().Delete(*TargetPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
					}

					if (OnComplete)
					{
						OnComplete(Outcome);
					}
				});

			if (!HttpRequest->ProcessRequest())
			{
				// Never dispatched, so the completion above will not run — close and clean up here.
				if (Writer.IsValid())
				{
					Writer->Close();
				}
				IFileManager::Get().Delete(*TargetPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);

				FFlockAssetDownloadOutcome Outcome;
				Outcome.Result = EFlockHttpResult::ConnectionError;
				Outcome.ErrorDetail = TEXT("Failed to start asset download");
				if (OnComplete)
				{
					OnComplete(Outcome);
				}
			}
			return FFlockRequestHandle(Token);
		}

	private:
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

		/** Pulls the leading bytes of a streamed error document back off disk. */
		static FString ReadErrorDetail(const FString& Path)
		{
			FString Contents;
			FFileHelper::LoadFileToString(Contents, *Path);
			return Contents.Left(MaxErrorDetailChars);
		}

		TSharedRef<IFlockLogger> Logger;
	};
}

TSharedRef<IFlockAssetDownloader> FlockCreateHttpAssetDownloader(const TSharedRef<IFlockLogger>& Logger)
{
	return MakeShared<FFlockHttpAssetDownloader>(Logger);
}
