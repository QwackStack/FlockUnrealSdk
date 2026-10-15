// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "FlockLogger.h"
#include "Http/FlockRequestsInTurn.h"
#include "Models/FlockMultiplayerModels.h"
#include "Multiplayer/FlockMatchmakingQueueNames.h"
#include "Multiplayer/FlockMatchmakingRequests.h"
#include "Multiplayer/FlockParty.h"
#include "Multiplayer/FlockRepeatingCalls.h"

class FFlockMultiplayerSessions;
class UFlockMultiplayerSession;

/**
 * One reading of the player's own ticket, numbered when it was sent. bRead is false when it could not be read this time;
 * bHasTicket is false when the player never searched.
 */
struct FFlockMatchmakingTicketRead
{
	int32 Number = 0;
	bool bRead = false;
	bool bHasTicket = false;
	FFlockMatchmakingTicketReading Ticket;
};

/**
 * The one owner of the player's search for a match: at most one per sign-in, its ticket checked until it leaves the queue, and
 * a party's search followed for its members through the party's refresh. A match hands over the session it seats the player in,
 * read through the sessions owner and held like any other. Game-thread only.
 */
class FFlockMatchmaking : public TSharedFromThis<FFlockMatchmaking>
{
public:
	static constexpr const TCHAR* CheckCallName = TEXT("Matchmaking check");
	/** The server asks for a check every 2 to 5 s; the repeating calls move each wait a quarter either way. */
	static constexpr double DefaultCheckIntervalSeconds = 3.0;

	FFlockMatchmaking(const TSharedRef<FFlockMatchmakingRequests>& InRequests, const TSharedRef<FFlockRepeatingCalls>& InRepeatingCalls,
		const TSharedRef<FFlockMultiplayerSessions>& InSessions, const TSharedRef<FFlockAuthSession>& InSession,
		const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockMatchmakingQueueNames>& InQueueNames);

	/** Searches alone. Cancelling the handle cancels the search on the server and completes the call as Cancelled. */
	FFlockRequestHandle FindAlone(const FString& QueueName, const FFlockMatchmakingOptions& Options, FFlockOnMatchmakingResult OnComplete);

	/**
	 * The leader's game starts the party's search; a member's game waits for the leader's. A search of the party already
	 * running here is waited for.
	 */
	FFlockRequestHandle FindForParty(UFlockParty* Party, const FString& QueueName, const FFlockMatchmakingOptions& Options,
		FFlockOnMatchmakingResult OnComplete);

	/** True while a search of this party is known here and has not ended. */
	bool IsSearching(const UFlockParty* Party) const;

	/**
	 * The player's own ticket, read with each party refresh so a member's game learns of the leader's search. A read that
	 * failed answers a reading not read this time, logged at debug, so the refresh goes on.
	 */
	FFlockRequestHandle ReadForParty(int32 SignInNumber, TFunction<void(const FFlockMatchmakingTicketRead&)> OnRead);

	/** Takes the ticket read with a party refresh, after the party's own reading and before the party's events are raised. */
	void TakePartyReading(UFlockParty* Party, const FFlockMatchmakingTicketRead& Reading);

	/**
	 * Marks where the party's searches stand the moment the game holds the party, so a search made after it is never taken
	 * for an old one. The read goes through InTurn, and Then runs however it ends: a read the game gave up marks nothing, and
	 * the party's next refresh marks the start instead.
	 */
	void StartWatching(UFlockParty* Party, const TSharedRef<FFlockRequestsInTurn>& InTurn, TFunction<void()> Then);

	/** Ends the search of a sign-in that has ended, or the one being made for it, as Cancelled; run once a frame. Nothing more is sent for it. */
	void EndIfSignInEnded();

	/**
	 * The game is quitting: the search is cancelled without waiting, so it seats nobody who has gone, and the call waiting for it
	 * completes as Cancelled. A search still being made is cancelled once its ticket lands, and a match already landing gives its
	 * seat up, while Flock waits for the requests on their way.
	 */
	void CancelForQuit();

	/** Flock is shutting down: the search ends with Cancelled and no event, and an answer still on its way starts nothing. */
	void StopForShutdown();

	int32 CurrentSignInNumber() const { return Session->GetSignInNumber(); }

	/** A game always checks at the default; tests shorten it. */
	void SetCheckIntervalForTesting(double Seconds) { CheckIntervalSeconds = Seconds; }

	/** True while a search is held here (its own, or the party's being followed); for tests. */
	bool HasSearchForTesting() const { return Search.IsValid(); }

private:
	struct FSearch;
	struct FStart;
	struct FCall;

	/** Makes the call's ticket: the queue by name, then the ticket, then the search. */
	FFlockRequestHandle Start(int32 SignInNumber, const FString& PlayerId, UFlockParty* Party, const FString& QueueName,
		const FFlockMatchmakingOptions& Options, FFlockOnMatchmakingResult OnComplete);
	/** Finds the queue's id by its name, then makes the ticket. */
	void LookUpQueue(const TSharedRef<FStart>& Starting);
	void MakeTicket(const TSharedRef<FStart>& Starting, const FString& QueueId, bool bSecondTry);
	void TakeMade(const TSharedRef<FStart>& Starting, const FFlockMatchmakingTicketsMade& Made);
	/** The player's own earlier search (an earlier launch's, or this party's when its leader searches again) is cancelled and this one made once more. */
	void ReplaceEarlierSearch(const TSharedRef<FStart>& Starting, const FString& QueueId, const FFlockError& Refusal);
	/** The start is over, answered with Result; one the game gave up was answered when it did. */
	void FinishStart(const TSharedRef<FStart>& Starting, const TFlockResult<FFlockMatchmakingResult>& Result);
	void Begin(const TSharedRef<FStart>& Starting, const FFlockMatchmakingTicketReading& Ticket);

	/** A member's game reads the player's own ticket until the party's search shows up. */
	TSharedRef<FSearch> WaitForLeader(int32 SignInNumber, UFlockParty* Party);
	/** The call waits for the search's result; refused when another call already does. */
	void Wait(const TSharedRef<FSearch>& Waited, const TSharedRef<FCall>& Call);
	TSharedRef<FCall> MakeCall(FFlockOnMatchmakingResult OnComplete);
	/** The game cancelled the call's handle. */
	void GiveUp(const TSharedRef<FCall>& Call);
	/** The start is given up: the call hears Why now, and a ticket still on its way is cancelled once it lands. */
	void GiveUpStart(const TSharedRef<FStart>& Starting, const FFlockError& Why);
	/** A member's game waiting for the leader stops waiting once nobody else will start the party's search for it. */
	void StopWaiting(const TSharedRef<FSearch>& Waiting, const UFlockParty* Party);
	/** The game gave up: the search is cancelled on the server, and a match that landed meanwhile gives its seat up. */
	void CancelSearch(const TSharedRef<FSearch>& Cancelled);
	/** Cancels a search; one matched meanwhile gives up the seat the match gave, so a search the game gave up leaves nothing behind. */
	void CancelOrLeave(int32 SignInNumber, const FString& TicketId, TFunction<void(TFlockResult<bool>)> Then);
	void LeaveMatched(const FFlockMatchmakingTicketReading& Ended, TFunction<void(TFlockResult<bool>)> Then);
	void LeaveQuietly(UFlockMultiplayerSession* Matched, TFunction<void()> Then);

	void Check(const TSharedRef<FSearch>& Checked);
	static void StopChecking(const TSharedRef<FSearch>& Checked);
	/** A check the server refused for good ends the search with that failure; the party's next reading follows its search again. */
	void CheckStopped(const TSharedRef<FSearch>& Checked, const FFlockError& Failure);
	/** Numbered as it is sent: the number, not the order answers arrive in, says which reading is newer. */
	FFlockRequestHandle ReadNumbered(int32 SignInNumber, const FString& TicketId, TFunction<void(TFlockResult<FFlockMatchmakingTicketRead>)> OnRead);
	/** Takes a reading newer than every one taken so far, and says whether it did. */
	bool TakeIfNewest(const FFlockMatchmakingTicketRead& Reading);
	void TakeCheck(const TSharedRef<FSearch>& Checked, const FFlockMatchmakingTicketRead& Reading);
	/** The one place a search's ticket moves it on: a ticket that left the queue ends it. */
	void TakeTicket(const TSharedRef<FSearch>& Moved, const FFlockMatchmakingTicketReading& Ticket);
	/** The player's own ticket read without its id, by a party refresh or a member's game waiting for the leader. */
	void TakeCurrent(UFlockParty* Party, const FFlockMatchmakingTicketRead& Reading);
	bool IsWatching(const UFlockParty* Party) const;
	/** Marks where the party's searches stand; true when one is already running, which is left unseen so it is followed and told started. */
	bool BeginWatch(UFlockParty* Party, const FFlockMatchmakingTicketRead& Reading);
	void SettlePartyTicket(const FSearch& Settled);

	/** A ticket that left the queue: a match hands over the session it seats the player in, read before anyone hears of it. */
	void Finish(const TSharedRef<FSearch>& Finished, const FFlockMatchmakingTicketReading& Ticket);
	void MatchedSessionOrNone(const FFlockMatchmakingTicketReading& Ticket, TFunction<void(UFlockMultiplayerSession*)> Then);
	/** The party is told first, then the call: a game's completion can start the next search, which is told after this one's end. */
	void Complete(const TSharedRef<FSearch>& Completed, const FFlockMatchmakingResult& Result);
	void AnswerCaller(const TSharedRef<FSearch>& Answered, const TFlockResult<FFlockMatchmakingResult>& Result);
	static FString OutcomeOf(const FFlockMatchmakingTicketReading& Ticket);

	bool RequireSignedIn(int32& OutSignInNumber, FString& OutPlayerId, FFlockError& OutError) const;
	static bool RequireQueueName(const FString& QueueName, FFlockError& OutError);
	static FFlockError PartyEndedError(const UFlockParty* Party);
	bool RequireNoSearch(FFlockError& OutError) const;
	/** The sign-in a call was asked under ended, or Flock shut down, while its answer was on its way. */
	bool StillCurrent(int32 SignInNumber) const { return !bStopped && SignInNumber == CurrentSignInNumber(); }
	static FFlockError CancelledError();
	static FFlockError ShutDownError();
	static FFlockError SignInEndedError();
	static void Answer(const FFlockOnMatchmakingResult& OnComplete, const TFlockResult<FFlockMatchmakingResult>& Result);

	TSharedRef<FFlockMatchmakingRequests> Requests;
	TSharedRef<FFlockRepeatingCalls> RepeatingCalls;
	TSharedRef<FFlockMultiplayerSessions> Sessions;
	TSharedRef<FFlockAuthSession> Session;
	TSharedRef<IFlockLogger> Logger;
	/** Shared with the provider's FindQueueId, so one read of the queue list serves everyone asking. */
	TSharedRef<FFlockMatchmakingQueueNames> QueueNames;
	double CheckIntervalSeconds = DefaultCheckIntervalSeconds;
	TSharedPtr<FSearch> Search;
	/** A search's first request is on its way, so a second one is refused until it lands. */
	TSharedPtr<FStart> Starting;
	int32 ReadsSent = 0;
	/** Readings numbered at or below this change nothing when they land. */
	int32 ReadsAppliedThrough = 0;
	/**
	 * The party whose searches are watched, its latest ticket seen, and whether that ticket needs nothing more: a party's
	 * search is reported once, and one from before the party was first read here never is.
	 */
	FString PartyWatched;
	int32 PartyWatchedSignInNumber = 0;
	FString PartyTicketSeen;
	bool bPartyTicketSettled = false;
	/** Flock shut down: an answer still on its way starts nothing. */
	bool bStopped = false;
};
