// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockErrorCode.h"
#include "FlockError.generated.h"

/**
 * Failure classification for an SDK HTTP operation. A value type rather than a thrown exception, since
 * UE disables C++ exceptions. Timeout/Connection are split out of Network so consumers and the retry
 * handler can distinguish them.
 */
UENUM(BlueprintType)
enum class EFlockErrorType : uint8
{
	/** No error — the default on a value-initialized FFlockError inside a successful result. */
	None,
	/** Server returned a non-success status (or an unclassified transport issue). */
	Network,
	/** 401/403 — authentication/authorization failed. */
	Auth,
	/** 400/422 — request validation failed. */
	Validation,
	/** Response could not be turned into the expected type (empty/malformed body). Permanent. */
	Serialization,
	/** The request timed out before a response arrived. */
	Timeout,
	/** A transport-level failure (DNS, refused, offline) — no HTTP response. */
	Connection,
	/** The caller cancelled the request via its FFlockRequestHandle. Never retried. */
	Cancelled,
};

/**
 * A failed SDK HTTP operation, carried by TFlockResult<T> on failure. Carries the response body, HTTP
 * status, and the server's error code (raw and typed) as a BlueprintType struct so future Blueprint
 * async nodes can surface typed errors without reshaping this layer.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockError
{
	GENERATED_BODY()

	/** Failure classification. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	EFlockErrorType Type = EFlockErrorType::None;

	/** Terse, error-tracker-friendly message (kept free of the payload for stable bucketing). */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Message;

	/** HTTP status when the error came from a server response; 0 for client-side/transport failures. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 StatusCode = 0;

	/** Raw server response body, kept off Message so error trackers bucket by type not payload. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Body;

	/** Server's machine-readable error code from the coded-error body; empty when the body had none. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Code;

	/** Typed form of Code; Unknown when there was no code or this SDK version doesn't recognize it. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	EFlockErrorCode ErrorCode = EFlockErrorCode::Unknown;

	/**
	 * Human-readable message from the server's coded-error body (`detail.message`); empty when the body
	 * had none. The server's wording of the failure — unlike Message, which stays terse for bucketing.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString ServerMessage;

	/** True when the server sent a Retry-After hint (429/503) parsed into RetryAfterSeconds. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool bHasRetryAfter = false;

	/** Server Retry-After hint in seconds; honored by the retry handler when bHasRetryAfter. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	float RetryAfterSeconds = 0.f;

	/** Builds an error and parses ErrorCode from the coded string. */
	static FFlockError Make(EFlockErrorType InType, const FString& InMessage, int32 InStatusCode = 0,
		const FString& InBody = FString(), const FString& InCode = FString(),
		const FString& InServerMessage = FString());

	/** Standard text plus the server body (so logs show the reason while Message stays terse). */
	FString ToString() const;

	/**
	 * True when a register/login route reports this identity (email/device/OAuth) already belongs to an
	 * account. Excludes a taken display name — that's a different fix.
	 */
	bool IsAlreadyRegistered() const;

	/** Permanent 4xx is an authoritative server answer; 408/429 are 4xx but transient by spec. */
	static bool IsPermanentStatus(int32 InStatusCode);

	/** 408/429 mean the server rejected the request before processing it (safe to retry non-idempotents). */
	static bool IsNotProcessed(int32 InStatusCode);
};
