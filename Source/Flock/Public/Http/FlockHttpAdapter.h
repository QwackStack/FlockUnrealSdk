// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/** Transport outcome of a request the adapter attempted. */
enum class EFlockHttpResult : uint8
{
	/** The HTTP exchange completed; StatusCode is set (any status, including 4xx/5xx). */
	Success,
	/** The request timed out before a response arrived. */
	Timeout,
	/** A transport-level failure (DNS, refused, offline) — no HTTP response. */
	ConnectionError,
	/** The request was cancelled via its FFlockRequestHandle before completing. */
	Cancelled,
};

/** Normalized request handed to an IFlockHttpAdapter. Plain struct — no engine-HTTP types leak here. */
struct FFlockHttpRequest
{
	FString Method;
	FString Url;
	TMap<FString, FString> Headers;

	/** Serialized JSON body. Only sent when bHasBody is true (distinguishes an empty body from none). */
	FString JsonBody;
	bool bHasBody = false;

	/** Per-request timeout in seconds; <= 0 uses the adapter's default. */
	float TimeoutSeconds = 0.f;
};

/** Normalized response returned by an IFlockHttpAdapter. */
struct FFlockHttpResponse
{
	EFlockHttpResult Result = EFlockHttpResult::ConnectionError;

	/** HTTP status when Result is Success; 0 otherwise. */
	int32 StatusCode = 0;

	/** Response text on Success; transport error detail on failure. */
	FString Body;

	/** Raw Retry-After header value, or empty. Parsed by the client. */
	FString RetryAfterHeader;
};

/** Shared cancellation state behind an FFlockRequestHandle. */
struct FFlockCancelToken
{
	/** Set once cancelled; callers check this to suppress a late completion. */
	bool bCancelled = false;

	/** Performs the actual cancel for the current owner of in-flight work (e.g. IHttpRequest::CancelRequest). */
	TFunction<void()> Canceller;
};

/**
 * Cancels an in-flight request (and, at the retry layer, any pending retry). Idempotent and safe to
 * call after completion. Completions and cancellation both
 * run on the game thread, so no external locking is required.
 */
class FLOCK_API FFlockRequestHandle
{
public:
	FFlockRequestHandle() = default;
	explicit FFlockRequestHandle(const TSharedPtr<FFlockCancelToken>& InToken)
		: Token(InToken)
	{
	}

	void Cancel()
	{
		if (Token.IsValid() && !Token->bCancelled)
		{
			Token->bCancelled = true;
			if (Token->Canceller)
			{
				Token->Canceller();
			}
		}
	}

	/** True when this handle refers to a cancellable operation. */
	bool IsValid() const { return Token.IsValid(); }

private:
	TSharedPtr<FFlockCancelToken> Token;
};

/**
 * Transport seam for SDK HTTP. The default is FFlockHttpModuleAdapter (engine HTTP module); tests inject
 * FFlockFakeTransport. One adapter covers all targets — UE's HTTP module already abstracts the platform
 * backend, so there is no per-platform split.
 */
class FLOCK_API IFlockHttpAdapter
{
public:
	virtual ~IFlockHttpAdapter() = default;

	/**
	 * Sends one request and invokes OnComplete on the game thread with a normalized response. Transport
	 * failures come back as a non-Success result, never as an exception. Returns a handle that cancels
	 * the in-flight request.
	 */
	virtual FFlockRequestHandle SendAsync(const FFlockHttpRequest& Request, TFunction<void(FFlockHttpResponse)> OnComplete) = 0;
};
