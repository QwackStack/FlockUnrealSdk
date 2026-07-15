// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockResult.h"
#include "Http/FlockError.h"
#include "Http/FlockJsonUtils.h"
#include "FlockLogger.h"

/**
 * Instance HTTP client: verbs + JSON (de)serialization + envelope unwrap + status→error mapping. One
 * attempt per call — retry is a separate concern (FFlockRetryHandler / FFlockProviderBase).
 *
 * This is an instance owned by whoever needs it, rather than a mutable process-global that would go
 * stale across PIE sessions: the subsystem owns one for runtime; the editor
 * version lookup builds a throwaway one. Must be heap-held via a shared pointer (Get<T> keeps the client
 * alive for the duration of the in-flight request via AsShared()).
 *
 * Enveloped verbs (Get/Post/...) unwrap {error, response, result} and return the bare model; *Raw verbs
 * deserialize the whole body for the endpoints that aren't enveloped.
 */
class FLOCK_API FFlockHttpClient : public TSharedFromThis<FFlockHttpClient>
{
public:
	FFlockHttpClient(const TSharedRef<IFlockHttpAdapter>& InAdapter, const TSharedRef<IFlockLogger>& InLogger,
		float InTimeoutSeconds = 30.f)
		: Adapter(InAdapter)
		, Logger(InLogger)
		, TimeoutSeconds(InTimeoutSeconds)
	{
	}

	/** Builds a client over the default engine-HTTP adapter, hiding the HTTP module from callers. */
	static TSharedRef<FFlockHttpClient> CreateDefault(float TimeoutSeconds, const TSharedRef<IFlockLogger>& Logger);

	// ── Enveloped verbs (unwrap `result` into T) ──

	template <typename T>
	FFlockRequestHandle Get(const FString& Url, const TMap<FString, FString>& Headers, TFunction<void(TFlockResult<T>)> OnComplete)
	{
		return Send<T>(TEXT("GET"), Url, Headers, FString(), /*bHasBody*/ false, /*bUnwrap*/ true, MoveTemp(OnComplete));
	}

	template <typename TResp, typename TBody>
	FFlockRequestHandle Post(const FString& Url, const TMap<FString, FString>& Headers, const TBody& Body, TFunction<void(TFlockResult<TResp>)> OnComplete)
	{
		return SendWithBody<TResp, TBody>(TEXT("POST"), Url, Headers, Body, /*bUnwrap*/ true, MoveTemp(OnComplete));
	}

	template <typename TResp, typename TBody>
	FFlockRequestHandle Put(const FString& Url, const TMap<FString, FString>& Headers, const TBody& Body, TFunction<void(TFlockResult<TResp>)> OnComplete)
	{
		return SendWithBody<TResp, TBody>(TEXT("PUT"), Url, Headers, Body, /*bUnwrap*/ true, MoveTemp(OnComplete));
	}

	template <typename TResp, typename TBody>
	FFlockRequestHandle Patch(const FString& Url, const TMap<FString, FString>& Headers, const TBody& Body, TFunction<void(TFlockResult<TResp>)> OnComplete)
	{
		return SendWithBody<TResp, TBody>(TEXT("PATCH"), Url, Headers, Body, /*bUnwrap*/ true, MoveTemp(OnComplete));
	}

	template <typename T>
	FFlockRequestHandle Delete(const FString& Url, const TMap<FString, FString>& Headers, TFunction<void(TFlockResult<T>)> OnComplete)
	{
		return Send<T>(TEXT("DELETE"), Url, Headers, FString(), /*bHasBody*/ false, /*bUnwrap*/ true, MoveTemp(OnComplete));
	}

	// ── Bare verbs (deserialize the whole body, for non-enveloped endpoints) ──

	template <typename T>
	FFlockRequestHandle GetRaw(const FString& Url, const TMap<FString, FString>& Headers, TFunction<void(TFlockResult<T>)> OnComplete)
	{
		return Send<T>(TEXT("GET"), Url, Headers, FString(), /*bHasBody*/ false, /*bUnwrap*/ false, MoveTemp(OnComplete));
	}

	// ── Paginated GET ({items, total, page, limit}) ──

	template <typename T>
	FFlockRequestHandle GetPaged(const FString& Url, const TMap<FString, FString>& Headers, TFunction<void(TFlockResult<TFlockPage<T>>)> OnComplete)
	{
		FFlockHttpRequest Request = MakeRequest(TEXT("GET"), Url, Headers, FString(), /*bHasBody*/ false);
		Logger->LogDebug(FString::Printf(TEXT("GET %s"), *Url));

		const TSharedRef<FFlockHttpClient> Self = AsShared();
		return Adapter->SendAsync(Request, [Self, OnComplete](FFlockHttpResponse Response)
		{
			if (!OnComplete)
			{
				return;
			}
			FFlockError Error;
			FString SuccessBody;
			if (!Self->ClassifyResponse(Response, Error, SuccessBody))
			{
				OnComplete(TFlockResult<TFlockPage<T>>::Fail(Error));
				return;
			}
			TFlockPage<T> Page;
			FString ParseError;
			if (!FFlockJsonUtils::UnwrapPaginated(SuccessBody, Page, ParseError))
			{
				OnComplete(TFlockResult<TFlockPage<T>>::Fail(FFlockError::Make(EFlockErrorType::Serialization, ParseError, Response.StatusCode, SuccessBody)));
				return;
			}
			OnComplete(TFlockResult<TFlockPage<T>>::Ok(Page));
		});
	}

private:
	FFlockHttpRequest MakeRequest(const FString& Method, const FString& Url, const TMap<FString, FString>& Headers,
		const FString& Body, bool bHasBody) const
	{
		FFlockHttpRequest Request;
		Request.Method = Method;
		Request.Url = Url;
		Request.Headers = Headers;
		Request.JsonBody = Body;
		Request.bHasBody = bHasBody;
		Request.TimeoutSeconds = TimeoutSeconds;
		return Request;
	}

	/** Transport + status mapping + coded-error/Retry-After parse. Returns true on 2xx (OutSuccessBody set). */
	bool ClassifyResponse(const FFlockHttpResponse& Response, FFlockError& OutError, FString& OutSuccessBody) const;

	template <typename T>
	TFlockResult<T> ParseInto(const FFlockHttpResponse& Response, bool bUnwrap) const
	{
		FFlockError Error;
		FString SuccessBody;
		if (!ClassifyResponse(Response, Error, SuccessBody))
		{
			return TFlockResult<T>::Fail(Error);
		}

		T Out;
		FString ParseError;
		const bool bOk = bUnwrap
			? FFlockJsonUtils::UnwrapResultToStruct(SuccessBody, Out, ParseError)
			: FFlockJsonUtils::WireJsonToStruct(SuccessBody, Out, ParseError);
		if (!bOk)
		{
			return TFlockResult<T>::Fail(FFlockError::Make(EFlockErrorType::Serialization, ParseError, Response.StatusCode, SuccessBody));
		}
		return TFlockResult<T>::Ok(Out);
	}

	template <typename T>
	FFlockRequestHandle Send(const FString& Method, const FString& Url, const TMap<FString, FString>& Headers,
		const FString& Body, bool bHasBody, bool bUnwrap, TFunction<void(TFlockResult<T>)> OnComplete)
	{
		FFlockHttpRequest Request = MakeRequest(Method, Url, Headers, Body, bHasBody);
		Logger->LogDebug(FString::Printf(TEXT("%s %s"), *Method, *Url));

		const TSharedRef<FFlockHttpClient> Self = AsShared();
		return Adapter->SendAsync(Request, [Self, bUnwrap, OnComplete](FFlockHttpResponse Response)
		{
			if (!OnComplete)
			{
				return;
			}
			OnComplete(Self->ParseInto<T>(Response, bUnwrap));
		});
	}

	template <typename TResp, typename TBody>
	FFlockRequestHandle SendWithBody(const FString& Method, const FString& Url, const TMap<FString, FString>& Headers,
		const TBody& Body, bool bUnwrap, TFunction<void(TFlockResult<TResp>)> OnComplete)
	{
		FString Json;
		if (!FFlockJsonUtils::StructToWireJson(Body, Json))
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<TResp>::Fail(FFlockError::Make(EFlockErrorType::Serialization, TEXT("Failed to serialize request body"))));
			}
			return FFlockRequestHandle();
		}
		return Send<TResp>(Method, Url, Headers, Json, /*bHasBody*/ true, bUnwrap, MoveTemp(OnComplete));
	}

	TSharedRef<IFlockHttpAdapter> Adapter;
	TSharedRef<IFlockLogger> Logger;
	float TimeoutSeconds;
};
