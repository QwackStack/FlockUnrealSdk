// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Http/FlockFileUploader.h"

/**
 * Header-only IFlockFileUploader fake, the counterpart of Flock core's FFlockFakeAssetDownloader. Routes by URL fragment
 * and records every upload it was asked for, with the file's size read at the moment of the call -- so a test
 * can tell a finished recording from one still being written, which no record of the path alone would show.
 * Include only from guarded test TUs.
 *
 * `SignedFor` models the one storage rule a record of the request cannot: a presigned link is signed for one
 * content type, and a PUT carrying another is refused. Without it a test asserting "the right content type is
 * sent" passes just as well against a caller that sends nothing at all.
 *
 * Deferred mode holds completions so an upload can still be in flight while the thing that started it goes
 * away -- which is the crossing that matters here, because a recording's run is held only for the length of
 * its upload.
 */
class FProtokitePlaytestFakeFileUploader : public IFlockFileUploader
{
public:
	/** Route a URL fragment to a successful upload. Last route for a fragment wins. */
	FProtokitePlaytestFakeFileUploader& On(const FString& UrlFragment)
	{
		FRoute& Route = Replace(UrlFragment);
		Route.Outcome.Result = EFlockHttpResult::Success;
		Route.Outcome.StatusCode = 200;
		return *this;
	}

	/** Route a fragment to an HTTP status -- 403 for an expired signature, 500 for a server fault. */
	FProtokitePlaytestFakeFileUploader& OnStatus(const FString& UrlFragment, int32 StatusCode, const FString& Detail = FString())
	{
		FRoute& Route = Replace(UrlFragment);
		Route.Outcome.Result = EFlockHttpResult::Success;
		Route.Outcome.StatusCode = StatusCode;
		Route.Outcome.ErrorDetail = Detail;
		return *this;
	}

	/** Route a fragment to a transport failure with no HTTP response at all. */
	FProtokitePlaytestFakeFileUploader& OnTransportFailure(const FString& UrlFragment, EFlockHttpResult Result)
	{
		FRoute& Route = Replace(UrlFragment);
		Route.Outcome.Result = Result;
		return *this;
	}

	/**
	 * Makes a route behave like a real presigned link: a PUT whose content type is not this one is refused with
	 * 403, the way storage refuses it, instead of quietly succeeding.
	 */
	FProtokitePlaytestFakeFileUploader& SignedFor(const FString& UrlFragment, const FString& ContentType)
	{
		Replace(UrlFragment, /*bKeepOutcome*/ true).SignedContentType = ContentType;
		return *this;
	}

	virtual FFlockRequestHandle UploadFile(const FString& Url, const FString& SourcePath, const FString& ContentType,
		float /*TimeoutSeconds*/, TFunction<void(int64, int64)> OnProgress,
		TFunction<void(FFlockFileUploadOutcome)> OnComplete) override
	{
		FUpload Record;
		Record.Url = Url;
		Record.SourcePath = SourcePath;
		Record.ContentType = ContentType;
		// Read now, not at delivery: what matters is the file as it was when the upload started.
		Record.FileBytes = IFileManager::Get().FileSize(*SourcePath);
		Uploads.Add(Record);

		const FRoute Route = Resolve(Url);

		TFunction<void()> Deliver = [Route, Record, OnProgress, OnComplete]()
		{
			FFlockFileUploadOutcome Outcome = Route.Outcome;

			if (Record.FileBytes < 0)
			{
				// The real uploader refuses rather than sending an empty body storage would accept.
				Outcome.Result = EFlockHttpResult::ConnectionError;
				Outcome.StatusCode = 0;
				Outcome.ErrorDetail = TEXT("There is no file to upload");
			}
			else if (!Route.SignedContentType.IsEmpty() && Route.SignedContentType != Record.ContentType)
			{
				Outcome.Result = EFlockHttpResult::Success;
				Outcome.StatusCode = 403;
				Outcome.ErrorDetail = TEXT("SignatureDoesNotMatch");
			}

			if (Outcome.IsUploaded())
			{
				Outcome.BytesSent = Record.FileBytes;
				if (OnProgress)
				{
					OnProgress(Record.FileBytes, Record.FileBytes);
				}
			}
			if (OnComplete)
			{
				OnComplete(Outcome);
			}
		};

		if (bDeferred)
		{
			Pending.Add(MoveTemp(Deliver));
		}
		else
		{
			Deliver();
		}
		return FFlockRequestHandle();
	}

	/** When true, UploadFile queues its completion instead of firing it. */
	bool bDeferred = false;

	/** Delivers one wave of queued completions. Work they start lands in the next wave. */
	void FlushPending()
	{
		TArray<TFunction<void()>> ToRun = MoveTemp(Pending);
		Pending.Reset();
		for (TFunction<void()>& Run : ToRun)
		{
			Run();
		}
	}

	/** Drains until nothing new is queued. */
	void FlushAll()
	{
		int32 Guard = 0;
		while (Pending.Num() > 0 && Guard++ < 100)
		{
			FlushPending();
		}
	}

	struct FUpload
	{
		FString Url;
		FString SourcePath;
		FString ContentType;
		/** The file's size when the upload started; -1 when there was no file there. */
		int64 FileBytes = 0;
	};

	int32 CountUploadsContaining(const FString& Fragment) const
	{
		int32 Count = 0;
		for (const FUpload& Upload : Uploads)
		{
			if (Upload.Url.Contains(Fragment))
			{
				++Count;
			}
		}
		return Count;
	}

	TArray<FUpload> Uploads;

private:
	struct FRoute
	{
		FString Fragment;
		FString SignedContentType;
		FFlockFileUploadOutcome Outcome;
	};

	/** Replaces a fragment's route, optionally keeping what was already set on it. */
	FRoute& Replace(const FString& UrlFragment, bool bKeepOutcome = false)
	{
		for (int32 Index = 0; Index < Routes.Num(); ++Index)
		{
			if (Routes[Index].Fragment == UrlFragment)
			{
				if (bKeepOutcome)
				{
					return Routes[Index];
				}
				const FString KeptContentType = Routes[Index].SignedContentType;
				Routes.RemoveAt(Index);
				FRoute& Added = Routes.AddDefaulted_GetRef();
				Added.Fragment = UrlFragment;
				Added.SignedContentType = KeptContentType;
				return Added;
			}
		}
		FRoute& Added = Routes.AddDefaulted_GetRef();
		Added.Fragment = UrlFragment;
		Added.Outcome.Result = EFlockHttpResult::Success;
		Added.Outcome.StatusCode = 200;
		return Added;
	}

	FRoute Resolve(const FString& Url) const
	{
		for (const FRoute& Route : Routes)
		{
			if (Url.Contains(Route.Fragment))
			{
				return Route;
			}
		}
		FRoute Fallback;
		Fallback.Outcome.Result = EFlockHttpResult::Success;
		Fallback.Outcome.StatusCode = 404;
		Fallback.Outcome.ErrorDetail = TEXT("no route");
		return Fallback;
	}

	TArray<FRoute> Routes;
	TArray<TFunction<void()>> Pending;
};
