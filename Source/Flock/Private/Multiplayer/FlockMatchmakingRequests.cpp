// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockMatchmakingRequests.h"
#include "Http/FlockEndpoints.h"
#include "Multiplayer/FlockMatchmakingQueueNames.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FFlockMatchmakingRequests::FFlockMatchmakingRequests(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
	const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession, const FString& InVersionedApiUrl)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, VersionedApiUrl(InVersionedApiUrl)
{
	SetAuthSession(InSession);
}

template <typename T>
FFlockRequestHandle FFlockMatchmakingRequests::SendAs(int32 SignInNumber, const FString& Context,
	TFunction<FFlockRequestHandle(const TMap<FString, FString>& Headers, TFunction<void(TFlockResult<T>)> OnResult)> Send,
	TFunction<void(TFlockResult<T>)> OnComplete, TFunction<bool(const FFlockError&)> IsExpectedFailure, int32 MaxRetriesOverride)
{
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	// The token is read at each attempt, so a refresh's replay carries the new one; the sign-in rule stops every attempt
	// once the sign-in asked for has ended.
	return Execute<T>(
		ForSignInOnly<T>(SessionRef, SignInNumber,
			[SessionRef, Send](TFunction<void(TFlockResult<T>)> OnResult) { return Send(SessionRef->GetAuthHeaders(), MoveTemp(OnResult)); }),
		MoveTemp(OnComplete), Context, /*bIdempotent*/ true, MaxRetriesOverride, /*bAllowAuthRetry*/ true, MoveTemp(IsExpectedFailure));
}

FFlockRequestHandle FFlockMatchmakingRequests::Create(int32 SignInNumber, const FString& QueueId, const FString& PartyId,
	const FFlockCommandData& Attributes, FOnTicketsMade OnComplete)
{
	// What is not given is left out: no party is a search alone, and no attributes sends none.
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("queue_id"), QueueId);
	if (!PartyId.IsEmpty())
	{
		Body->SetStringField(TEXT("party_id"), PartyId);
	}
	if (!Attributes.IsEmpty())
	{
		Body->SetObjectField(TEXT("attributes"), Attributes.ToJsonObject());
	}
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString RequestUrl = Url(FlockEndpoints::MatchmakingTicket);
	const FString Json = ToJson(Body);
	return SendAs<FFlockMatchmakingTicketsMade>(SignInNumber, TEXT("Find match"),
		[ClientRef, RequestUrl, Json](const TMap<FString, FString>& Headers, FOnTicketsMade OnResult)
		{
			// Raw, because the result is a list of tickets, which the reading reads from the envelope itself.
			return ClientRef->PostJsonRaw<FFlockMatchmakingTicketsMade>(RequestUrl, Headers, Json, MoveTemp(OnResult));
		},
		MoveTemp(OnComplete),
		[](const FFlockError& Failure) { return Failure.ErrorCode == EFlockErrorCode::MatchmakingAlreadyQueued; });
}

FFlockRequestHandle FFlockMatchmakingRequests::Read(int32 SignInNumber, const FString& TicketId, FOnTicket OnComplete)
{
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString RequestUrl = Url(FlockEndpoints::MatchmakingTicketById(TicketId));
	return SendAs<FFlockMatchmakingTicketReading>(SignInNumber, TEXT("Read matchmaking search"),
		[ClientRef, RequestUrl](const TMap<FString, FString>& Headers, FOnTicket OnResult)
		{
			return ClientRef->Get<FFlockMatchmakingTicketReading>(RequestUrl, Headers, MoveTemp(OnResult));
		},
		MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMatchmakingRequests::ReadCurrent(int32 SignInNumber, FOnTicket OnComplete, bool bTryOnce)
{
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString RequestUrl = Url(FlockEndpoints::MatchmakingTicketCurrent);
	return SendAs<FFlockMatchmakingTicketReading>(SignInNumber, TEXT("Read my matchmaking search"),
		[ClientRef, RequestUrl](const TMap<FString, FString>& Headers, FOnTicket OnResult)
		{
			return ClientRef->Get<FFlockMatchmakingTicketReading>(RequestUrl, Headers, MoveTemp(OnResult));
		},
		MoveTemp(OnComplete),
		[](const FFlockError& Failure) { return Failure.ErrorCode == EFlockErrorCode::MatchmakingTicketNotFound; },
		bTryOnce ? 0 : -1);
}

FFlockRequestHandle FFlockMatchmakingRequests::ReadQueues(TFunction<void(TFlockResult<TArray<FFlockMatchmakingQueue>>)> OnComplete)
{
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString RequestUrl = FString::Printf(TEXT("%s?limit=%d"), *Url(FlockEndpoints::MatchmakingQueues),
		FFlockMatchmakingQueueNames::MostQueuesListed);
	// The API key alone: the list is the game's, so no player's token goes with it and a 401 is never a lapsed sign-in.
	const TMap<FString, FString> Headers = Session->GetBaseHeaders();
	return Execute<TArray<FFlockMatchmakingQueue>>(
		[ClientRef, RequestUrl, Headers](TFunction<void(TFlockResult<TArray<FFlockMatchmakingQueue>>)> OnResult)
		{
			return ClientRef->GetList<FFlockMatchmakingQueue>(RequestUrl, Headers, MoveTemp(OnResult));
		},
		MoveTemp(OnComplete), TEXT("Read matchmaking queues"), /*bIdempotent*/ true, /*MaxRetriesOverride*/ -1,
		/*bAllowAuthRetry*/ false);
}

FFlockRequestHandle FFlockMatchmakingRequests::Cancel(int32 SignInNumber, const FString& TicketId, FOnTicket OnComplete)
{
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString RequestUrl = Url(FlockEndpoints::MatchmakingTicketById(TicketId));
	return SendAs<FFlockMatchmakingTicketReading>(SignInNumber, TEXT("Cancel matchmaking search"),
		[ClientRef, RequestUrl](const TMap<FString, FString>& Headers, FOnTicket OnResult)
		{
			return ClientRef->Delete<FFlockMatchmakingTicketReading>(RequestUrl, Headers, MoveTemp(OnResult));
		},
		MoveTemp(OnComplete),
		[](const FFlockError& Failure) { return Failure.ErrorCode == EFlockErrorCode::MatchmakingTicketNotCancelable; });
}

FString FFlockMatchmakingRequests::ToJson(const TSharedRef<FJsonObject>& Object)
{
	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Object, Writer);
	return Json;
}
