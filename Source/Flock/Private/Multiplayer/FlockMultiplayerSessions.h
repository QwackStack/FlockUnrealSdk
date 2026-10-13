// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "FlockLogger.h"
#include "Http/FlockRequestsInTurn.h"
#include "Models/FlockMultiplayerModels.h"
#include "Multiplayer/FlockMultiplayerSession.h"
#include "Multiplayer/FlockMultiplayerSessionRequests.h"
#include "Multiplayer/FlockRepeatingCalls.h"
#include "UObject/StrongObjectPtr.h"

/** One reading of a session, numbered when it was sent; bFound false means "my session" found the player seated nowhere. */
struct FFlockNumberedSessionReading
{
	int32 Number = 0;
	int32 SignInNumber = 0;
	bool bFound = false;
	FFlockMultiplayerSessionReading Session;
};

/**
 * The one owner of the player's multiplayer session: at most one held per sign-in, changed only by the newest answer. Every
 * session route answers the whole session, so each answer is a reading; reads are numbered when sent, a finished change
 * settles the reads before it, and a reading with a lower host or connection epoch than the held one changes nothing (both
 * only grow). The held session is kept seated by heartbeats whose answers are readings too. Game-thread only.
 */
class FFlockMultiplayerSessions : public TSharedFromThis<FFlockMultiplayerSessions>
{
public:
	static constexpr const TCHAR* HeartbeatCallName = TEXT("Session heartbeat");
	/** A read overtaken this many times in a row means the session is changing faster than it can be read. */
	static constexpr int32 MostReadsWhileTheSessionChanges = 5;

	FFlockMultiplayerSessions(const TSharedRef<FFlockMultiplayerSessionRequests>& InRequests,
		const TSharedRef<FFlockRepeatingCalls>& InRepeatingCalls, const TSharedRef<FFlockAuthSession>& InSession,
		const TSharedRef<IFlockLogger>& InLogger);

	FFlockRequestHandle Host(TOptional<int32> MaxPlayers, const FFlockCommandData& Data, bool bSameVersionOnly, FFlockOnMultiplayerSession OnComplete);
	FFlockRequestHandle Join(const FString& JoinCode, FFlockOnMultiplayerSession OnComplete);
	/** A session the player holds or once held a seat in, read now; one the player is no longer seated in comes back ended. */
	FFlockRequestHandle Get(const FString& SessionId, FFlockOnMultiplayerSession OnComplete);
	/** The session the player is seated in now, found without its id, or null when seated nowhere. */
	FFlockRequestHandle GetMine(FFlockOnMultiplayerSession OnComplete);

	FFlockRequestHandle Leave(UFlockMultiplayerSession* Session, FFlockOnMultiplayerSession OnComplete);
	FFlockRequestHandle End(UFlockMultiplayerSession* Session, FFlockOnMultiplayerSession OnComplete);
	FFlockRequestHandle MakeHost(UFlockMultiplayerSession* Session, const FString& PlayerId, FFlockOnMultiplayerSession OnComplete);
	FFlockRequestHandle PublishConnection(UFlockMultiplayerSession* Session, const FString& Mode, const FFlockCommandData& Values,
		FFlockOnMultiplayerSession OnComplete);
	FFlockRequestHandle RequestJoinToken(UFlockMultiplayerSession* Session, TFunction<void(TFlockResult<FFlockMultiplayerSessionJoinToken>)> OnComplete);
	FFlockRequestHandle VerifyJoinToken(UFlockMultiplayerSession* Session, const FString& Token, TFunction<void(TFlockResult<FString>)> OnComplete);

	/** Ends the session of a sign-in that has ended; run once a frame. */
	void EndIfSignInEnded();

	/**
	 * The game is ending: gives the seat up without waiting, so the others see it at once rather than when the server stops
	 * hearing from it. The session ends as left, raising nothing. True when a leave was sent.
	 */
	bool LeaveForQuit();

	/** Flock is shutting down: the session ends without a word to the game, and an answer still on its way holds nothing. */
	void StopForShutdown();

	int32 CurrentSignInNumber() const { return AuthSession->GetSignInNumber(); }
	UFlockMultiplayerSession* GetHeld() const { return Held.Get(); }
	const TSharedPtr<FFlockRepeatingCall>& GetHeartbeatForTesting() const { return Heartbeat; }

private:
	/** A change keeps the session or ends it (end, the host leaving alone): its answer is the session as it now is. */
	FFlockRequestHandle Change(UFlockMultiplayerSession* Session,
		TFunction<FFlockRequestHandle(FFlockMultiplayerSessionRequests::FOnSession)> Send, FFlockOnMultiplayerSession OnComplete);

	/** Sends a call about a held session; one the server no longer has the player in is over here too, read once to learn why. */
	template <typename T>
	void Ask(UFlockMultiplayerSession* Session, const TSharedRef<FFlockRequestsInTurn>& InTurn,
		TFunction<FFlockRequestHandle(TFunction<void(TFlockResult<T>)>)> Send, TFunction<void(TFlockResult<T>)> OnAnswer);

	/** The reading says why the session is over; one overtaken by a newer answer leaves it to that answer. Then runs either way. */
	void LearnWhyItEnded(UFlockMultiplayerSession* Session, const TSharedPtr<FFlockRequestsInTurn>& InTurn, TFunction<void()> Then);

	/** A reading taken over by a newer one, or by a change finishing, is read again so the caller gets the server's latest. */
	void ReadUntilNewest(int32 SignInNumber, const FString& PlayerId, const FString& SessionId, const TSharedRef<FFlockRequestsInTurn>& InTurn,
		int32 Attempt, FFlockOnMultiplayerSession OnComplete);

	/** Numbered as it is sent: the number, not the order answers arrive in, says which reading is newer. An empty id reads "my session". */
	FFlockRequestHandle ReadNumbered(int32 SignInNumber, const FString& SessionId, TFunction<void(TFlockResult<FFlockNumberedSessionReading>)> OnRead);

	/** The answer to a change is the newest reading: reads this sign-in sent before the change finished change nothing. */
	UFlockMultiplayerSession* TakeAnswerOfChange(const FFlockMultiplayerSessionReading& Reading, int32 SignInNumber, const FString& PlayerId);

	/** The one place a reading changes the session; holds it when a call asked for it (PlayerId given) and the player is seated. */
	UFlockMultiplayerSession* Take(const FFlockMultiplayerSessionReading& Reading, int32 SignInNumber, const FString& PlayerId);

	/** Settles everything before raising the end, so a handler that calls back in finds the session over. */
	void MarkEnded(UFlockMultiplayerSession* Session, const FString& Reason, bool bRaise = true);

	/** Keeps the held session seated at the interval its answers give; a new interval, or a call a failure stopped, starts them again. */
	void KeepHeartbeating();
	void StopHeartbeating();
	/** A heartbeat's answer is a reading like any other: numbered when sent, so an older one changes nothing. */
	FFlockRequestHandle HeartbeatNumbered(int32 SignInNumber, const FString& SessionId, TFunction<void(TFlockResult<FFlockNumberedSessionReading>)> OnRead);
	void TakeHeartbeat(const FFlockNumberedSessionReading& Read);
	/** A seat the server no longer has ends the session, read once to learn why; any other refusal leaves the seat to time out. */
	void HeartbeatStopped(const FString& SessionId, const FFlockError& Failure);

	/** The leave of this session has its answer; a leave of another session started since stays remembered. */
	void ForgetLeaveOf(const FString& SessionId);

	/** Why a reading says the session is no longer the player's, or empty while they hold a seat in it. */
	static FString EndingIn(const FFlockMultiplayerSessionReading& Reading, const FString& PlayerId);
	static bool IsOutOfTheSession(const FFlockError& Failure);
	static FFlockError ShutDownError();
	/** False, with a Validation error naming the input, when the value is empty. */
	static bool IsGiven(const FString& Value, const TCHAR* Name, FFlockError& OutError);

	bool RequireSignedIn(int32& OutSignInNumber, FString& OutPlayerId, FFlockError& OutError) const;
	/** The sign-in a call was asked under ended, or Flock shut down, while its answer was on its way. */
	bool IsStillCurrent(int32 SignInNumber) const { return !bStopped && SignInNumber == CurrentSignInNumber(); }
	static bool IsRunning(const UFlockMultiplayerSession* Session, FFlockError& OutError);

	TSharedRef<FFlockMultiplayerSessionRequests> Requests;
	TSharedRef<FFlockRepeatingCalls> RepeatingCalls;
	TSharedRef<FFlockAuthSession> AuthSession;
	TSharedRef<IFlockLogger> Logger;
	/** Kept alive by this while it is the player's. */
	TStrongObjectPtr<UFlockMultiplayerSession> Held;
	TSharedPtr<FFlockRepeatingCall> Heartbeat;
	int32 ReadsSent = 0;
	/** Reads numbered at or below this change nothing when they land. */
	int32 ReadsSettledThrough = 0;
	/** The session this game's leave is on its way for, or empty: a reading that finds the player out of it is that leave's doing. */
	FString LeavingSessionId;
	/** Flock shut down: an answer still on its way holds nothing. */
	bool bStopped = false;
};
