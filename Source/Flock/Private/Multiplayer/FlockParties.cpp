// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockParties.h"
#include "Http/FlockErrorHints.h"
#include "UObject/Package.h"

void FFlockParties::Answer(const FFlockOnParty& OnComplete, const TFlockResult<UFlockParty*>& Result)
{
	if (OnComplete)
	{
		OnComplete(Result);
	}
}

FFlockError FFlockParties::ShutDownError()
{
	return FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Flock shut down while the party was on its way"));
}

FString FFlockParties::EndReasonName(EFlockPartyEndReason Reason)
{
	return StaticEnum<EFlockPartyEndReason>()->GetNameStringByValue(static_cast<int64>(Reason));
}

FFlockParties::FFlockParties(const TSharedRef<FFlockPartyRequests>& InRequests, const TSharedRef<FFlockRepeatingCalls>& InRepeatingCalls,
	const TSharedRef<FFlockMatchmaking>& InMatchmaking, const TSharedRef<FFlockAuthSession>& InSession,
	const TSharedRef<IFlockLogger>& InLogger, double InRefreshIntervalSeconds)
	: Requests(InRequests)
	, RepeatingCalls(InRepeatingCalls)
	, Matchmaking(InMatchmaking)
	, Session(InSession)
	, Logger(InLogger)
	, RefreshIntervalSeconds(InRefreshIntervalSeconds)
{
}

FFlockRequestHandle FFlockParties::Create(TOptional<int32> MaxSize, const FFlockCommandData& Settings, FFlockOnParty OnComplete)
{
	int32 SignInNumber = 0;
	FString PlayerId;
	if (!RequireSignedIn(SignInNumber, PlayerId, OnComplete))
	{
		return FFlockRequestHandle();
	}
	const TWeakPtr<FFlockParties> WeakSelf = AsShared();
	const TSharedRef<FFlockRequestsInTurn> InTurn = FFlockRequestsInTurn::Make();
	InTurn->Send([&]()
	{
		return Requests->Create(SignInNumber, MaxSize, Settings,
			[WeakSelf, InTurn, SignInNumber, PlayerId, OnComplete](TFlockResult<FFlockPartyReading> Created)
			{
				const TSharedPtr<FFlockParties> Self = WeakSelf.Pin();
				if (!Self.IsValid())
				{
					Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(ShutDownError()));
					return;
				}
				if (!Created.bSuccess)
				{
					Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(Created.Error));
					return;
				}
				if (!Self->StillCurrent(SignInNumber, OnComplete))
				{
					return;
				}
				// A new party is newer than every reading on its way; its only member is the player who made it, its leader.
				Self->SettleReadsSentSoFar(SignInNumber);
				UFlockParty* Party = Self->Hold(SignInNumber, PlayerId, Created.Value, { Created.Value.LeaderPlayerId });
				Self->Watched(Party, InTurn, OnComplete);
			});
	});
	return InTurn->GetHandle();
}

FFlockRequestHandle FFlockParties::Join(const FString& InviteCode, FFlockOnParty OnComplete)
{
	if (InviteCode.IsEmpty())
	{
		Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation,
			TEXT("Invite Code is empty"), TEXT("Pass the party's invite code; the leader's game reads it from the party with GetInviteCode."))));
		return FFlockRequestHandle();
	}
	int32 SignInNumber = 0;
	FString PlayerId;
	if (!RequireSignedIn(SignInNumber, PlayerId, OnComplete))
	{
		return FFlockRequestHandle();
	}
	const TWeakPtr<FFlockParties> WeakSelf = AsShared();
	const TSharedRef<FFlockRequestsInTurn> InTurn = FFlockRequestsInTurn::Make();
	InTurn->Send([&]()
	{
		return Requests->Join(SignInNumber, InviteCode, [WeakSelf, InTurn, SignInNumber, PlayerId, OnComplete](TFlockResult<FFlockPartyReading> Joined)
		{
			const TSharedPtr<FFlockParties> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(ShutDownError()));
				return;
			}
			if (!Joined.bSuccess)
			{
				Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(Joined.Error));
				return;
			}
			Self->SettleReadsSentSoFar(SignInNumber);
			if (InTurn->IsCancelled())
			{
				Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Request cancelled"))));
				return;
			}
			// The join answer carries no members, so the party is read before it is handed over.
			Self->ReadUntilNewest(SignInNumber, Joined.Value.Id, InTurn, 1,
				[WeakSelf, InTurn, SignInNumber, PlayerId, OnComplete](TFlockResult<FFlockPartyRead> Read)
				{
					const TSharedPtr<FFlockParties> Reader = WeakSelf.Pin();
					if (!Reader.IsValid())
					{
						Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(ShutDownError()));
						return;
					}
					if (!Read.bSuccess)
					{
						Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(Read.Error));
						return;
					}
					if (Reader->StillCurrent(SignInNumber, OnComplete))
					{
						Reader->Watched(Reader->Apply(Read.Value, PlayerId), InTurn, OnComplete);
					}
				});
		});
	});
	return InTurn->GetHandle();
}

FFlockRequestHandle FFlockParties::GetMine(FFlockOnParty OnComplete)
{
	int32 SignInNumber = 0;
	FString PlayerId;
	if (!RequireSignedIn(SignInNumber, PlayerId, OnComplete))
	{
		return FFlockRequestHandle();
	}
	const TWeakPtr<FFlockParties> WeakSelf = AsShared();
	const TSharedRef<FFlockRequestsInTurn> InTurn = FFlockRequestsInTurn::Make();
	ReadUntilNewest(SignInNumber, FString(), InTurn, 1, [WeakSelf, InTurn, SignInNumber, PlayerId, OnComplete](TFlockResult<FFlockPartyRead> Read)
	{
		const TSharedPtr<FFlockParties> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(ShutDownError()));
			return;
		}
		if (!Read.bSuccess)
		{
			Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(Read.Error));
			return;
		}
		if (Self->StillCurrent(SignInNumber, OnComplete))
		{
			Self->Watched(Self->Apply(Read.Value, PlayerId), InTurn, OnComplete);
		}
	});
	return InTurn->GetHandle();
}

void FFlockParties::Watched(UFlockParty* Party, const TSharedRef<FFlockRequestsInTurn>& InTurn, const FFlockOnParty& OnComplete)
{
	// The party's searches are watched from the moment the game holds it: a match made before its first refresh is not taken for an old one.
	const TWeakObjectPtr<UFlockParty> WeakParty = Party;
	Matchmaking->StartWatching(Party, InTurn, [WeakParty, OnComplete]()
	{
		Answer(OnComplete, TFlockResult<UFlockParty*>::Ok(WeakParty.Get()));
	});
}

FFlockRequestHandle FFlockParties::FindMatch(UFlockParty* Party, const FString& QueueName, const FFlockMatchmakingOptions& Options,
	FFlockOnMatchmakingResult OnComplete)
{
	return Matchmaking->FindForParty(Party, QueueName, Options, MoveTemp(OnComplete));
}

bool FFlockParties::IsSearching(const UFlockParty* Party) const
{
	return Matchmaking->IsSearching(Party);
}

FFlockRequestHandle FFlockParties::Leave(UFlockParty* Party, FFlockOnParty OnComplete)
{
	if (!RequireRunning(Party, OnComplete))
	{
		return FFlockRequestHandle();
	}
	const int32 SignInNumber = Party->SignInNumber;
	const TWeakObjectPtr<UFlockParty> WeakParty = Party;
	const TWeakPtr<FFlockParties> WeakSelf = AsShared();
	const FString PartyId = Party->Id;
	EndingByThisGame = EFlockPartyEndReason::Left;
	EndingPartyId = PartyId;
	return Requests->Leave(SignInNumber, PartyId, [WeakSelf, WeakParty, SignInNumber, PartyId, OnComplete](TFlockResult<FFlockPartyReading> Answered)
	{
		const TSharedPtr<FFlockParties> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(ShutDownError()));
			return;
		}
		Self->ForgetEndingOf(PartyId);
		// Out of the party already is what leaving asks for; a retried leave whose first answer was lost lands here too.
		const bool bAlreadyOut = !Answered.bSuccess && (Answered.Error.ErrorCode == EFlockErrorCode::PartyNotAMember
			|| Answered.Error.ErrorCode == EFlockErrorCode::PartyNotFound);
		if (!Answered.bSuccess && !bAlreadyOut)
		{
			Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(Answered.Error));
			return;
		}
		Self->SettleReadsSentSoFar(SignInNumber);
		if (UFlockParty* Left = WeakParty.Get())
		{
			Self->End(Left, EFlockPartyEndReason::Left);
		}
		Answer(OnComplete, TFlockResult<UFlockParty*>::Ok(WeakParty.Get()));
	});
}

FFlockRequestHandle FFlockParties::Kick(UFlockParty* Party, const FString& PlayerId, FFlockOnParty OnComplete)
{
	if (PlayerId.IsEmpty())
	{
		Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation,
			TEXT("Player ID is empty"), TEXT("Pass the id of a party member, from the party's GetMembers."))));
		return FFlockRequestHandle();
	}
	const TSharedRef<FFlockPartyRequests> RequestsRef = Requests;
	const int32 SignInNumber = Party != nullptr ? Party->SignInNumber : 0;
	const FString PartyId = Party != nullptr ? Party->Id : FString();
	return Change(Party, [RequestsRef, SignInNumber, PartyId, PlayerId](FFlockPartyRequests::FOnParty OnSent)
	{
		return RequestsRef->Kick(SignInNumber, PartyId, PlayerId, MoveTemp(OnSent));
	}, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockParties::MakeLeader(UFlockParty* Party, const FString& PlayerId, FFlockOnParty OnComplete)
{
	if (PlayerId.IsEmpty())
	{
		Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation,
			TEXT("Player ID is empty"), TEXT("Pass the id of a party member, from the party's GetMembers."))));
		return FFlockRequestHandle();
	}
	const TSharedRef<FFlockPartyRequests> RequestsRef = Requests;
	const int32 SignInNumber = Party != nullptr ? Party->SignInNumber : 0;
	const FString PartyId = Party != nullptr ? Party->Id : FString();
	return Change(Party, [RequestsRef, SignInNumber, PartyId, PlayerId](FFlockPartyRequests::FOnParty OnSent)
	{
		return RequestsRef->MakeLeader(SignInNumber, PartyId, PlayerId, MoveTemp(OnSent));
	}, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockParties::Update(UFlockParty* Party, TOptional<int32> MaxSize, const TOptional<FFlockCommandData>& Settings,
	FFlockOnParty OnComplete)
{
	if (!MaxSize.IsSet() && !Settings.IsSet())
	{
		Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation,
			TEXT("Nothing to update: no size or settings was given"), TEXT("Give the party a size, settings or both."))));
		return FFlockRequestHandle();
	}
	const TSharedRef<FFlockPartyRequests> RequestsRef = Requests;
	const int32 SignInNumber = Party != nullptr ? Party->SignInNumber : 0;
	const FString PartyId = Party != nullptr ? Party->Id : FString();
	return Change(Party, [RequestsRef, SignInNumber, PartyId, MaxSize, Settings](FFlockPartyRequests::FOnParty OnSent)
	{
		return RequestsRef->Update(SignInNumber, PartyId, MaxSize, Settings, MoveTemp(OnSent));
	}, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockParties::Disband(UFlockParty* Party, FFlockOnParty OnComplete)
{
	if (!RequireRunning(Party, OnComplete))
	{
		return FFlockRequestHandle();
	}
	const int32 SignInNumber = Party->SignInNumber;
	const TWeakObjectPtr<UFlockParty> WeakParty = Party;
	const TWeakPtr<FFlockParties> WeakSelf = AsShared();
	const FString PartyId = Party->Id;
	EndingByThisGame = EFlockPartyEndReason::Disbanded;
	EndingPartyId = PartyId;
	return Requests->Disband(SignInNumber, PartyId, [WeakSelf, WeakParty, SignInNumber, PartyId, OnComplete](TFlockResult<FFlockPartyReading> Answered)
	{
		const TSharedPtr<FFlockParties> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(ShutDownError()));
			return;
		}
		// Forgotten after the answer is taken, so a disband answered party.not_found ends as this game's disband.
		const bool bTaken = Self->TakeChangeAnswer(WeakParty.Get(), SignInNumber, Answered, OnComplete);
		Self->ForgetEndingOf(PartyId);
		if (!bTaken)
		{
			return;
		}
		if (UFlockParty* Disbanded = WeakParty.Get())
		{
			Self->End(Disbanded, EFlockPartyEndReason::Disbanded);
		}
		Answer(OnComplete, TFlockResult<UFlockParty*>::Ok(WeakParty.Get()));
	});
}

void FFlockParties::EndIfSignInEnded()
{
	UFlockParty* Party = Held.Get();
	if (Party != nullptr && Party->SignInNumber != CurrentSignInNumber())
	{
		End(Party, EFlockPartyEndReason::SignedOut);
	}
}

void FFlockParties::StopForShutdown()
{
	bStopped = true;
	if (UFlockParty* Party = Held.Get())
	{
		End(Party, EFlockPartyEndReason::SignedOut, /*bRaise*/ false);
	}
	StopRefreshing();
}

FFlockRequestHandle FFlockParties::Change(UFlockParty* Party, TFunction<FFlockRequestHandle(FFlockPartyRequests::FOnParty)> Send,
	FFlockOnParty OnComplete)
{
	if (!RequireRunning(Party, OnComplete))
	{
		return FFlockRequestHandle();
	}
	const int32 SignInNumber = Party->SignInNumber;
	const TWeakObjectPtr<UFlockParty> WeakParty = Party;
	const TWeakPtr<FFlockParties> WeakSelf = AsShared();
	const TSharedRef<FFlockRequestsInTurn> InTurn = FFlockRequestsInTurn::Make();
	InTurn->Send([&]()
	{
		return Send([WeakSelf, WeakParty, SignInNumber, InTurn, OnComplete](TFlockResult<FFlockPartyReading> Changed)
		{
			const TSharedPtr<FFlockParties> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(ShutDownError()));
				return;
			}
			if (!Self->TakeChangeAnswer(WeakParty.Get(), SignInNumber, Changed, OnComplete))
			{
				return;
			}
			if (InTurn->IsCancelled())
			{
				// The change went through; the next refresh brings the party.
				Answer(OnComplete, TFlockResult<UFlockParty*>::Ok(WeakParty.Get()));
				return;
			}
			InTurn->Send([&]()
			{
				return Self->ReadNumbered(SignInNumber, FString(), [WeakSelf, WeakParty, OnComplete](TFlockResult<FFlockPartyRead> Read)
				{
					if (const TSharedPtr<FFlockParties> Reader = WeakSelf.Pin())
					{
						if (Read.bSuccess)
						{
							Reader->Apply(Read.Value, FString());
						}
						else if (Read.Error.Type != EFlockErrorType::Cancelled)
						{
							Reader->Logger->LogWarning(FString::Printf(
								TEXT("The party changed, but reading it afterwards failed: %s. The next refresh brings it."), *Read.Error.Message));
						}
					}
					Answer(OnComplete, TFlockResult<UFlockParty*>::Ok(WeakParty.Get()));
				});
			});
		});
	});
	return InTurn->GetHandle();
}

bool FFlockParties::TakeChangeAnswer(UFlockParty* Party, int32 SignInNumber, const TFlockResult<FFlockPartyReading>& Answered,
	const FFlockOnParty& OnComplete)
{
	if (!Answered.bSuccess)
	{
		// A party the server no longer has for this player is over here too, whichever call found out.
		if (Answered.Error.ErrorCode == EFlockErrorCode::PartyNotFound && Party != nullptr)
		{
			End(Party, EndingOf(Party));
		}
		Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(Answered.Error));
		return false;
	}
	SettleReadsSentSoFar(SignInNumber);
	return true;
}

void FFlockParties::ReadUntilNewest(int32 SignInNumber, const FString& PartyId, const TSharedRef<FFlockRequestsInTurn>& InTurn,
	int32 Attempt, TFunction<void(TFlockResult<FFlockPartyRead>)> OnRead)
{
	const TWeakPtr<FFlockParties> WeakSelf = AsShared();
	InTurn->Send([&]()
	{
		return ReadNumbered(SignInNumber, PartyId, [WeakSelf, SignInNumber, PartyId, InTurn, Attempt, OnRead](TFlockResult<FFlockPartyRead> Read)
		{
			const TSharedPtr<FFlockParties> Self = WeakSelf.Pin();
			if (!Self.IsValid() || !Read.bSuccess || Read.Value.Number > Self->ReadsSettledThrough)
			{
				OnRead(Read);
				return;
			}
			if (Attempt == MostReadsWhileThePartyChanges)
			{
				OnRead(TFlockResult<FFlockPartyRead>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Network,
					TEXT("The party kept changing while it was being read"), TEXT("Read it again in a moment: it changes while players join and leave and while leadership moves."))));
				return;
			}
			if (InTurn->IsCancelled())
			{
				OnRead(TFlockResult<FFlockPartyRead>::Fail(FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Request cancelled"))));
				return;
			}
			Self->ReadUntilNewest(SignInNumber, PartyId, InTurn, Attempt + 1, OnRead);
		});
	});
}

FFlockRequestHandle FFlockParties::ReadNumbered(int32 SignInNumber, const FString& PartyId,
	TFunction<void(TFlockResult<FFlockPartyRead>)> OnRead)
{
	const int32 Number = ++ReadsSent;
	if (PartyId.IsEmpty())
	{
		return Requests->ReadMine(SignInNumber, [Number, SignInNumber, OnRead](TFlockResult<FFlockMyPartyReading> Mine)
		{
			if (!Mine.bSuccess)
			{
				OnRead(TFlockResult<FFlockPartyRead>::Fail(Mine.Error));
				return;
			}
			FFlockPartyRead Read;
			Read.Number = Number;
			Read.SignInNumber = SignInNumber;
			Read.bInParty = Mine.Value.bInParty;
			Read.Party = Mine.Value.Party;
			OnRead(TFlockResult<FFlockPartyRead>::Ok(Read));
		});
	}
	return Requests->Read(SignInNumber, PartyId, [Number, SignInNumber, OnRead](TFlockResult<FFlockPartyReading> One)
	{
		if (!One.bSuccess)
		{
			OnRead(TFlockResult<FFlockPartyRead>::Fail(One.Error));
			return;
		}
		FFlockPartyRead Read;
		Read.Number = Number;
		Read.SignInNumber = SignInNumber;
		Read.bInParty = true;
		Read.Party = One.Value;
		OnRead(TFlockResult<FFlockPartyRead>::Ok(Read));
	});
}

UFlockParty* FFlockParties::Apply(const FFlockPartyRead& Read, const FString& PlayerId)
{
	FFlockPartyChanges Changes;
	UFlockParty* Party = Take(Read, PlayerId, Changes);
	if (Party != nullptr && !Party->HasEnded())
	{
		Party->RaiseChanges(Changes);
	}
	return Party;
}

UFlockParty* FFlockParties::Take(const FFlockPartyRead& Read, const FString& PlayerId, FFlockPartyChanges& OutChanges)
{
	OutChanges = FFlockPartyChanges();
	// A party from a sign-in that ended is over first; that sign-in's late readings are older than any newer one.
	EndIfSignInEnded();
	if (Read.Number <= ReadsSettledThrough)
	{
		return Held.Get();
	}
	ReadsSettledThrough = Read.Number;

	UFlockParty* Party = Held.Get();
	if (Party != nullptr && Read.bInParty && Party->Id.Equals(Read.Party.Id, ESearchCase::CaseSensitive))
	{
		OutChanges = Party->TakeReading(Read.Party, Read.Party.MemberIds);
		if (!PlayerId.IsEmpty())
		{
			KeepRefreshing();
		}
		return Party;
	}
	// In no party now, or in another one than the one held.
	if (Party != nullptr)
	{
		End(Party, EndingOf(Party));
	}
	// A refresh never takes up another party: only a call the game made does.
	if (!Read.bInParty || PlayerId.IsEmpty())
	{
		return nullptr;
	}
	return Hold(Read.SignInNumber, PlayerId, Read.Party, Read.Party.MemberIds);
}

void FFlockParties::TakeRefresh(const FFlockPartyRefresh& Refreshed)
{
	FFlockPartyChanges Changes;
	UFlockParty* Party = Take(Refreshed.Party, FString(), Changes);
	if (Party == nullptr)
	{
		return;
	}
	// The search's state first, so a members or leader handler reads IsSearching as this same refresh found it.
	Matchmaking->TakePartyReading(Party, Refreshed.Ticket);
	if (!Party->HasEnded())
	{
		Party->RaiseChanges(Changes);
	}
}

FFlockRequestHandle FFlockParties::ReadForRefresh(int32 SignInNumber, TFunction<void(TFlockResult<FFlockPartyRefresh>)> OnRead)
{
	const TSharedRef<FFlockRequestsInTurn> InTurn = FFlockRequestsInTurn::Make();
	const TWeakPtr<FFlockParties> WeakSelf = AsShared();
	InTurn->Send([&]()
	{
		return ReadNumbered(SignInNumber, FString(), [WeakSelf, SignInNumber, InTurn, OnRead](TFlockResult<FFlockPartyRead> Read)
		{
			if (!Read.bSuccess)
			{
				OnRead(TFlockResult<FFlockPartyRefresh>::Fail(Read.Error));
				return;
			}
			const TSharedPtr<FFlockParties> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				OnRead(TFlockResult<FFlockPartyRefresh>::Fail(ShutDownError()));
				return;
			}
			if (InTurn->IsCancelled())
			{
				OnRead(TFlockResult<FFlockPartyRefresh>::Fail(FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Request cancelled"))));
				return;
			}
			// A refresh also reads the player's own matchmaking ticket, which is how a member's game learns of the leader's search.
			const FFlockPartyRead PartyRead = Read.Value;
			InTurn->Send([&]()
			{
				return Self->Matchmaking->ReadForParty(SignInNumber, [PartyRead, OnRead](const FFlockMatchmakingTicketRead& Ticket)
				{
					FFlockPartyRefresh Read;
					Read.Party = PartyRead;
					Read.Ticket = Ticket;
					OnRead(TFlockResult<FFlockPartyRefresh>::Ok(Read));
				});
			});
		});
	});
	return InTurn->GetHandle();
}

UFlockParty* FFlockParties::Hold(int32 SignInNumber, const FString& PlayerId, const FFlockPartyReading& Reading,
	const TArray<FString>& MemberIds)
{
	// A reading of the party already held is taken by Apply; anything held here is another party, which this one replaces.
	EndIfSignInEnded();
	// A party replaced while this game's leave of it is on its way was left, not removed.
	if (UFlockParty* Replaced = Held.Get())
	{
		End(Replaced, EndingOf(Replaced));
	}
	UFlockParty* Party = NewObject<UFlockParty>(GetTransientPackage());
	Party->Begin(AsShared(), Session, SignInNumber, PlayerId, Reading, MemberIds);
	Held.Reset(Party);
	KeepRefreshing();
	return Party;
}

void FFlockParties::SettleReadsSentSoFar(int32 SignInNumber)
{
	if (SignInNumber == CurrentSignInNumber())
	{
		ReadsSettledThrough = ReadsSent;
	}
}

EFlockPartyEndReason FFlockParties::EndingOf(const UFlockParty* Party) const
{
	const bool bEndedByThisGame = EndingByThisGame != EFlockPartyEndReason::None && Party->Id.Equals(EndingPartyId, ESearchCase::CaseSensitive);
	return bEndedByThisGame ? EndingByThisGame : EFlockPartyEndReason::Removed;
}

void FFlockParties::ForgetEndingOf(const FString& PartyId)
{
	if (EndingPartyId.Equals(PartyId, ESearchCase::CaseSensitive))
	{
		EndingByThisGame = EFlockPartyEndReason::None;
		EndingPartyId.Reset();
	}
}

void FFlockParties::End(UFlockParty* Party, EFlockPartyEndReason Reason, bool bRaise)
{
	if (Party->EndReasonSet != EFlockPartyEndReason::None)
	{
		return;
	}
	// A party whose sign-in ended reads as signed out from then on, whatever answer lands after.
	if (Party->SignInNumber != CurrentSignInNumber())
	{
		Reason = EFlockPartyEndReason::SignedOut;
	}
	// Kept alive through the end's handlers, which may drop the game's own hold on it.
	const TStrongObjectPtr<UFlockParty> KeepWhileEnding(Party);
	if (Held.Get() == Party)
	{
		Held.Reset();
		StopRefreshing();
	}
	Party->End(Reason, bRaise);
}

void FFlockParties::KeepRefreshing()
{
	UFlockParty* Party = Held.Get();
	if (RefreshIntervalSeconds <= 0.0 || Party == nullptr)
	{
		return;
	}
	if (Refresh.IsValid() && Refresh->IsRunning())
	{
		return;
	}
	const int32 SignInNumber = Party->SignInNumber;
	const TWeakPtr<FFlockParties> WeakSelf = AsShared();
	const TSharedRef<IFlockLogger> Log = Logger;
	Refresh = RepeatingCalls->Start<FFlockPartyRefresh>(RefreshCallName, RefreshIntervalSeconds,
		[WeakSelf, SignInNumber](TFunction<void(TFlockResult<FFlockPartyRefresh>)> OnResult) -> FFlockRequestHandle
		{
			if (const TSharedPtr<FFlockParties> Self = WeakSelf.Pin())
			{
				return Self->ReadForRefresh(SignInNumber, MoveTemp(OnResult));
			}
			OnResult(TFlockResult<FFlockPartyRefresh>::Fail(ShutDownError()));
			return FFlockRequestHandle();
		},
		[WeakSelf](const FFlockPartyRefresh& Refreshed)
		{
			if (const TSharedPtr<FFlockParties> Self = WeakSelf.Pin())
			{
				Self->TakeRefresh(Refreshed);
			}
		},
		[Log](const FFlockError& Failure)
		{
			Log->LogWarning(FString::Printf(TEXT("The party is no longer refreshed: %s. GetMyParty starts it again."), *Failure.Message));
		});
}

void FFlockParties::StopRefreshing()
{
	if (Refresh.IsValid())
	{
		Refresh->Stop();
		Refresh.Reset();
	}
}

bool FFlockParties::RequireSignedIn(int32& OutSignInNumber, FString& OutPlayerId, const FFlockOnParty& OnComplete) const
{
	if (!Session->IsAuthenticated())
	{
		Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Auth, TEXT("No player is signed in"), FFlockErrorHints::SignInFirst())));
		return false;
	}
	// The player id is what tells a call's reading from a refresh's (Apply), so a sign-in that names no player sends nothing.
	OutPlayerId = Session->GetPlayerId();
	if (OutPlayerId.IsEmpty())
	{
		Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Auth,
			TEXT("The sign-in names no player: its token carries no player id"), TEXT("Call Logout and sign the player in again with one of Flock's sign-in calls."))));
		return false;
	}
	OutSignInNumber = CurrentSignInNumber();
	return true;
}

bool FFlockParties::StillCurrent(int32 SignInNumber, const FFlockOnParty& OnComplete) const
{
	if (bStopped || SignInNumber != CurrentSignInNumber())
	{
		Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(FFlockError::Make(EFlockErrorType::Cancelled,
			TEXT("The player signed out or changed, or Flock shut down, while the party was on its way"))));
		return false;
	}
	return true;
}

bool FFlockParties::RequireRunning(const UFlockParty* Party, const FFlockOnParty& OnComplete)
{
	if (Party == nullptr)
	{
		Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation, TEXT("No party was given"), FFlockErrorHints::PassAHeldParty())));
		return false;
	}
	if (Party->HasEnded())
	{
		Answer(OnComplete, TFlockResult<UFlockParty*>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation, FString::Printf(TEXT("This party has ended (%s)"), *EndReasonName(Party->GetEndReason())), FFlockErrorHints::FindAnotherParty())));
		return false;
	}
	return true;
}
