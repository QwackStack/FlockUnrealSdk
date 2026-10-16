// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Http/FlockHttpClient.h"
#include "Http/FlockEndpoints.h"
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
			// A negative value would make the retry wait less than nothing; it means "now".
			OutSeconds = FMath::Max(0.f, FCString::Atof(*HeaderValue));
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

	/**
	 * What a failed status means, for an answer that gave no reason of its own. Flock's game API always gives one, so
	 * these are mostly read on answers from something else: a proxy, a captive portal, another server.
	 */
	FString DescribeStatus(int32 StatusCode)
	{
		switch (StatusCode)
		{
		case 400: return TEXT("The server could not use the request");
		case 401: return TEXT("The server did not accept the request's credentials");
		case 403: return TEXT("The server refused to do this");
		case 404: return TEXT("The server found nothing at this address");
		case 405: return TEXT("The server does not take this kind of request at this address");
		case 408: return TEXT("The server stopped waiting for the request");
		case 409: return TEXT("The request clashes with what the server holds now");
		case 413: return TEXT("The request is larger than the server accepts");
		case 422: return TEXT("The server refused a value in the request");
		case 429: return TEXT("The server is getting too many requests from this game");
		default:
			return StatusCode >= 500
				? FString(TEXT("The server failed while handling the request"))
				: FString(TEXT("The server refused the request"));
		}
	}
}

TSharedRef<FFlockHttpClient> FFlockHttpClient::CreateDefault(float TimeoutSeconds, const TSharedRef<IFlockLogger>& Logger)
{
	const TSharedRef<IFlockHttpAdapter> Adapter = MakeShared<FFlockHttpModuleAdapter>(TimeoutSeconds);
	return MakeShared<FFlockHttpClient>(Adapter, Logger, TimeoutSeconds);
}

void FFlockHttpClient::NoteReachability(const FFlockHttpResponse& Response)
{
	switch (Response.Result)
	{
	case EFlockHttpResult::Success:
		// The exchange completed. Any status at all — 200, 401, 500 — took a round trip, so the network
		// is up whatever the server thought of the request.
		OfflineSinceSeconds = -1.0;
		break;
	case EFlockHttpResult::ConnectionError:
		// No HTTP response: DNS, refused, or no route. Re-latch on every such failure rather than only
		// the first, so a long offline stretch keeps extending the window instead of lapsing mid-outage.
		OfflineSinceSeconds = FPlatformTime::Seconds();
		break;
	case EFlockHttpResult::Timeout:
	case EFlockHttpResult::Cancelled:
		// Neither says anything about reachability. See NoteReachability's declaration for why a timeout
		// must not latch.
		break;
	}
}

bool FFlockHttpClient::IsLikelyOffline() const
{
	if (OfflineSinceSeconds < 0.0)
	{
		return false;
	}
	return (FPlatformTime::Seconds() - OfflineSinceSeconds) < OfflineLatchSeconds;
}

bool FFlockHttpClient::ClassifyResponse(const FFlockHttpResponse& Response, FFlockError& OutError, FString& OutSuccessBody,
	bool bBodyRequired) const
{
	switch (Response.Result)
	{
	case EFlockHttpResult::Cancelled:
		OutError = FFlockError::Make(EFlockErrorType::Cancelled, TEXT("The request was cancelled before an answer came"));
		return false;
	case EFlockHttpResult::Timeout:
		OutError = FFlockError::Make(EFlockErrorType::Timeout, TEXT("The server did not answer in time"));
		return false;
	case EFlockHttpResult::ConnectionError:
		OutError = FFlockError::Make(EFlockErrorType::Connection, TEXT("The request could not reach the server"));
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
		if (Code == 401 || Code == 403)
		{
			Type = EFlockErrorType::Auth;
		}
		else if (Code == 400 || Code == 422)
		{
			Type = EFlockErrorType::Validation;
		}
		else
		{
			Type = EFlockErrorType::Network;
		}

		// What is wrong is the server's own reason when it gave one; the status's plain meaning otherwise.
		const FString Message = ServerMessage.IsEmpty() ? DescribeStatus(Code) : ServerMessage;
		OutError = FFlockError::Make(Type, Message, Code, Response.Body, Coded, ServerMessage);

		float Seconds = 0.f;
		if (TryParseRetryAfter(Response.RetryAfterHeader, Seconds))
		{
			OutError.bHasRetryAfter = true;
			OutError.RetryAfterSeconds = Seconds;
		}
		return false;
	}

	// A read exists for its body, so a 2xx without one still fails it; a send has nothing to read.
	if (bBodyRequired && Response.Body.IsEmpty())
	{
		OutError = FFlockError::Make(EFlockErrorType::Serialization, TEXT("The server's answer was empty, so there was nothing to read"), Code);
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

	// A cancel is the game's or the SDK's own choice, so it is no failure and stays out of the warning channel.
	if (Response.Result == EFlockHttpResult::Cancelled)
	{
		Logger->LogDebug(FString::Printf(TEXT("<- %s %s [%s] cancelled after %.0f ms"), *Method, *Url, *Origin, ElapsedMs));
		return;
	}
	// A timeout reached the network and heard nothing back in time, which is not the same as not reaching the server.
	if (Response.Result == EFlockHttpResult::Timeout)
	{
		Logger->LogWarning(FString::Printf(TEXT("<- %s %s [%s] had no answer within the timeout (%.0f ms)"),
			*Method, *Url, *Origin, ElapsedMs));
		return;
	}
	if (Response.Result != EFlockHttpResult::Success)
	{
		Logger->LogWarning(FString::Printf(TEXT("<- %s %s [%s] failed to reach the server after %.0f ms: %s"),
			*Method, *Url, *Origin, ElapsedMs, *Detail));
		return;
	}
	// "The player's search" answered not found means the player has none, which a party refresh asks every few seconds:
	// an answer, not a failure, so it stays out of the warning channel.
	if (Response.StatusCode == 404 && Url.EndsWith(FString(TEXT("/")) + FlockEndpoints::MatchmakingTicketCurrent, ESearchCase::CaseSensitive)
		&& Response.Body.Contains(TEXT("matchmaking.ticket_not_found"), ESearchCase::CaseSensitive))
	{
		Logger->LogDebug(FString::Printf(TEXT("<- 404 %s %s (%.0f ms) [%s]: the player has no search"), *Method, *Url, ElapsedMs, *Origin));
		return;
	}
	// A join token a connecting player showed the host and Flock did not vouch for is Flock's answer, which the host's check turns
	// into a refusal: no failure of the host's own call. Anyone who reaches a host can send one, so a warning here would let them
	// fill the host's log (a flood of made-up tokens wrote one a check, 60 a minute).
	if (Response.StatusCode == 400 && Url.EndsWith(FlockEndpoints::SessionVerifyJoinTokenEnding, ESearchCase::CaseSensitive)
		&& Response.Body.Contains(TEXT("multiplayer.invalid_join_token"), ESearchCase::CaseSensitive))
	{
		Logger->LogDebug(FString::Printf(TEXT("<- 400 %s %s (%.0f ms) [%s]: Flock did not vouch for the join token, so the host turns that player away"),
			*Method, *Url, ElapsedMs, *Origin));
		return;
	}
	Logger->LogWarning(FString::Printf(TEXT("<- %d %s %s (%.0f ms) [%s]: %s"),
		Response.StatusCode, *Method, *Url, ElapsedMs, *Origin, *Detail));
}
