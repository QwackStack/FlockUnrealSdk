// Copyright 2022, Qwacks. All Rights Reserved.

#include "Http/FlockHttpModuleAdapter.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

FFlockHttpModuleAdapter::FFlockHttpModuleAdapter(float InDefaultTimeoutSeconds)
	: DefaultTimeoutSeconds(InDefaultTimeoutSeconds)
{
}

FFlockRequestHandle FFlockHttpModuleAdapter::SendAsync(const FFlockHttpRequest& Request, TFunction<void(FFlockHttpResponse)> OnComplete)
{
	const FHttpRequestRef HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(Request.Url);
	HttpRequest->SetVerb(Request.Method);

	for (const TPair<FString, FString>& Header : Request.Headers)
	{
		if (!Header.Value.IsEmpty())
		{
			HttpRequest->SetHeader(Header.Key, Header.Value);
		}
	}

	if (Request.bHasBody)
	{
		HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		HttpRequest->SetContentAsString(Request.JsonBody);
	}

	const float Timeout = Request.TimeoutSeconds > 0.f ? Request.TimeoutSeconds : DefaultTimeoutSeconds;
	if (Timeout > 0.f)
	{
		HttpRequest->SetTimeout(Timeout);
	}

	// The completion handler checks bCancelled to suppress a late callback after CancelRequest().
	const TSharedPtr<FFlockCancelToken> Token = MakeShared<FFlockCancelToken>();
	const TWeakPtr<IHttpRequest, ESPMode::ThreadSafe> WeakRequest = HttpRequest;
	Token->Canceller = [WeakRequest]()
	{
		if (const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Pinned = WeakRequest.Pin())
		{
			Pinned->CancelRequest();
		}
	};

	HttpRequest->OnProcessRequestComplete().BindLambda(
		[OnComplete, Token](FHttpRequestPtr HttpRequestPtr, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
		{
			if (!OnComplete)
			{
				return;
			}

			FFlockHttpResponse Out;

			if (Token->bCancelled)
			{
				Out.Result = EFlockHttpResult::Cancelled;
				OnComplete(Out);
				return;
			}

			if (bConnectedSuccessfully && HttpResponse.IsValid())
			{
				Out.Result = EFlockHttpResult::Success;
				Out.StatusCode = HttpResponse->GetResponseCode();
				Out.Body = HttpResponse->GetContentAsString();
				Out.RetryAfterHeader = HttpResponse->GetHeader(TEXT("Retry-After"));
				OnComplete(Out);
				return;
			}

			// No HTTP response — classify the transport failure. GetFailureReason() is UE 5.4+.
			const EHttpFailureReason Reason = HttpRequestPtr.IsValid()
				? HttpRequestPtr->GetFailureReason()
				: EHttpFailureReason::Other;
			switch (Reason)
			{
			case EHttpFailureReason::TimedOut:
				Out.Result = EFlockHttpResult::Timeout;
				break;
			case EHttpFailureReason::Cancelled:
				Out.Result = EFlockHttpResult::Cancelled;
				break;
			default:
				Out.Result = EFlockHttpResult::ConnectionError;
				Out.Body = TEXT("Network request failed");
				break;
			}
			OnComplete(Out);
		});

	if (!HttpRequest->ProcessRequest())
	{
		FFlockHttpResponse Out;
		Out.Result = EFlockHttpResult::ConnectionError;
		Out.Body = TEXT("Failed to start HTTP request");
		OnComplete(Out);
	}

	return FFlockRequestHandle(Token);
}
