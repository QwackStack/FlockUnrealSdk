// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Backoff parameters for FFlockRetryHandler. Runtime values come from UFlockConfig (RetryMaxRetries,
 * bRetryUseJitter, HttpTimeoutSeconds).
 */
struct FFlockRetryPolicy
{
	/** How many times to retry after the initial attempt fails. */
	int32 MaxRetries = 3;

	/** Wait before the first retry. */
	float InitialDelaySeconds = 1.f;

	/** Upper bound on any single delay, regardless of backoff growth or a Retry-After hint. */
	float MaxDelaySeconds = 30.f;

	/** Each retry multiplies the previous delay by this factor. */
	double BackoffMultiplier = 2.0;

	/** Adds ±25% randomness to each backoff delay to avoid thundering-herd reconnects. */
	bool bUseJitter = true;
};
