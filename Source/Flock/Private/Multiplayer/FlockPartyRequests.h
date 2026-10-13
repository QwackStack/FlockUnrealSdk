// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockMultiplayerModels.h"

/**
 * The party routes on the wire, each sent as the sign-in it was asked for or not at all: every attempt, retries included,
 * fails Cancelled without sending once that sign-in has ended. Every route is enveloped.
 */
class FFlockPartyRequests : public FFlockProviderBase
{
public:
	using FOnParty = TFunction<void(TFlockResult<FFlockPartyReading>)>;

	FFlockPartyRequests(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
		const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession, const FString& InVersionedApiUrl);

	/** Not retried: a lost answer may mean the party was made, and a second try would say "already in a party". */
	FFlockRequestHandle Create(int32 SignInNumber, TOptional<int32> MaxSize, const FFlockCommandData& Settings, FOnParty OnComplete);

	/** Retried: joining the party the player is already in answers 200. */
	FFlockRequestHandle Join(int32 SignInNumber, const FString& InviteCode, FOnParty OnComplete);

	/** The party with its members; the server answers party.not_found to anyone not in it. */
	FFlockRequestHandle Read(int32 SignInNumber, const FString& PartyId, FOnParty OnComplete);

	/** The player's party with its members, or none. */
	FFlockRequestHandle ReadMine(int32 SignInNumber, TFunction<void(TFlockResult<FFlockMyPartyReading>)> OnComplete);

	/** Retried: the caller reads "not a member" after a lost answer as the leave having gone through. */
	FFlockRequestHandle Leave(int32 SignInNumber, const FString& PartyId, FOnParty OnComplete);

	/** Not retried: a second try after a lost answer would be refused for a player already gone. */
	FFlockRequestHandle Kick(int32 SignInNumber, const FString& PartyId, const FString& PlayerId, FOnParty OnComplete);

	/** Not retried: after a lost answer the player no longer leads, and a second try would be refused. */
	FFlockRequestHandle MakeLeader(int32 SignInNumber, const FString& PartyId, const FString& PlayerId, FOnParty OnComplete);

	/** Retried: the same size and settings sent twice leave the party as once. New settings replace all of the old ones. */
	FFlockRequestHandle Update(int32 SignInNumber, const FString& PartyId, TOptional<int32> MaxSize,
		const TOptional<FFlockCommandData>& Settings, FOnParty OnComplete);

	/** Not retried: after a lost answer the party is gone, and a second try would say it was not found. */
	FFlockRequestHandle Disband(int32 SignInNumber, const FString& PartyId, FOnParty OnComplete);

private:
	FString Url(const FString& Path) const { return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Path); }

	/** Sends one enveloped party request as the given sign-in, under the retry policy. */
	FFlockRequestHandle SendAs(int32 SignInNumber, const FString& Context, bool bIdempotent,
		TFunction<FFlockRequestHandle(const TMap<FString, FString>& Headers, FOnParty OnResult)> Send, FOnParty OnComplete,
		TFunction<bool(const FFlockError&)> IsExpectedFailure = nullptr);

	static FString ToJson(const TSharedRef<FJsonObject>& Object);

	TSharedRef<FFlockAuthSession> Session;
	FString VersionedApiUrl;
};
