// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockMultiplayerSessionRequests.h"
#include "Http/FlockEndpoints.h"

FFlockMultiplayerSessionRequests::FFlockMultiplayerSessionRequests(const TSharedRef<FFlockHttpClient>& InClient,
	const FFlockRetryPolicy& InPolicy, const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
	const FString& InVersionedApiUrl)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, VersionedApiUrl(InVersionedApiUrl)
{
	SetAuthSession(InSession);
}

template <typename T>
FFlockRequestHandle FFlockMultiplayerSessionRequests::SendAs(int32 SignInNumber, const FString& Context, bool bIdempotent,
	TFunction<FFlockRequestHandle(const TMap<FString, FString>& Headers, TFunction<void(TFlockResult<T>)> OnResult)> Send,
	TFunction<void(TFlockResult<T>)> OnComplete, TFunction<bool(const FFlockError&)> IsExpectedFailure)
{
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	// The token is read at each attempt, so a refresh's replay carries the new one; the sign-in rule stops every attempt
	// once the sign-in asked for has ended.
	return Execute<T>(
		ForSignInOnly<T>(SessionRef, SignInNumber,
			[SessionRef, Send](TFunction<void(TFlockResult<T>)> OnResult) { return Send(SessionRef->GetAuthHeaders(), MoveTemp(OnResult)); }),
		MoveTemp(OnComplete), Context, bIdempotent, /*MaxRetriesOverride*/ -1, /*bAllowAuthRetry*/ true, MoveTemp(IsExpectedFailure));
}

template <typename T>
FFlockRequestHandle FFlockMultiplayerSessionRequests::PostAs(int32 SignInNumber, const FString& Context, bool bIdempotent,
	const FString& RequestUrl, const FString& Json, TFunction<void(TFlockResult<T>)> OnComplete, TFunction<bool(const FFlockError&)> IsExpectedFailure)
{
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	return SendAs<T>(SignInNumber, Context, bIdempotent,
		[ClientRef, RequestUrl, Json](const TMap<FString, FString>& Headers, TFunction<void(TFlockResult<T>)> OnResult)
		{
			return ClientRef->PostJson<T>(RequestUrl, Headers, Json, MoveTemp(OnResult));
		},
		MoveTemp(OnComplete), MoveTemp(IsExpectedFailure));
}

FFlockRequestHandle FFlockMultiplayerSessionRequests::Host(int32 SignInNumber, TOptional<int32> MaxPlayers, const FFlockCommandData& Data,
	bool bSameVersionOnly, FOnSession OnComplete)
{
	// What is not given is left out, so the server's defaults apply (the game's own player count, no data, same version only).
	FFlockCommandData Body;
	if (MaxPlayers.IsSet())
	{
		Body.Set(TEXT("max_players"), MaxPlayers.GetValue());
	}
	if (!Data.IsEmpty())
	{
		Body.SetRawJson(TEXT("data"), Data.ToJsonString());
	}
	if (!bSameVersionOnly)
	{
		Body.Set(TEXT("same_version_only"), false);
	}
	return PostAs<FFlockMultiplayerSessionReading>(SignInNumber, TEXT("Host session"), /*bIdempotent*/ false, Url(FlockEndpoints::Sessions),
		Body.ToJsonString(), MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerSessionRequests::Join(int32 SignInNumber, const FString& JoinCode, FOnSession OnComplete)
{
	return PostAs<FFlockMultiplayerSessionReading>(SignInNumber, TEXT("Join session"), /*bIdempotent*/ true, Url(FlockEndpoints::SessionJoin),
		FFlockCommandData().Set(TEXT("join_code"), JoinCode).ToJsonString(), MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerSessionRequests::Read(int32 SignInNumber, const FString& SessionId, FOnSession OnComplete)
{
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString RequestUrl = Url(FlockEndpoints::SessionById(SessionId));
	return SendAs<FFlockMultiplayerSessionReading>(SignInNumber, TEXT("Read session"), /*bIdempotent*/ true,
		[ClientRef, RequestUrl](const TMap<FString, FString>& Headers, FOnSession OnResult)
		{
			return ClientRef->Get<FFlockMultiplayerSessionReading>(RequestUrl, Headers, MoveTemp(OnResult));
		},
		MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerSessionRequests::ReadCurrent(int32 SignInNumber, FOnSession OnComplete)
{
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString RequestUrl = Url(FlockEndpoints::SessionCurrent);
	return SendAs<FFlockMultiplayerSessionReading>(SignInNumber, TEXT("Read my session"), /*bIdempotent*/ true,
		[ClientRef, RequestUrl](const TMap<FString, FString>& Headers, FOnSession OnResult)
		{
			return ClientRef->Get<FFlockMultiplayerSessionReading>(RequestUrl, Headers, MoveTemp(OnResult));
		},
		MoveTemp(OnComplete),
		// Seated nowhere is an answer here, which the caller turns into "no session", so it is not logged as a failure.
		[](const FFlockError& Failure) { return Failure.ErrorCode == EFlockErrorCode::MultiplayerSessionNotFound; });
}

FFlockRequestHandle FFlockMultiplayerSessionRequests::Heartbeat(int32 SignInNumber, const FString& SessionId, FOnSession OnComplete)
{
	return PostAs<FFlockMultiplayerSessionReading>(SignInNumber, TEXT("Session heartbeat"), /*bIdempotent*/ true,
		Url(FlockEndpoints::SessionHeartbeat(SessionId)), TEXT("{}"), MoveTemp(OnComplete),
		// A seat the server gave up ends the session as dropped, an outcome the game hears of, so it is not logged as a failure.
		[](const FFlockError& Failure)
		{
			return Failure.ErrorCode == EFlockErrorCode::MultiplayerSessionNotFound || Failure.ErrorCode == EFlockErrorCode::MultiplayerNotAParticipant;
		});
}

FFlockRequestHandle FFlockMultiplayerSessionRequests::Leave(int32 SignInNumber, const FString& SessionId, FOnSession OnComplete)
{
	return PostAs<FFlockMultiplayerSessionReading>(SignInNumber, TEXT("Leave session"), /*bIdempotent*/ true,
		Url(FlockEndpoints::SessionLeave(SessionId)), TEXT("{}"), MoveTemp(OnComplete),
		// Out of the session already is what leaving asks for, so it is not logged as a failure.
		[](const FFlockError& Failure)
		{
			return Failure.ErrorCode == EFlockErrorCode::MultiplayerNotAParticipant || Failure.ErrorCode == EFlockErrorCode::MultiplayerSessionNotFound;
		});
}

FFlockRequestHandle FFlockMultiplayerSessionRequests::End(int32 SignInNumber, const FString& SessionId, FOnSession OnComplete)
{
	return PostAs<FFlockMultiplayerSessionReading>(SignInNumber, TEXT("End session"), /*bIdempotent*/ false,
		Url(FlockEndpoints::SessionEnd(SessionId)), TEXT("{}"), MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerSessionRequests::MakeHost(int32 SignInNumber, const FString& SessionId, const FString& PlayerId,
	FOnSession OnComplete)
{
	return PostAs<FFlockMultiplayerSessionReading>(SignInNumber, TEXT("Make session host"), /*bIdempotent*/ false,
		Url(FlockEndpoints::SessionHost(SessionId)), FFlockCommandData().Set(TEXT("player_id"), PlayerId).ToJsonString(), MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerSessionRequests::PublishConnection(int32 SignInNumber, const FString& SessionId,
	const FFlockCommandData& Connection, FOnSession OnComplete)
{
	return PostAs<FFlockMultiplayerSessionReading>(SignInNumber, TEXT("Publish session connection"), /*bIdempotent*/ true,
		Url(FlockEndpoints::SessionConnection(SessionId)),
		FFlockCommandData().SetRawJson(TEXT("connection_info"), Connection.ToJsonString()).ToJsonString(), MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerSessionRequests::RequestJoinToken(int32 SignInNumber, const FString& SessionId,
	TFunction<void(TFlockResult<FFlockMultiplayerSessionJoinToken>)> OnComplete)
{
	return PostAs<FFlockMultiplayerSessionJoinToken>(SignInNumber, TEXT("Request join token"), /*bIdempotent*/ true,
		Url(FlockEndpoints::SessionJoinToken(SessionId)), TEXT("{}"), MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerSessionRequests::VerifyJoinToken(int32 SignInNumber, const FString& SessionId, const FString& Token,
	TFunction<void(TFlockResult<FFlockVerifiedJoinToken>)> OnComplete)
{
	return PostAs<FFlockVerifiedJoinToken>(SignInNumber, TEXT("Verify join token"), /*bIdempotent*/ true,
		Url(FlockEndpoints::SessionVerifyJoinToken(SessionId)), FFlockCommandData().Set(TEXT("token"), Token).ToJsonString(),
		MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerSessionRequests::ReadStunServers(int32 SignInNumber,
	TFunction<void(TFlockResult<FFlockStunServerList>)> OnComplete)
{
	return PostAs<FFlockStunServerList>(SignInNumber, TEXT("Read the servers to connect through"), /*bIdempotent*/ true,
		Url(FlockEndpoints::RelayCredentials), TEXT("{}"), MoveTemp(OnComplete));
}
