// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockMatchmaking.h"
#include "Http/FlockErrorHints.h"
#include "Multiplayer/FlockMultiplayerSession.h"
#include "Multiplayer/FlockMultiplayerSessions.h"

/** One FindMatch call: what it waits for, and how it is answered, once. */
struct FFlockMatchmaking::FCall
{
	FFlockOnMatchmakingResult OnComplete;
	TSharedRef<FFlockCancelToken> Token = MakeShared<FFlockCancelToken>();
	bool bAnswered = false;
	/** Set while the call's own search is being made. */
	TSharedPtr<FStart> Starting;
	/** Set once the call waits for a search. */
	TSharedPtr<FSearch> Waited;

	void Answer(const TFlockResult<FFlockMatchmakingResult>& Result)
	{
		if (bAnswered)
		{
			return;
		}
		bAnswered = true;
		if (OnComplete)
		{
			OnComplete(Result);
		}
	}
};

/** A search being made: the queue looked up, the ticket on its way. */
struct FFlockMatchmaking::FStart
{
	int32 SignInNumber = 0;
	FString PlayerId;
	TWeakObjectPtr<UFlockParty> Party;
	bool bForParty = false;
	FString PartyId;
	FString QueueName;
	FFlockMatchmakingOptions Options;
	TSharedPtr<FCall> Call;
	/** The game gave up before the ticket landed: a ticket that lands is cancelled, and nothing replaces an earlier search. */
	bool bGivenUp = false;
	/** The server no longer knew the queue's id, so the list was read again once. */
	bool bQueueReadAgain = false;
	FFlockRequestHandle QueueLookup;
};

/** A search held here: this player's own, or the party's that this game follows. */
struct FFlockMatchmaking::FSearch
{
	int32 SignInNumber = 0;
	/** The party whose search this is; unset for a search alone. */
	TWeakObjectPtr<UFlockParty> Party;
	bool bForParty = false;
	FString PartyId;
	/** Empty while a member's game waits for the leader to start. */
	FString TicketId;
	TSharedPtr<FFlockRepeatingCall> Check;
	/** The game's call waiting for the result. */
	TSharedPtr<FCall> Caller;
	/** The result, once the search has ended here. */
	TOptional<TFlockResult<FFlockMatchmakingResult>> Finished;
	/** The ticket left the queue and the result is being worked out; no other reading counts. */
	bool bEnding = false;
	/** The game gave up on it, and its cancel settles the search. */
	bool bCancelledByThisGame = false;
	/** The game gave up while the search was ending: a seat the match gave is let go, and the call hears a cancellation. */
	bool bCallerGaveUpWhileEnding = false;

	UFlockParty* GetParty() const { return Party.Get(); }
	bool IsOfParty(const UFlockParty* Other) const { return bForParty && Other != nullptr && Party.Get() == Other; }
};

FFlockMatchmaking::FFlockMatchmaking(const TSharedRef<FFlockMatchmakingRequests>& InRequests, const TSharedRef<FFlockRepeatingCalls>& InRepeatingCalls,
	const TSharedRef<FFlockMultiplayerSessions>& InSessions, const TSharedRef<FFlockAuthSession>& InSession,
	const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockMatchmakingQueueNames>& InQueueNames)
	: Requests(InRequests)
	, RepeatingCalls(InRepeatingCalls)
	, Sessions(InSessions)
	, Session(InSession)
	, Logger(InLogger)
	, QueueNames(InQueueNames)
{
}

// ---- the calls ----

FFlockRequestHandle FFlockMatchmaking::FindAlone(const FString& QueueName, const FFlockMatchmakingOptions& Options, FFlockOnMatchmakingResult OnComplete)
{
	int32 SignInNumber = 0;
	FString PlayerId;
	FFlockError Refused;
	if (!RequireSignedIn(SignInNumber, PlayerId, Refused) || !RequireNoSearch(Refused))
	{
		Answer(OnComplete, TFlockResult<FFlockMatchmakingResult>::Fail(Refused));
		return FFlockRequestHandle();
	}
	if (!RequireQueueName(QueueName, Refused))
	{
		Answer(OnComplete, TFlockResult<FFlockMatchmakingResult>::Fail(Refused));
		return FFlockRequestHandle();
	}
	return Start(SignInNumber, PlayerId, nullptr, QueueName, Options, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMatchmaking::FindForParty(UFlockParty* Party, const FString& QueueName, const FFlockMatchmakingOptions& Options,
	FFlockOnMatchmakingResult OnComplete)
{
	if (Party == nullptr)
	{
		Answer(OnComplete, TFlockResult<FFlockMatchmakingResult>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation, TEXT("No party was given"), FFlockErrorHints::PassAHeldParty())));
		return FFlockRequestHandle();
	}
	if (Party->HasEnded())
	{
		Answer(OnComplete, TFlockResult<FFlockMatchmakingResult>::Fail(PartyEndedError(Party)));
		return FFlockRequestHandle();
	}
	int32 SignInNumber = 0;
	FString PlayerId;
	FFlockError Refused;
	if (!RequireSignedIn(SignInNumber, PlayerId, Refused))
	{
		Answer(OnComplete, TFlockResult<FFlockMatchmakingResult>::Fail(Refused));
		return FFlockRequestHandle();
	}
	if (Search.IsValid() && Search->IsOfParty(Party))
	{
		const TSharedRef<FCall> Call = MakeCall(MoveTemp(OnComplete));
		Wait(Search.ToSharedRef(), Call);
		return FFlockRequestHandle(Call->Token);
	}
	if (!RequireNoSearch(Refused))
	{
		Answer(OnComplete, TFlockResult<FFlockMatchmakingResult>::Fail(Refused));
		return FFlockRequestHandle();
	}
	if (Party->IsLeader())
	{
		if (!RequireQueueName(QueueName, Refused))
		{
			Answer(OnComplete, TFlockResult<FFlockMatchmakingResult>::Fail(Refused));
			return FFlockRequestHandle();
		}
		return Start(SignInNumber, PlayerId, Party, QueueName, Options, MoveTemp(OnComplete));
	}
	const TSharedRef<FSearch> Waiting = WaitForLeader(SignInNumber, Party);
	const TSharedRef<FCall> Call = MakeCall(MoveTemp(OnComplete));
	Wait(Waiting, Call);
	return FFlockRequestHandle(Call->Token);
}

bool FFlockMatchmaking::IsSearching(const UFlockParty* Party) const
{
	return Search.IsValid() && Search->IsOfParty(Party) && !Search->TicketId.IsEmpty() && !Search->bEnding;
}

// ---- starting a search ----

FFlockRequestHandle FFlockMatchmaking::Start(int32 SignInNumber, const FString& PlayerId, UFlockParty* Party, const FString& QueueName,
	const FFlockMatchmakingOptions& Options, FFlockOnMatchmakingResult OnComplete)
{
	const TSharedRef<FCall> Call = MakeCall(MoveTemp(OnComplete));
	const TSharedRef<FStart> Made = MakeShared<FStart>();
	Made->SignInNumber = SignInNumber;
	Made->PlayerId = PlayerId;
	Made->Party = Party;
	Made->bForParty = Party != nullptr;
	Made->PartyId = Party != nullptr ? Party->GetId() : FString();
	Made->QueueName = QueueName;
	Made->Options = Options;
	Made->Call = Call;
	Call->Starting = Made;
	Starting = Made;
	LookUpQueue(Made);
	return FFlockRequestHandle(Call->Token);
}

void FFlockMatchmaking::LookUpQueue(const TSharedRef<FStart>& Made)
{
	const TWeakPtr<FFlockMatchmaking> WeakSelf = AsShared();
	Made->QueueLookup = QueueNames->FindId(Made->QueueName, [WeakSelf, Made](TFlockResult<FString> Found)
	{
		const TSharedPtr<FFlockMatchmaking> Self = WeakSelf.Pin();
		if (!Self.IsValid() || Made->bGivenUp)
		{
			return;
		}
		if (!Found.bSuccess)
		{
			Self->FinishStart(Made, TFlockResult<FFlockMatchmakingResult>::Fail(Found.Error));
			return;
		}
		if (!Self->StillCurrent(Made->SignInNumber))
		{
			Self->FinishStart(Made, TFlockResult<FFlockMatchmakingResult>::Fail(SignInEndedError()));
			return;
		}
		Self->MakeTicket(Made, Found.Value, /*bSecondTry*/ false);
	});
}

void FFlockMatchmaking::MakeTicket(const TSharedRef<FStart>& Making, const FString& QueueId, bool bSecondTry)
{
	const TWeakPtr<FFlockMatchmaking> WeakSelf = AsShared();
	Requests->Create(Making->SignInNumber, QueueId, Making->PartyId, Making->Options.Attributes,
		[WeakSelf, Making, QueueId, bSecondTry](TFlockResult<FFlockMatchmakingTicketsMade> Made)
		{
			const TSharedPtr<FFlockMatchmaking> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (Made.bSuccess)
			{
				Self->TakeMade(Making, Made.Value);
				return;
			}
			// A search the game gave up never replaces one: the search queued may be the game's next.
			if (Made.Error.ErrorCode == EFlockErrorCode::MatchmakingAlreadyQueued && !bSecondTry && !Making->bGivenUp)
			{
				Self->ReplaceEarlierSearch(Making, QueueId, Made.Error);
				return;
			}
			// The queue list kept for the launch named an id the server no longer knows: the queue was deleted, or made again
			// under the same name. The list is read again, once, and the name looked up in it.
			if (Made.Error.ErrorCode == EFlockErrorCode::MatchmakingQueueNotFound && !Making->bQueueReadAgain && !Making->bGivenUp
				&& Self->StillCurrent(Making->SignInNumber))
			{
				Making->bQueueReadAgain = true;
				Self->QueueNames->ForgetQueue(QueueId);
				Self->LookUpQueue(Making);
				return;
			}
			Self->FinishStart(Making, TFlockResult<FFlockMatchmakingResult>::Fail(Made.Error));
		});
}

void FFlockMatchmaking::TakeMade(const TSharedRef<FStart>& Making, const FFlockMatchmakingTicketsMade& Made)
{
	// A party's call answers every member's ticket; the player's own is the one to check.
	const FFlockMatchmakingTicketReading* Own = Made.FindTicketOf(Making->PlayerId);
	if (Own == nullptr)
	{
		FinishStart(Making, TFlockResult<FFlockMatchmakingResult>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Network,
			TEXT("The server's answer names no search for this player"), TEXT("The answer does not match what this SDK sends for: keep the Flock SDK and the server on matching versions, then call FindMatch again."))));
		return;
	}
	if (Making->bGivenUp)
	{
		// The game gave up while the ticket was on its way: it is cancelled now that it has landed, and nobody waits for that.
		CancelOrLeave(Making->SignInNumber, Own->Id, [](TFlockResult<bool>) {});
		return;
	}
	if (!StillCurrent(Making->SignInNumber))
	{
		FinishStart(Making, TFlockResult<FFlockMatchmakingResult>::Fail(SignInEndedError()));
		return;
	}
	Begin(Making, *Own);
}

void FFlockMatchmaking::ReplaceEarlierSearch(const TSharedRef<FStart>& Making, const FString& QueueId, const FFlockError& Refusal)
{
	const TWeakPtr<FFlockMatchmaking> WeakSelf = AsShared();
	Requests->ReadCurrent(Making->SignInNumber, [WeakSelf, Making, QueueId, Refusal](TFlockResult<FFlockMatchmakingTicketReading> Current)
	{
		const TSharedPtr<FFlockMatchmaking> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}
		if (!Current.bSuccess)
		{
			const bool bNone = Current.Error.ErrorCode == EFlockErrorCode::MatchmakingTicketNotFound;
			Self->FinishStart(Making, TFlockResult<FFlockMatchmakingResult>::Fail(bNone ? Refusal : Current.Error));
			return;
		}
		// Only the player's own search alone, or this party's, is cancelled: one member's cancel stops a whole party's search.
		const bool bOurs = Current.Value.IsQueued() && (Current.Value.PartyId.IsEmpty() || Current.Value.IsForParty(Making->PartyId));
		if (!bOurs)
		{
			Self->FinishStart(Making, TFlockResult<FFlockMatchmakingResult>::Fail(Refusal));
			return;
		}
		if (Making->bGivenUp)
		{
			return;
		}
		Self->CancelOrLeave(Making->SignInNumber, Current.Value.Id, [WeakSelf, Making, QueueId](TFlockResult<bool> Done)
		{
			const TSharedPtr<FFlockMatchmaking> Again = WeakSelf.Pin();
			if (!Again.IsValid())
			{
				return;
			}
			if (!Done.bSuccess)
			{
				Again->FinishStart(Making, TFlockResult<FFlockMatchmakingResult>::Fail(Done.Error));
				return;
			}
			if (Making->bGivenUp)
			{
				return;
			}
			Again->Logger->LogDebug(TEXT("The player's search left from before was cancelled, and this one takes its place."));
			Again->MakeTicket(Making, QueueId, /*bSecondTry*/ true);
		});
	});
}

void FFlockMatchmaking::FinishStart(const TSharedRef<FStart>& Finished, const TFlockResult<FFlockMatchmakingResult>& Result)
{
	if (Starting == Finished)
	{
		Starting.Reset();
	}
	if (Finished->Call.IsValid())
	{
		Finished->Call->Starting.Reset();
		Finished->Call->Answer(Result);
	}
}

void FFlockMatchmaking::Begin(const TSharedRef<FStart>& Begun, const FFlockMatchmakingTicketReading& Ticket)
{
	if (Starting == Begun)
	{
		Starting.Reset();
	}
	// A ticket just made is newer than every reading sent before it.
	ReadsAppliedThrough = ReadsSent;
	const TSharedRef<FSearch> Made = MakeShared<FSearch>();
	Made->SignInNumber = Begun->SignInNumber;
	Made->Party = Begun->Party;
	Made->bForParty = Begun->bForParty;
	Made->PartyId = Begun->PartyId;
	Made->TicketId = Ticket.Id;
	Search = Made;
	Check(Made);
	if (Begun->bForParty)
	{
		PartyWatched = Begun->PartyId;
		PartyWatchedSignInNumber = Begun->SignInNumber;
		PartyTicketSeen = Ticket.Id;
		bPartyTicketSettled = false;
		if (UFlockParty* Party = Begun->Party.Get())
		{
			if (!Party->HasEnded())
			{
				Party->RaiseSearchStarted();
			}
		}
	}
	if (Begun->Call.IsValid())
	{
		Wait(Made, Begun->Call.ToSharedRef());
	}
}

TSharedRef<FFlockMatchmaking::FSearch> FFlockMatchmaking::WaitForLeader(int32 SignInNumber, UFlockParty* Party)
{
	const TSharedRef<FSearch> Waiting = MakeShared<FSearch>();
	Waiting->SignInNumber = SignInNumber;
	Waiting->Party = Party;
	Waiting->bForParty = true;
	Waiting->PartyId = Party->GetId();
	Search = Waiting;
	Check(Waiting);
	return Waiting;
}

// ---- the game's call ----

TSharedRef<FFlockMatchmaking::FCall> FFlockMatchmaking::MakeCall(FFlockOnMatchmakingResult OnComplete)
{
	const TSharedRef<FCall> Call = MakeShared<FCall>();
	Call->OnComplete = MoveTemp(OnComplete);
	const TWeakPtr<FFlockMatchmaking> WeakSelf = AsShared();
	const TWeakPtr<FCall> WeakCall = Call;
	Call->Token->Canceller = [WeakSelf, WeakCall]()
	{
		const TSharedPtr<FFlockMatchmaking> Self = WeakSelf.Pin();
		const TSharedPtr<FCall> Pinned = WeakCall.Pin();
		if (Self.IsValid() && Pinned.IsValid())
		{
			Self->GiveUp(Pinned.ToSharedRef());
		}
	};
	return Call;
}

void FFlockMatchmaking::Wait(const TSharedRef<FSearch>& Waited, const TSharedRef<FCall>& Call)
{
	if (Waited->Caller.IsValid())
	{
		Call->Answer(TFlockResult<FFlockMatchmakingResult>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation,
			TEXT("A call already waits for this search"), TEXT("Wait for that call's result, or for the party's OnSearchEnded, rather than calling FindMatch again."))));
		return;
	}
	Call->Starting.Reset();
	Call->Waited = Waited;
	if (Waited->Finished.IsSet())
	{
		Call->Answer(Waited->Finished.GetValue());
		return;
	}
	Waited->Caller = Call;
}

void FFlockMatchmaking::GiveUp(const TSharedRef<FCall>& Call)
{
	if (Call->bAnswered)
	{
		return;
	}
	if (Call->Waited.IsValid())
	{
		CancelSearch(Call->Waited.ToSharedRef());
	}
	else if (Call->Starting.IsValid())
	{
		GiveUpStart(Call->Starting.ToSharedRef(), CancelledError());
	}
}

void FFlockMatchmaking::GiveUpStart(const TSharedRef<FStart>& GivenUp, const FFlockError& Why)
{
	// The call hears why now; a ticket still on its way is cancelled once it lands.
	GivenUp->bGivenUp = true;
	GivenUp->QueueLookup.Cancel();
	FinishStart(GivenUp, TFlockResult<FFlockMatchmakingResult>::Fail(Why));
}

void FFlockMatchmaking::StopWaiting(const TSharedRef<FSearch>& Waiting, const UFlockParty* Party)
{
	if (Search == Waiting)
	{
		Search.Reset();
	}
	StopChecking(Waiting);
	const FFlockError Why = Party == nullptr || Party->HasEnded() ? PartyEndedError(Party)
		: FFlockError::MakeWithFix(EFlockErrorType::Validation,
			TEXT("This player leads the party now, so nobody else will start its search"), TEXT("Start it with the party's FindMatch and a queue name."));
	AnswerCaller(Waiting, TFlockResult<FFlockMatchmakingResult>::Fail(Why));
}

void FFlockMatchmaking::CancelSearch(const TSharedRef<FSearch>& Cancelled)
{
	if (Cancelled->bEnding)
	{
		// The result is on its way: a seat the match gave is let go when it lands, and the call hears a cancellation then.
		Cancelled->bCallerGaveUpWhileEnding = true;
		return;
	}
	Cancelled->bCancelledByThisGame = true;
	StopChecking(Cancelled);
	if (Search == Cancelled)
	{
		Search.Reset();
	}
	if (Cancelled->TicketId.IsEmpty())
	{
		// A member's game waiting for the leader: nothing on the server is this game's to cancel.
		AnswerCaller(Cancelled, TFlockResult<FFlockMatchmakingResult>::Fail(CancelledError()));
		return;
	}
	const TWeakPtr<FFlockMatchmaking> WeakSelf = AsShared();
	CancelOrLeave(Cancelled->SignInNumber, Cancelled->TicketId, [WeakSelf, Cancelled](TFlockResult<bool> Done)
	{
		if (const TSharedPtr<FFlockMatchmaking> Self = WeakSelf.Pin())
		{
			if (!Done.bSuccess && Done.Error.Type != EFlockErrorType::Cancelled)
			{
				Self->Logger->LogWarning(FString::Printf(TEXT("The search could not be cancelled on the server: %s. It ends on its own within 5 minutes."),
					*Done.Error.Message));
			}
			if (Cancelled->bForParty)
			{
				Self->SettlePartyTicket(*Cancelled);
				UFlockParty* Party = Cancelled->GetParty();
				if (Party != nullptr && !Party->HasEnded())
				{
					FFlockMatchmakingResult Result;
					Result.Outcome = FlockMatchmakingOutcomes::Cancelled;
					Party->RaiseSearchEnded(Result);
				}
			}
		}
		Cancelled->Finished = TFlockResult<FFlockMatchmakingResult>::Fail(CancelledError());
		if (Cancelled->Caller.IsValid())
		{
			const TSharedPtr<FCall> Call = Cancelled->Caller;
			Cancelled->Caller.Reset();
			Call->Answer(Cancelled->Finished.GetValue());
		}
	});
}

void FFlockMatchmaking::CancelOrLeave(int32 SignInNumber, const FString& TicketId, TFunction<void(TFlockResult<bool>)> Then)
{
	const TWeakPtr<FFlockMatchmaking> WeakSelf = AsShared();
	Requests->Cancel(SignInNumber, TicketId, [WeakSelf, SignInNumber, TicketId, Then](TFlockResult<FFlockMatchmakingTicketReading> Cancelled)
	{
		const TSharedPtr<FFlockMatchmaking> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			Then(TFlockResult<bool>::Fail(ShutDownError()));
			return;
		}
		if (Cancelled.bSuccess)
		{
			Self->LeaveMatched(Cancelled.Value, Then);
			return;
		}
		if (Cancelled.Error.ErrorCode != EFlockErrorCode::MatchmakingTicketNotCancelable)
		{
			Then(TFlockResult<bool>::Fail(Cancelled.Error));
			return;
		}
		// Not cancelable means it left the queue meanwhile: how it ended says whether a seat is to be given up.
		Self->Requests->Read(SignInNumber, TicketId, [WeakSelf, Then](TFlockResult<FFlockMatchmakingTicketReading> Read)
		{
			const TSharedPtr<FFlockMatchmaking> Reader = WeakSelf.Pin();
			if (!Reader.IsValid())
			{
				Then(TFlockResult<bool>::Fail(ShutDownError()));
				return;
			}
			if (!Read.bSuccess)
			{
				Then(TFlockResult<bool>::Fail(Read.Error));
				return;
			}
			Reader->LeaveMatched(Read.Value, Then);
		});
	});
}

void FFlockMatchmaking::LeaveMatched(const FFlockMatchmakingTicketReading& Ended, TFunction<void(TFlockResult<bool>)> Then)
{
	if (!Ended.IsMatched() || !Ended.bHasMatch || Ended.Match.SessionId.IsEmpty())
	{
		Then(TFlockResult<bool>::Ok(true));
		return;
	}
	Sessions->Get(Ended.Match.SessionId, [Then](TFlockResult<UFlockMultiplayerSession*> Got)
	{
		if (!Got.bSuccess)
		{
			Then(TFlockResult<bool>::Fail(Got.Error));
			return;
		}
		UFlockMultiplayerSession* Matched = Got.Value;
		if (Matched == nullptr || Matched->HasEnded())
		{
			Then(TFlockResult<bool>::Ok(true));
			return;
		}
		Matched->Leave([Then](TFlockResult<UFlockMultiplayerSession*> Left)
		{
			Then(Left.bSuccess ? TFlockResult<bool>::Ok(true) : TFlockResult<bool>::Fail(Left.Error));
		});
	});
}

void FFlockMatchmaking::LeaveQuietly(UFlockMultiplayerSession* Matched, TFunction<void()> Then)
{
	if (Matched == nullptr || Matched->HasEnded())
	{
		Then();
		return;
	}
	const TSharedRef<IFlockLogger> Log = Logger;
	Matched->Leave([Log, Then](TFlockResult<UFlockMultiplayerSession*> Left)
	{
		if (!Left.bSuccess)
		{
			Log->LogWarning(FString::Printf(TEXT("The session matched while the search was being cancelled could not be left: %s. The server lets the seat go when it stops hearing from it."),
				*Left.Error.Message));
		}
		Then();
	});
}

// ---- checking the ticket ----

void FFlockMatchmaking::Check(const TSharedRef<FSearch>& Checked)
{
	const TWeakPtr<FFlockMatchmaking> WeakSelf = AsShared();
	const TWeakPtr<FSearch> WeakSearch = Checked;
	const int32 SignInNumber = Checked->SignInNumber;
	Checked->Check = RepeatingCalls->Start<FFlockMatchmakingTicketRead>(CheckCallName, CheckIntervalSeconds,
		[WeakSelf, WeakSearch, SignInNumber](TFunction<void(TFlockResult<FFlockMatchmakingTicketRead>)> OnResult) -> FFlockRequestHandle
		{
			const TSharedPtr<FFlockMatchmaking> Self = WeakSelf.Pin();
			const TSharedPtr<FSearch> Sending = WeakSearch.Pin();
			if (!Self.IsValid() || !Sending.IsValid())
			{
				OnResult(TFlockResult<FFlockMatchmakingTicketRead>::Fail(ShutDownError()));
				return FFlockRequestHandle();
			}
			return Self->ReadNumbered(SignInNumber, Sending->TicketId, MoveTemp(OnResult));
		},
		[WeakSelf, WeakSearch](const FFlockMatchmakingTicketRead& Reading)
		{
			const TSharedPtr<FFlockMatchmaking> Self = WeakSelf.Pin();
			const TSharedPtr<FSearch> Answered = WeakSearch.Pin();
			if (Self.IsValid() && Answered.IsValid())
			{
				Self->TakeCheck(Answered.ToSharedRef(), Reading);
			}
		},
		[WeakSelf, WeakSearch](const FFlockError& Failure)
		{
			const TSharedPtr<FFlockMatchmaking> Self = WeakSelf.Pin();
			const TSharedPtr<FSearch> Stopped = WeakSearch.Pin();
			if (Self.IsValid() && Stopped.IsValid())
			{
				Self->CheckStopped(Stopped.ToSharedRef(), Failure);
			}
		});
}

void FFlockMatchmaking::StopChecking(const TSharedRef<FSearch>& Checked)
{
	if (Checked->Check.IsValid())
	{
		Checked->Check->Stop();
		Checked->Check.Reset();
	}
}

void FFlockMatchmaking::CheckStopped(const TSharedRef<FSearch>& Stopped, const FFlockError& Failure)
{
	if (Search != Stopped)
	{
		return;
	}
	Search.Reset();
	if (!Stopped->Caller.IsValid())
	{
		Logger->LogWarning(FString::Printf(TEXT("The party's search is no longer followed: %s. The party's next refresh, or its FindMatch, follows it again."),
			*Failure.Message));
	}
	AnswerCaller(Stopped, TFlockResult<FFlockMatchmakingResult>::Fail(Failure));
}

FFlockRequestHandle FFlockMatchmaking::ReadNumbered(int32 SignInNumber, const FString& TicketId,
	TFunction<void(TFlockResult<FFlockMatchmakingTicketRead>)> OnRead)
{
	const int32 Number = ++ReadsSent;
	if (!TicketId.IsEmpty())
	{
		return Requests->Read(SignInNumber, TicketId, [Number, OnRead](TFlockResult<FFlockMatchmakingTicketReading> Read)
		{
			if (!Read.bSuccess)
			{
				OnRead(TFlockResult<FFlockMatchmakingTicketRead>::Fail(Read.Error));
				return;
			}
			FFlockMatchmakingTicketRead Reading;
			Reading.Number = Number;
			Reading.bRead = true;
			Reading.bHasTicket = true;
			Reading.Ticket = Read.Value;
			OnRead(TFlockResult<FFlockMatchmakingTicketRead>::Ok(Reading));
		});
	}
	// A player who never searched has no ticket, which is an answer here, not a failure. Sent once: every caller of a reading
	// without the ticket's id repeats it (a refresh, a member's wait) or only marks where the party's searches stand.
	return Requests->ReadCurrent(SignInNumber, [Number, OnRead](TFlockResult<FFlockMatchmakingTicketReading> Read)
	{
		FFlockMatchmakingTicketRead Reading;
		Reading.Number = Number;
		Reading.bRead = true;
		if (Read.bSuccess)
		{
			Reading.bHasTicket = true;
			Reading.Ticket = Read.Value;
		}
		else if (Read.Error.ErrorCode != EFlockErrorCode::MatchmakingTicketNotFound)
		{
			OnRead(TFlockResult<FFlockMatchmakingTicketRead>::Fail(Read.Error));
			return;
		}
		OnRead(TFlockResult<FFlockMatchmakingTicketRead>::Ok(Reading));
	}, /*bTryOnce*/ true);
}

FFlockRequestHandle FFlockMatchmaking::ReadForParty(int32 SignInNumber, TFunction<void(const FFlockMatchmakingTicketRead&)> OnRead)
{
	const TSharedRef<IFlockLogger> Log = Logger;
	return ReadNumbered(SignInNumber, FString(), [Log, OnRead](TFlockResult<FFlockMatchmakingTicketRead> Read)
	{
		if (Read.bSuccess)
		{
			OnRead(Read.Value);
			return;
		}
		Log->LogDebug(FString::Printf(TEXT("The party refresh could not read the player's search this time: %s"), *Read.Error.Message));
		OnRead(FFlockMatchmakingTicketRead());
	});
}

bool FFlockMatchmaking::TakeIfNewest(const FFlockMatchmakingTicketRead& Reading)
{
	if (!Reading.bRead || Reading.Number <= ReadsAppliedThrough)
	{
		return false;
	}
	ReadsAppliedThrough = Reading.Number;
	return true;
}

void FFlockMatchmaking::TakeCheck(const TSharedRef<FSearch>& Checked, const FFlockMatchmakingTicketRead& Reading)
{
	if (Checked->TicketId.IsEmpty())
	{
		UFlockParty* Party = Checked->GetParty();
		if (Party == nullptr || Party->HasEnded() || Party->IsLeader())
		{
			StopWaiting(Checked, Party);
			return;
		}
		TakeCurrent(Party, Reading);
		return;
	}
	if (TakeIfNewest(Reading) && Reading.bHasTicket)
	{
		TakeTicket(Checked, Reading.Ticket);
	}
}

void FFlockMatchmaking::TakeTicket(const TSharedRef<FSearch>& Moved, const FFlockMatchmakingTicketReading& Ticket)
{
	if (Search != Moved || Moved->bEnding || Moved->bCancelledByThisGame || !Ticket.Id.Equals(Moved->TicketId, ESearchCase::CaseSensitive))
	{
		return;
	}
	if (Ticket.IsQueued())
	{
		return;
	}
	Finish(Moved, Ticket);
}

// ---- following the party's search ----

void FFlockMatchmaking::TakePartyReading(UFlockParty* Party, const FFlockMatchmakingTicketRead& Reading)
{
	TakeCurrent(Party, Reading);
}

void FFlockMatchmaking::StartWatching(UFlockParty* Party, const TSharedRef<FFlockRequestsInTurn>& InTurn, TFunction<void()> Then)
{
	if (Party == nullptr || Party->HasEnded() || IsWatching(Party))
	{
		Then();
		return;
	}
	const int32 SignInNumber = Party->SignInNumber;
	const TWeakPtr<FFlockMatchmaking> WeakSelf = AsShared();
	const TWeakObjectPtr<UFlockParty> WeakParty = Party;
	InTurn->Send([&]()
	{
		return ReadForParty(SignInNumber, [WeakSelf, WeakParty, Then](const FFlockMatchmakingTicketRead& Reading)
		{
			const TSharedPtr<FFlockMatchmaking> Self = WeakSelf.Pin();
			UFlockParty* Watched = WeakParty.Get();
			// A read the game gave up, or that failed, marks nothing: the party is the player's already, and its next refresh marks the start.
			if (Self.IsValid() && Watched != nullptr && Reading.bRead)
			{
				if (Self->IsWatching(Watched))
				{
					Self->TakeCurrent(Watched, Reading);
				}
				// Only marks the start: a search already running is followed by the next reading, once the game holds the party to listen.
				else if (!Self->bStopped && !Watched->HasEnded() && Self->TakeIfNewest(Reading))
				{
					Self->BeginWatch(Watched, Reading);
				}
			}
			Then();
		});
	});
}

bool FFlockMatchmaking::IsWatching(const UFlockParty* Party) const
{
	return Party != nullptr && PartyWatched.Equals(Party->GetId(), ESearchCase::CaseSensitive) && PartyWatchedSignInNumber == Party->SignInNumber;
}

bool FFlockMatchmaking::BeginWatch(UFlockParty* Party, const FFlockMatchmakingTicketRead& Reading)
{
	PartyWatched = Party->GetId();
	PartyWatchedSignInNumber = Party->SignInNumber;
	const bool bRunning = Reading.bHasTicket && Reading.Ticket.IsQueued() && Reading.Ticket.IsForParty(Party->GetId());
	PartyTicketSeen = !bRunning && Reading.bHasTicket ? Reading.Ticket.Id : FString();
	bPartyTicketSettled = !bRunning;
	return bRunning;
}

void FFlockMatchmaking::TakeCurrent(UFlockParty* Party, const FFlockMatchmakingTicketRead& Reading)
{
	if (bStopped || Party == nullptr || Party->HasEnded() || !TakeIfNewest(Reading))
	{
		return;
	}
	// The party's first reading here only marks where its searches stood; a search already running is followed.
	if (!IsWatching(Party) && !BeginWatch(Party, Reading))
	{
		return;
	}
	if (!Reading.bHasTicket || !Reading.Ticket.IsForParty(Party->GetId()))
	{
		return;
	}
	const FFlockMatchmakingTicketReading& Ticket = Reading.Ticket;
	if (Search.IsValid() && Search->TicketId.Equals(Ticket.Id, ESearchCase::CaseSensitive))
	{
		TakeTicket(Search.ToSharedRef(), Ticket);
		return;
	}
	if (Ticket.Id.Equals(PartyTicketSeen, ESearchCase::CaseSensitive) && bPartyTicketSettled)
	{
		return;
	}
	// This game's own search on its way is begun by its own answer, not followed from a reading.
	if (Starting.IsValid())
	{
		return;
	}
	// A search alone runs here: the server queues a player once, so the party's is not this player's yet.
	if (Search.IsValid() && (!Search->IsOfParty(Party) || !Search->TicketId.IsEmpty()))
	{
		return;
	}

	const bool bIsNew = !Ticket.Id.Equals(PartyTicketSeen, ESearchCase::CaseSensitive);
	PartyTicketSeen = Ticket.Id;
	bPartyTicketSettled = false;
	TSharedRef<FSearch> Followed = Search.IsValid() ? Search.ToSharedRef() : MakeShared<FSearch>();
	if (!Search.IsValid())
	{
		Followed->SignInNumber = Party->SignInNumber;
		Followed->Party = Party;
		Followed->bForParty = true;
		Followed->PartyId = Party->GetId();
	}
	Search = Followed;
	Followed->TicketId = Ticket.Id;
	if (bIsNew)
	{
		Party->RaiseSearchStarted();
	}
	if (Search != Followed)
	{
		return;
	}
	if (!Ticket.IsQueued())
	{
		TakeTicket(Followed, Ticket);
	}
	else if (!Followed->Check.IsValid())
	{
		Check(Followed);
	}
}

void FFlockMatchmaking::SettlePartyTicket(const FSearch& Settled)
{
	if (Settled.PartyId.Equals(PartyWatched, ESearchCase::CaseSensitive) && Settled.SignInNumber == PartyWatchedSignInNumber
		&& Settled.TicketId.Equals(PartyTicketSeen, ESearchCase::CaseSensitive))
	{
		bPartyTicketSettled = true;
	}
}

// ---- how a search ends ----

void FFlockMatchmaking::Finish(const TSharedRef<FSearch>& Finished, const FFlockMatchmakingTicketReading& Ticket)
{
	Finished->bEnding = true;
	StopChecking(Finished);
	if (!Ticket.IsMatched())
	{
		FFlockMatchmakingResult Result;
		Result.Outcome = OutcomeOf(Ticket);
		Complete(Finished, Result);
		return;
	}
	const TWeakPtr<FFlockMatchmaking> WeakSelf = AsShared();
	const TArray<FString> PlayerIds = Ticket.bHasMatch ? Ticket.Match.PlayerIds : TArray<FString>();
	MatchedSessionOrNone(Ticket, [WeakSelf, Finished, PlayerIds](UFlockMultiplayerSession* Matched)
	{
		if (const TSharedPtr<FFlockMatchmaking> Self = WeakSelf.Pin())
		{
			FFlockMatchmakingResult Result;
			Result.Outcome = FlockMatchmakingOutcomes::Matched;
			Result.Session = Matched;
			Result.PlayerIds = PlayerIds;
			Self->Complete(Finished, Result);
		}
	});
}

void FFlockMatchmaking::MatchedSessionOrNone(const FFlockMatchmakingTicketReading& Ticket, TFunction<void(UFlockMultiplayerSession*)> Then)
{
	if (!Ticket.bHasMatch)
	{
		Logger->LogWarning(TEXT("The server's answer named the search matched but carried no match, so its session is not known here. GetMySession finds it."));
		Then(nullptr);
		return;
	}
	if (Ticket.Match.SessionId.IsEmpty())
	{
		Logger->LogWarning(TEXT("The match made no session: the game's multiplayer settings in the Flock dashboard create none on a match. The result names the matched players."));
		Then(nullptr);
		return;
	}
	const TSharedRef<IFlockLogger> Log = Logger;
	Sessions->Get(Ticket.Match.SessionId, [Log, Then](TFlockResult<UFlockMultiplayerSession*> Got)
	{
		if (Got.bSuccess)
		{
			Then(Got.Value);
			return;
		}
		// A sign-in that ended while the session was on its way: the search ends as cancelled on the next frame, with no word here.
		if (Got.Error.Type != EFlockErrorType::Cancelled)
		{
			Log->LogWarning(FString::Printf(TEXT("The match's session could not be read: %s. GetMySession finds it."), *Got.Error.Message));
		}
		Then(nullptr);
	});
}

void FFlockMatchmaking::Complete(const TSharedRef<FSearch>& Completed, const FFlockMatchmakingResult& Result)
{
	// Replaced meanwhile (a sign-in that ended, a shutdown, a refused check): whoever replaced it answered the call.
	if (Search != Completed)
	{
		return;
	}
	Search.Reset();
	if (Completed->bForParty)
	{
		SettlePartyTicket(*Completed);
		UFlockParty* Party = Completed->GetParty();
		if (Party != nullptr && !Party->HasEnded())
		{
			Party->RaiseSearchEnded(Result);
		}
	}
	if (!Completed->bCallerGaveUpWhileEnding)
	{
		AnswerCaller(Completed, TFlockResult<FFlockMatchmakingResult>::Ok(Result));
		return;
	}
	// The game gave up while the result was on its way: the seat the match gave is let go, then the call hears its cancellation.
	const TSharedRef<FSearch> GivenUp = Completed;
	LeaveQuietly(Result.Session, [GivenUp]()
	{
		GivenUp->Finished = TFlockResult<FFlockMatchmakingResult>::Fail(CancelledError());
		if (GivenUp->Caller.IsValid())
		{
			const TSharedPtr<FCall> Call = GivenUp->Caller;
			GivenUp->Caller.Reset();
			Call->Answer(GivenUp->Finished.GetValue());
		}
	});
}

void FFlockMatchmaking::AnswerCaller(const TSharedRef<FSearch>& Answered, const TFlockResult<FFlockMatchmakingResult>& Result)
{
	Answered->Finished = Result;
	if (Answered->Caller.IsValid())
	{
		const TSharedPtr<FCall> Call = Answered->Caller;
		Answered->Caller.Reset();
		Call->Answer(Result);
	}
}

FString FFlockMatchmaking::OutcomeOf(const FFlockMatchmakingTicketReading& Ticket)
{
	if (Ticket.Status.Equals(FlockMatchmakingWireValues::TicketExpired, ESearchCase::CaseSensitive))
	{
		return FlockMatchmakingOutcomes::Expired;
	}
	if (Ticket.Status.Equals(FlockMatchmakingWireValues::TicketCancelled, ESearchCase::CaseSensitive))
	{
		return Ticket.CancelReason.Equals(FlockMatchmakingWireValues::CancelledForPartyChange, ESearchCase::CaseSensitive)
			? FlockMatchmakingOutcomes::PartyChanged : FlockMatchmakingOutcomes::Cancelled;
	}
	// A status the server added: the search ended as the server names it.
	return Ticket.Status;
}

// ---- the frame, quitting and shutting down ----

void FFlockMatchmaking::EndIfSignInEnded()
{
	// Its ticket, once it lands, is not cancelled: nothing more is sent for a sign-in that ended.
	if (Starting.IsValid() && Starting->SignInNumber != CurrentSignInNumber())
	{
		GiveUpStart(Starting.ToSharedRef(), SignInEndedError());
	}
	const TSharedPtr<FSearch> Ended = Search;
	if (!Ended.IsValid() || Ended->SignInNumber == CurrentSignInNumber())
	{
		return;
	}
	Search.Reset();
	StopChecking(Ended.ToSharedRef());
	AnswerCaller(Ended.ToSharedRef(), TFlockResult<FFlockMatchmakingResult>::Fail(SignInEndedError()));
}

void FFlockMatchmaking::CancelForQuit()
{
	// Flock waits for the requests on their way once the game ends, so a ticket landing then is cancelled as one the game gave up.
	if (Starting.IsValid())
	{
		GiveUpStart(Starting.ToSharedRef(), ShutDownError());
	}
	const TSharedPtr<FSearch> Quit = Search;
	if (!Quit.IsValid())
	{
		return;
	}
	if (Quit->bEnding)
	{
		// The result is on its way: the seat the match gave is let go when it lands, then the call hears its cancellation.
		Quit->bCallerGaveUpWhileEnding = true;
		return;
	}
	Search.Reset();
	StopChecking(Quit.ToSharedRef());
	if (!Quit->TicketId.IsEmpty())
	{
		Requests->Cancel(Quit->SignInNumber, Quit->TicketId, [](TFlockResult<FFlockMatchmakingTicketReading>) {});
	}
	// Flock shuts down right after: the call waiting for the search hears that now, since the shutdown finds no search left.
	AnswerCaller(Quit.ToSharedRef(), TFlockResult<FFlockMatchmakingResult>::Fail(ShutDownError()));
}

void FFlockMatchmaking::StopForShutdown()
{
	bStopped = true;
	const TSharedPtr<FSearch> Stopped = Search;
	Search.Reset();
	if (Stopped.IsValid())
	{
		StopChecking(Stopped.ToSharedRef());
		AnswerCaller(Stopped.ToSharedRef(), TFlockResult<FFlockMatchmakingResult>::Fail(ShutDownError()));
	}
	if (Starting.IsValid())
	{
		GiveUpStart(Starting.ToSharedRef(), ShutDownError());
	}
}

// ---- helpers ----

bool FFlockMatchmaking::RequireSignedIn(int32& OutSignInNumber, FString& OutPlayerId, FFlockError& OutError) const
{
	if (!Session->IsAuthenticated())
	{
		OutError = FFlockError::MakeWithFix(EFlockErrorType::Auth, TEXT("No player is signed in"), FFlockErrorHints::SignInFirst());
		return false;
	}
	// The player's own ticket among a party's is found by this id, so a sign-in that names no player searches for nobody.
	OutPlayerId = Session->GetPlayerId();
	if (OutPlayerId.IsEmpty())
	{
		OutError = FFlockError::MakeWithFix(EFlockErrorType::Auth, TEXT("The sign-in names no player: its token carries no player id"), TEXT("Call Logout and sign the player in again with one of Flock's sign-in calls."));
		return false;
	}
	OutSignInNumber = CurrentSignInNumber();
	return true;
}

bool FFlockMatchmaking::RequireQueueName(const FString& QueueName, FFlockError& OutError)
{
	if (QueueName.TrimStartAndEnd().IsEmpty())
	{
		OutError = FFlockError::MakeWithFix(EFlockErrorType::Validation, TEXT("Queue Name is empty"), TEXT("Pass the matchmaking queue's name as the Flock dashboard shows it, letter case included."));
		return false;
	}
	return true;
}

FFlockError FFlockMatchmaking::PartyEndedError(const UFlockParty* Party)
{
	const FString Reason = Party != nullptr
		? StaticEnum<EFlockPartyEndReason>()->GetNameStringByValue(static_cast<int64>(Party->GetEndReason())) : FString(TEXT("gone"));
	return FFlockError::MakeWithFix(EFlockErrorType::Validation, FString::Printf(TEXT("This party has ended (%s)"), *Reason), FFlockErrorHints::FindAnotherParty());
}

bool FFlockMatchmaking::RequireNoSearch(FFlockError& OutError) const
{
	if (Starting.IsValid() || Search.IsValid())
	{
		OutError = FFlockError::MakeWithFix(EFlockErrorType::Validation,
			TEXT("A search for a match is already running here: this player's own, or the party's that this game follows"), TEXT("Wait for its result (the party's through its FindMatch or OnSearchEnded), or cancel it through the handle FindMatch returned."));
		return false;
	}
	return true;
}

FFlockError FFlockMatchmaking::CancelledError()
{
	return FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Request cancelled"));
}

FFlockError FFlockMatchmaking::ShutDownError()
{
	return FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Flock shut down while the search was on its way"));
}

FFlockError FFlockMatchmaking::SignInEndedError()
{
	return FFlockError::Make(EFlockErrorType::Cancelled, TEXT("The player signed out or changed, or Flock shut down, while the search was running"));
}

void FFlockMatchmaking::Answer(const FFlockOnMatchmakingResult& OnComplete, const TFlockResult<FFlockMatchmakingResult>& Result)
{
	if (OnComplete)
	{
		OnComplete(Result);
	}
}
