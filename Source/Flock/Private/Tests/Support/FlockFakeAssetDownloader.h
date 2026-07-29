// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Assets/FlockAssetDownloader.h"
#include "Misc/FileHelper.h"

/**
 * Header-only IFlockAssetDownloader fake. Routes by URL fragment, writes the routed payload to the
 * target path exactly as the real one would, and records every URL it was asked for. Include only from
 * guarded test TUs.
 *
 * Deferred mode holds completions so several transfers can be in flight at once — the only way to
 * observe the concurrency cap with a fake that would otherwise complete synchronously. It is also what
 * makes PeakActive meaningful: in immediate mode every transfer finishes before the next begins, so the
 * peak would always read 1 no matter what the cap was set to.
 */
class FFlockFakeAssetDownloader : public IFlockAssetDownloader
{
public:
	/** Route a URL fragment to a successful transfer carrying Payload. Last route for a fragment wins. */
	FFlockFakeAssetDownloader& On(const FString& UrlFragment, const FString& Payload)
	{
		Routes.RemoveAll([&](const FRoute& R) { return R.Fragment == UrlFragment; });
		FRoute Route;
		Route.Fragment = UrlFragment;
		Route.Payload = Payload;
		Route.Outcome.Result = EFlockHttpResult::Success;
		Route.Outcome.StatusCode = 200;
		Routes.Add(Route);
		return *this;
	}

	/** Route a fragment to an HTTP status — 403 for an expired signature, 500 for a server fault. */
	FFlockFakeAssetDownloader& OnStatus(const FString& UrlFragment, int32 StatusCode, const FString& Detail = FString())
	{
		Routes.RemoveAll([&](const FRoute& R) { return R.Fragment == UrlFragment; });
		FRoute Route;
		Route.Fragment = UrlFragment;
		Route.Outcome.Result = EFlockHttpResult::Success;
		Route.Outcome.StatusCode = StatusCode;
		Route.Outcome.ErrorDetail = Detail;
		Routes.Add(Route);
		return *this;
	}

	/** Route a fragment to a transport failure with no HTTP response at all. */
	FFlockFakeAssetDownloader& OnTransportFailure(const FString& UrlFragment, EFlockHttpResult Result)
	{
		Routes.RemoveAll([&](const FRoute& R) { return R.Fragment == UrlFragment; });
		FRoute Route;
		Route.Fragment = UrlFragment;
		Route.Outcome.Result = Result;
		Routes.Add(Route);
		return *this;
	}

	virtual FFlockRequestHandle DownloadToFile(const FString& Url, const FString& TargetPath, float /*TimeoutSeconds*/,
		TFunction<void(int64, int64)> OnProgress,
		TFunction<void(FFlockAssetDownloadOutcome)> OnComplete) override
	{
		Requests.Add(Url);
		++ActiveCount;
		PeakActive = FMath::Max(PeakActive, ActiveCount);

		const FRoute Route = Resolve(Url);
		const bool bOk = Route.Outcome.Result == EFlockHttpResult::Success
			&& Route.Outcome.StatusCode >= 200 && Route.Outcome.StatusCode < 300;

		TFunction<void()> Deliver = [this, Route, TargetPath, bOk, OnProgress, OnComplete]()
		{
			FFlockAssetDownloadOutcome Outcome = Route.Outcome;
			if (bOk)
			{
				// Same contract as the real downloader: the bytes are on disk before the completion fires.
				FFileHelper::SaveStringToFile(Route.Payload, *TargetPath);
				Outcome.BytesWritten = Route.Payload.Len();
				if (OnProgress)
				{
					OnProgress(Outcome.BytesWritten, Outcome.BytesWritten);
				}
			}
			else
			{
				// And it never leaves a partial file behind.
				IFileManager::Get().Delete(*TargetPath, false, true, true);
			}
			--ActiveCount;
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

	/** When true, DownloadToFile queues its completion instead of firing it. */
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

	/** Drains until nothing new is queued — the whole cap-limited pipeline. */
	void FlushAll()
	{
		int32 Guard = 0;
		while (Pending.Num() > 0 && Guard++ < 100)
		{
			FlushPending();
		}
	}

	int32 CountRequestsContaining(const FString& Fragment) const
	{
		int32 Count = 0;
		for (const FString& Url : Requests)
		{
			if (Url.Contains(Fragment))
			{
				++Count;
			}
		}
		return Count;
	}

	TArray<FString> Requests;

	/** Highest number of transfers in flight simultaneously — what the concurrency cap is asserted against. */
	int32 PeakActive = 0;

private:
	struct FRoute
	{
		FString Fragment;
		FString Payload;
		FFlockAssetDownloadOutcome Outcome;
	};

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
	int32 ActiveCount = 0;
};
