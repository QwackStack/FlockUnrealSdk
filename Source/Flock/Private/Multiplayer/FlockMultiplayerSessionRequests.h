// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockMultiplayerModels.h"

/**
 * The multiplayer session routes on the wire, each sent as the sign-in it was asked for or not at all: every attempt, retries
 * included, fails Cancelled without sending once that sign-in has ended. Every route is enveloped.
 */
class FFlockMultiplayerSessionRequests : public FFlockProviderBase
{
public:
	using FOnSession = TFunction<void(TFlockResult<FFlockMultiplayerSessionReading>)>;

	FFlockMultiplayerSessionRequests(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
		const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession, const FString& InVersionedApiUrl);

	/** Not retried: a lost answer may mean the session was made, and a second try would make another. same_version_only is sent only as false. */
	FFlockRequestHandle Host(int32 SignInNumber, TOptional<int32> MaxPlayers, const FFlockCommandData& Data, bool bSameVersionOnly,
		FOnSession OnComplete);

	/** Retried: joining a session the player is already seated in answers 200. */
	FFlockRequestHandle Join(int32 SignInNumber, const FString& JoinCode, FOnSession OnComplete);

	/** The session as it is now; readable by anyone who ever held a seat in it, session_not_found for anyone else. */
	FFlockRequestHandle Read(int32 SignInNumber, const FString& SessionId, FOnSession OnComplete);

	/** The live session the player is seated in, found without its id; refused with session_not_found when seated nowhere. */
	FFlockRequestHandle ReadCurrent(int32 SignInNumber, FOnSession OnComplete);

	/** Keeps the seat and answers the session as it is now; a second beat after a lost answer only keeps it again. */
	FFlockRequestHandle Heartbeat(int32 SignInNumber, const FString& SessionId, FOnSession OnComplete);

	/** Retried: the caller reads "not a participant" after a lost answer as the leave having gone through. */
	FFlockRequestHandle Leave(int32 SignInNumber, const FString& SessionId, FOnSession OnComplete);

	/** Not retried: after a lost answer the session is over, and a second try would say it was not found. */
	FFlockRequestHandle End(int32 SignInNumber, const FString& SessionId, FOnSession OnComplete);

	/** Not retried: after a lost answer the player no longer hosts, and a second try would be refused. */
	FFlockRequestHandle MakeHost(int32 SignInNumber, const FString& SessionId, const FString& PlayerId, FOnSession OnComplete);

	/** Retried: publishing the same connection again only moves its epoch. */
	FFlockRequestHandle PublishConnection(int32 SignInNumber, const FString& SessionId, const FFlockCommandData& Connection,
		FOnSession OnComplete);

	FFlockRequestHandle RequestJoinToken(int32 SignInNumber, const FString& SessionId,
		TFunction<void(TFlockResult<FFlockMultiplayerSessionJoinToken>)> OnComplete);

	FFlockRequestHandle VerifyJoinToken(int32 SignInNumber, const FString& SessionId, const FString& Token,
		TFunction<void(TFlockResult<FFlockVerifiedJoinToken>)> OnComplete);

	/**
	 * The STUN servers Flock lists, asked with no session so a direct connection is not counted as relay use of the player's
	 * seat. Retried: a read. Each ask takes one of the player's relay requests a minute, so ask once a launch.
	 */
	FFlockRequestHandle ReadStunServers(int32 SignInNumber, TFunction<void(TFlockResult<FFlockStunServerList>)> OnComplete);

private:
	FString Url(const FString& Path) const { return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Path); }

	/** Sends one enveloped request as the given sign-in, under the retry policy, with the token read at each attempt. */
	template <typename T>
	FFlockRequestHandle SendAs(int32 SignInNumber, const FString& Context, bool bIdempotent,
		TFunction<FFlockRequestHandle(const TMap<FString, FString>& Headers, TFunction<void(TFlockResult<T>)> OnResult)> Send,
		TFunction<void(TFlockResult<T>)> OnComplete, TFunction<bool(const FFlockError&)> IsExpectedFailure = nullptr);

	/** A POST of Json to RequestUrl, answered with the enveloped T. */
	template <typename T>
	FFlockRequestHandle PostAs(int32 SignInNumber, const FString& Context, bool bIdempotent, const FString& RequestUrl, const FString& Json,
		TFunction<void(TFlockResult<T>)> OnComplete, TFunction<bool(const FFlockError&)> IsExpectedFailure = nullptr);

	TSharedRef<FFlockAuthSession> Session;
	FString VersionedApiUrl;
};
