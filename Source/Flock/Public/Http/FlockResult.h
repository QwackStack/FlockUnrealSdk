// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"

/**
 * Result of an SDK HTTP operation: either a value of type T or an FFlockError. Failure is a value, not
 * a thrown exception (UE disables C++ exceptions).
 *
 * A plain C++ template (not a USTRUCT — USTRUCTs can't be generic), returned by value to a
 * TFunction completion callback. T is typically a USTRUCT model, but may be any default-constructible
 * type (e.g. FString, or a container of models).
 */
template <typename T>
struct TFlockResult
{
	/** True when the operation succeeded and Value is populated. */
	bool bSuccess = false;

	/** The deserialized payload; valid only when bSuccess. */
	T Value = T();

	/** The failure detail; valid only when !bSuccess. */
	FFlockError Error;

	static TFlockResult<T> Ok(const T& InValue)
	{
		TFlockResult<T> Result;
		Result.bSuccess = true;
		Result.Value = InValue;
		return Result;
	}

	static TFlockResult<T> Fail(const FFlockError& InError)
	{
		TFlockResult<T> Result;
		Result.bSuccess = false;
		Result.Error = InError;
		return Result;
	}

	bool IsSuccess() const { return bSuccess; }
};

/**
 * One page of a paginated list response ({items, total, page, limit}). A C++ template return like
 * TFlockResult<T>, for the same generics reason.
 */
template <typename T>
struct TFlockPage
{
	/** Items on this page. */
	TArray<T> Items;

	/** Total items across all pages. */
	int32 Total = 0;

	/** 1-based page index this response represents. */
	int32 Page = 0;

	/** Page size. */
	int32 Limit = 0;
};
