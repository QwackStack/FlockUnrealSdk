// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Http/FlockHttpClient.h"
#include "Http/FlockHttpModuleAdapter.h"

namespace
{
	/** Parses Retry-After as delta-seconds or an HTTP-date so the retry handler can honor it. */
	bool TryParseRetryAfter(const FString& HeaderValue, float& OutSeconds)
	{
		if (HeaderValue.IsEmpty())
		{
			return false;
		}
		if (HeaderValue.IsNumeric())
		{
			OutSeconds = FCString::Atof(*HeaderValue);
			return true;
		}
		FDateTime When;
		if (FDateTime::ParseHttpDate(HeaderValue, When))
		{
			const FTimespan Until = When - FDateTime::UtcNow();
			OutSeconds = static_cast<float>(FMath::Max(0.0, Until.GetTotalSeconds()));
			return true;
		}
		return false;
	}
}

TSharedRef<FFlockHttpClient> FFlockHttpClient::CreateDefault(float TimeoutSeconds, const TSharedRef<IFlockLogger>& Logger)
{
	const TSharedRef<IFlockHttpAdapter> Adapter = MakeShared<FFlockHttpModuleAdapter>(TimeoutSeconds);
	return MakeShared<FFlockHttpClient>(Adapter, Logger, TimeoutSeconds);
}

bool FFlockHttpClient::ClassifyResponse(const FFlockHttpResponse& Response, FFlockError& OutError, FString& OutSuccessBody) const
{
	switch (Response.Result)
	{
	case EFlockHttpResult::Cancelled:
		OutError = FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Request cancelled"));
		return false;
	case EFlockHttpResult::Timeout:
		OutError = FFlockError::Make(EFlockErrorType::Timeout, TEXT("Request timeout"));
		return false;
	case EFlockHttpResult::ConnectionError:
		OutError = FFlockError::Make(EFlockErrorType::Connection, TEXT("Network request failed"));
		OutError.Body = Response.Body;
		return false;
	case EFlockHttpResult::Success:
		break;
	}

	const int32 Code = Response.StatusCode;
	if (Code < 200 || Code >= 300)
	{
		FString Coded;
		FString ServerMessage;
		FFlockJsonUtils::ParseCodedError(Response.Body, Coded, ServerMessage);

		EFlockErrorType Type;
		FString Message;
		if (Code == 401 || Code == 403)
		{
			Type = EFlockErrorType::Auth;
			Message = FString::Printf(TEXT("Authentication failed (HTTP %d)"), Code);
		}
		else if (Code == 400 || Code == 422)
		{
			Type = EFlockErrorType::Validation;
			Message = FString::Printf(TEXT("Validation failed (HTTP %d)"), Code);
		}
		else
		{
			Type = EFlockErrorType::Network;
			Message = FString::Printf(TEXT("HTTP request failed (HTTP %d)"), Code);
		}

		OutError = FFlockError::Make(Type, Message, Code, Response.Body, Coded, ServerMessage);

		float Seconds = 0.f;
		if (TryParseRetryAfter(Response.RetryAfterHeader, Seconds))
		{
			OutError.bHasRetryAfter = true;
			OutError.RetryAfterSeconds = Seconds;
		}
		return false;
	}

	if (Response.Body.IsEmpty())
	{
		OutError = FFlockError::Make(EFlockErrorType::Serialization, TEXT("Empty response from server"), Code);
		return false;
	}

	OutSuccessBody = Response.Body;
	return true;
}

FString FFlockHttpClient::LogRequest(const FString& Method, const FString& Url) const
{
	const FString Origin = FFlockCallOrigin::Get();
	Logger->LogDebug(FString::Printf(TEXT("-> %s %s [%s]"), *Method, *Url, *Origin));
	return Origin;
}

void FFlockHttpClient::LogResponse(const FString& Method, const FString& Url, const FFlockHttpResponse& Response,
	double StartedAtSeconds, const FString& Origin) const
{
	const double ElapsedMs = (FPlatformTime::Seconds() - StartedAtSeconds) * 1000.0;

	if (Response.Result == EFlockHttpResult::Success && Response.StatusCode < 400)
	{
		Logger->LogDebug(FString::Printf(TEXT("<- %d %s %s (%.0f ms, %d bytes) [%s]"),
			Response.StatusCode, *Method, *Url, ElapsedMs, Response.Body.Len(), *Origin));
		return;
	}

	// Warning, not debug: someone chasing a failure has usually not turned debug logs on yet, and a
	// silent failure is the thing that sends them here in the first place. The body carries the server's
	// coded error, which is the single most useful line in the whole trace — and on a failure it is an
	// error document, not user data.
	const FString Detail = Response.Body.IsEmpty()
		? FString(TEXT("no response body"))
		: Response.Body.Left(512);

	if (Response.Result != EFlockHttpResult::Success)
	{
		Logger->LogWarning(FString::Printf(TEXT("<- %s %s [%s] failed to reach the server after %.0f ms: %s"),
			*Method, *Url, *Origin, ElapsedMs, *Detail));
		return;
	}
	Logger->LogWarning(FString::Printf(TEXT("<- %d %s %s (%.0f ms) [%s]: %s"),
		Response.StatusCode, *Method, *Url, ElapsedMs, *Origin, *Detail));
}
