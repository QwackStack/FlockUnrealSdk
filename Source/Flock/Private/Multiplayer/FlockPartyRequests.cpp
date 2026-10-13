// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockPartyRequests.h"
#include "Http/FlockEndpoints.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FFlockPartyRequests::FFlockPartyRequests(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
	const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession, const FString& InVersionedApiUrl)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, VersionedApiUrl(InVersionedApiUrl)
{
	SetAuthSession(InSession);
}

FFlockRequestHandle FFlockPartyRequests::SendAs(int32 SignInNumber, const FString& Context, bool bIdempotent,
	TFunction<FFlockRequestHandle(const TMap<FString, FString>& Headers, FOnParty OnResult)> Send, FOnParty OnComplete,
	TFunction<bool(const FFlockError&)> IsExpectedFailure)
{
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	// The token is read at each attempt, so a refresh's replay carries the new one; the sign-in rule stops every attempt
	// once the sign-in asked for has ended.
	return Execute<FFlockPartyReading>(
		ForSignInOnly<FFlockPartyReading>(SessionRef, SignInNumber,
			[SessionRef, Send](FOnParty OnResult) { return Send(SessionRef->GetAuthHeaders(), MoveTemp(OnResult)); }),
		MoveTemp(OnComplete), Context, bIdempotent, /*MaxRetriesOverride*/ -1, /*bAllowAuthRetry*/ true, MoveTemp(IsExpectedFailure));
}

FFlockRequestHandle FFlockPartyRequests::Create(int32 SignInNumber, TOptional<int32> MaxSize, const FFlockCommandData& Settings,
	FOnParty OnComplete)
{
	// What is not given is left out, so the server's defaults apply (4 players, no settings).
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	if (MaxSize.IsSet())
	{
		Body->SetNumberField(TEXT("max_size"), MaxSize.GetValue());
	}
	if (!Settings.IsEmpty())
	{
		Body->SetObjectField(TEXT("settings"), Settings.ToJsonObject());
	}
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString RequestUrl = Url(FlockEndpoints::Party);
	const FString Json = ToJson(Body);
	return SendAs(SignInNumber, TEXT("Create party"), /*bIdempotent*/ false,
		[ClientRef, RequestUrl, Json](const TMap<FString, FString>& Headers, FOnParty OnResult)
		{
			return ClientRef->PostJson<FFlockPartyReading>(RequestUrl, Headers, Json, MoveTemp(OnResult));
		},
		MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockPartyRequests::Join(int32 SignInNumber, const FString& InviteCode, FOnParty OnComplete)
{
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("invite_code"), InviteCode);
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString RequestUrl = Url(FlockEndpoints::PartyJoin);
	const FString Json = ToJson(Body);
	return SendAs(SignInNumber, TEXT("Join party"), /*bIdempotent*/ true,
		[ClientRef, RequestUrl, Json](const TMap<FString, FString>& Headers, FOnParty OnResult)
		{
			return ClientRef->PostJson<FFlockPartyReading>(RequestUrl, Headers, Json, MoveTemp(OnResult));
		},
		MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockPartyRequests::Read(int32 SignInNumber, const FString& PartyId, FOnParty OnComplete)
{
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString RequestUrl = Url(FlockEndpoints::PartyById(PartyId));
	return SendAs(SignInNumber, TEXT("Read party"), /*bIdempotent*/ true,
		[ClientRef, RequestUrl](const TMap<FString, FString>& Headers, FOnParty OnResult)
		{
			return ClientRef->Get<FFlockPartyReading>(RequestUrl, Headers, MoveTemp(OnResult));
		},
		MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockPartyRequests::ReadMine(int32 SignInNumber, TFunction<void(TFlockResult<FFlockMyPartyReading>)> OnComplete)
{
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString RequestUrl = Url(FlockEndpoints::PartyMine);
	// Raw, because a null result is the server's "in no party", which the enveloped verbs read as a misread answer.
	return Execute<FFlockMyPartyReading>(
		ForSignInOnly<FFlockMyPartyReading>(SessionRef, SignInNumber,
			[SessionRef, ClientRef, RequestUrl](TFunction<void(TFlockResult<FFlockMyPartyReading>)> OnResult)
			{
				return ClientRef->GetRaw<FFlockMyPartyReading>(RequestUrl, SessionRef->GetAuthHeaders(), MoveTemp(OnResult));
			}),
		MoveTemp(OnComplete), TEXT("Read my party"));
}

FFlockRequestHandle FFlockPartyRequests::Leave(int32 SignInNumber, const FString& PartyId, FOnParty OnComplete)
{
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString RequestUrl = Url(FlockEndpoints::PartyLeave(PartyId));
	return SendAs(SignInNumber, TEXT("Leave party"), /*bIdempotent*/ true,
		[ClientRef, RequestUrl](const TMap<FString, FString>& Headers, FOnParty OnResult)
		{
			return ClientRef->PostJson<FFlockPartyReading>(RequestUrl, Headers, TEXT("{}"), MoveTemp(OnResult));
		},
		MoveTemp(OnComplete),
		// Out of the party already is what leaving asks for, so it is not logged as a failure.
		[](const FFlockError& Failure)
		{
			return Failure.ErrorCode == EFlockErrorCode::PartyNotAMember || Failure.ErrorCode == EFlockErrorCode::PartyNotFound;
		});
}

FFlockRequestHandle FFlockPartyRequests::Kick(int32 SignInNumber, const FString& PartyId, const FString& PlayerId, FOnParty OnComplete)
{
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("player_id"), PlayerId);
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString RequestUrl = Url(FlockEndpoints::PartyKick(PartyId));
	const FString Json = ToJson(Body);
	return SendAs(SignInNumber, TEXT("Remove party member"), /*bIdempotent*/ false,
		[ClientRef, RequestUrl, Json](const TMap<FString, FString>& Headers, FOnParty OnResult)
		{
			return ClientRef->PostJson<FFlockPartyReading>(RequestUrl, Headers, Json, MoveTemp(OnResult));
		},
		MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockPartyRequests::MakeLeader(int32 SignInNumber, const FString& PartyId, const FString& PlayerId,
	FOnParty OnComplete)
{
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("player_id"), PlayerId);
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString RequestUrl = Url(FlockEndpoints::PartyTransfer(PartyId));
	const FString Json = ToJson(Body);
	return SendAs(SignInNumber, TEXT("Make party leader"), /*bIdempotent*/ false,
		[ClientRef, RequestUrl, Json](const TMap<FString, FString>& Headers, FOnParty OnResult)
		{
			return ClientRef->PostJson<FFlockPartyReading>(RequestUrl, Headers, Json, MoveTemp(OnResult));
		},
		MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockPartyRequests::Update(int32 SignInNumber, const FString& PartyId, TOptional<int32> MaxSize,
	const TOptional<FFlockCommandData>& Settings, FOnParty OnComplete)
{
	// What is not given is left out, and the server keeps it.
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	if (MaxSize.IsSet())
	{
		Body->SetNumberField(TEXT("max_size"), MaxSize.GetValue());
	}
	if (Settings.IsSet())
	{
		Body->SetObjectField(TEXT("settings"), Settings.GetValue().ToJsonObject());
	}
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString RequestUrl = Url(FlockEndpoints::PartyById(PartyId));
	const FString Json = ToJson(Body);
	return SendAs(SignInNumber, TEXT("Update party"), /*bIdempotent*/ true,
		[ClientRef, RequestUrl, Json](const TMap<FString, FString>& Headers, FOnParty OnResult)
		{
			return ClientRef->PatchJson<FFlockPartyReading>(RequestUrl, Headers, Json, MoveTemp(OnResult));
		},
		MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockPartyRequests::Disband(int32 SignInNumber, const FString& PartyId, FOnParty OnComplete)
{
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString RequestUrl = Url(FlockEndpoints::PartyById(PartyId));
	return SendAs(SignInNumber, TEXT("Disband party"), /*bIdempotent*/ false,
		[ClientRef, RequestUrl](const TMap<FString, FString>& Headers, FOnParty OnResult)
		{
			return ClientRef->Delete<FFlockPartyReading>(RequestUrl, Headers, MoveTemp(OnResult));
		},
		MoveTemp(OnComplete));
}

FString FFlockPartyRequests::ToJson(const TSharedRef<FJsonObject>& Object)
{
	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Object, Writer);
	return Json;
}
