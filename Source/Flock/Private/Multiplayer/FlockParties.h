// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "FlockLogger.h"
#include "Http/FlockRequestsInTurn.h"
#include "Models/FlockMultiplayerModels.h"
#include "Multiplayer/FlockParty.h"
#include "Multiplayer/FlockPartyRequests.h"
#include "Multiplayer/FlockRepeatingCalls.h"
#include "UObject/StrongObjectPtr.h"

/** One reading of the player's party, numbered when it was sent; bInParty false means the player is in none. */
struct FFlockPartyRead
{
	int32 Number = 0;
	int32 SignInNumber = 0;
	bool bInParty = false;
	FFlockPartyReading Party;
};

/**
 * The one owner of the player's party: at most one held per sign-in, changed only by the newest reading. Reads are
 * numbered when sent; one older than the newest applied reading, or than a change this game finished, changes nothing.
 * Game-thread only.
 */
class FFlockParties : public TSharedFromThis<FFlockParties>
{
public:
	static constexpr const TCHAR* RefreshCallName = TEXT("Party refresh");
	/** A read overtaken this many times in a row means the party is changing faster than it can be read. */
	static constexpr int32 MostReadsWhileThePartyChanges = 5;

	FFlockParties(const TSharedRef<FFlockPartyRequests>& InRequests, const TSharedRef<FFlockRepeatingCalls>& InRepeatingCalls,
		const TSharedRef<FFlockAuthSession>& InSession, const TSharedRef<IFlockLogger>& InLogger, double InRefreshIntervalSeconds);

	FFlockRequestHandle Create(TOptional<int32> MaxSize, const FFlockCommandData& Settings, FFlockOnParty OnComplete);
	FFlockRequestHandle Join(const FString& InviteCode, FFlockOnParty OnComplete);
	/** The player's party, read now, or null when the player is in none. */
	FFlockRequestHandle GetMine(FFlockOnParty OnComplete);

	FFlockRequestHandle Leave(UFlockParty* Party, FFlockOnParty OnComplete);
	FFlockRequestHandle Kick(UFlockParty* Party, const FString& PlayerId, FFlockOnParty OnComplete);
	FFlockRequestHandle MakeLeader(UFlockParty* Party, const FString& PlayerId, FFlockOnParty OnComplete);
	FFlockRequestHandle Update(UFlockParty* Party, TOptional<int32> MaxSize, const TOptional<FFlockCommandData>& Settings,
		FFlockOnParty OnComplete);
	FFlockRequestHandle Disband(UFlockParty* Party, FFlockOnParty OnComplete);

	/** Ends the party of a sign-in that has ended; run once a frame. */
	void EndIfSignInEnded();

	/** Flock is shutting down: the party ends without a word to the game, and an answer still on its way holds nothing. */
	void StopForShutdown();

	int32 CurrentSignInNumber() const { return Session->GetSignInNumber(); }
	UFlockParty* GetHeld() const { return Held.Get(); }
	double GetRefreshIntervalSeconds() const { return RefreshIntervalSeconds; }
	const TSharedPtr<FFlockRepeatingCall>& GetRefreshForTesting() const { return Refresh; }

private:
	/** A change keeps the party: once the server has it, the party is read again, so its events come from one place. */
	FFlockRequestHandle Change(UFlockParty* Party, TFunction<FFlockRequestHandle(FFlockPartyRequests::FOnParty)> Send,
		FFlockOnParty OnComplete);

	/** A failed change to a party the server no longer has for this player ends it; a finished one settles the reads before it. */
	bool TakeChangeAnswer(UFlockParty* Party, int32 SignInNumber, const TFlockResult<FFlockPartyReading>& Answer,
		const FFlockOnParty& OnComplete);

	/** A reading taken over by a newer one, or by a change finishing, is read again so the caller gets the server's latest. */
	void ReadUntilNewest(int32 SignInNumber, const FString& PartyId, const TSharedRef<FFlockRequestsInTurn>& Requests,
		int32 Attempt, TFunction<void(TFlockResult<FFlockPartyRead>)> OnRead);

	/** Numbered as it is sent: the number, not the order answers arrive in, says which reading is newer. */
	FFlockRequestHandle ReadNumbered(int32 SignInNumber, const FString& PartyId, TFunction<void(TFlockResult<FFlockPartyRead>)> OnRead);

	/** The one place a reading changes the party; holds the party when a call asked for it (PlayerId given), else returns the held one or null. */
	UFlockParty* Apply(const FFlockPartyRead& Read, const FString& PlayerId);

	UFlockParty* Hold(int32 SignInNumber, const FString& PlayerId, const FFlockPartyReading& Reading, const TArray<FString>& MemberIds);

	/** A finished write is newer than every read sent before it; one answered for a sign-in that ended settles nothing. */
	void SettleReadsSentSoFar(int32 SignInNumber);

	/** Settles everything before raising the end, so a handler that calls back in finds the party over. */
	void End(UFlockParty* Party, EFlockPartyEndReason Reason, bool bRaise = true);

	/** Also restarts a refresh a failure stopped, whenever the game asks for its party again. */
	void KeepRefreshing();
	void StopRefreshing();

	static void Answer(const FFlockOnParty& OnComplete, const TFlockResult<UFlockParty*>& Result);
	static FFlockError ShutDownError();
	static FString EndReasonName(EFlockPartyEndReason Reason);

	bool RequireSignedIn(int32& OutSignInNumber, FString& OutPlayerId, const FFlockOnParty& OnComplete) const;
	/** The sign-in a call was asked under ended, or Flock shut down, while its answer was on its way. */
	bool StillCurrent(int32 SignInNumber, const FFlockOnParty& OnComplete) const;
	static bool RequireRunning(const UFlockParty* Party, const FFlockOnParty& OnComplete);

	TSharedRef<FFlockPartyRequests> Requests;
	TSharedRef<FFlockRepeatingCalls> RepeatingCalls;
	TSharedRef<FFlockAuthSession> Session;
	TSharedRef<IFlockLogger> Logger;
	double RefreshIntervalSeconds = 0.0;
	/** Kept alive by this while it is the player's. */
	TStrongObjectPtr<UFlockParty> Held;
	TSharedPtr<FFlockRepeatingCall> Refresh;
	int32 ReadsSent = 0;
	/** Reads numbered at or below this change nothing when they land. */
	int32 ReadsSettledThrough = 0;
	/** A leave or disband of the held party on its way: a reading that finds the player out is its doing, not a removal. */
	EFlockPartyEndReason EndingByThisGame = EFlockPartyEndReason::None;
	/** Flock shut down: an answer still on its way holds nothing. */
	bool bStopped = false;
};
