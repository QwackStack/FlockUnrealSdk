// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockParty.h"
#include "Auth/FlockAuthSession.h"
#include "Multiplayer/FlockParties.h"

EFlockPartyEndReason UFlockParty::GetEndReason() const
{
	if (EndReasonSet != EFlockPartyEndReason::None)
	{
		return EndReasonSet;
	}
	// A sign-in that ended reads as signed out at once; the party's end is raised on the next frame.
	const TSharedPtr<FFlockAuthSession> Pinned = AuthSession.Pin();
	if (!Pinned.IsValid() || Pinned->GetSignInNumber() != SignInNumber)
	{
		return EFlockPartyEndReason::SignedOut;
	}
	return EFlockPartyEndReason::None;
}

FFlockRequestHandle UFlockParty::Leave(FFlockOnParty OnComplete)
{
	if (const TSharedPtr<FFlockParties> Pinned = Owner.Pin())
	{
		return Pinned->Leave(this, MoveTemp(OnComplete));
	}
	return RefuseWithoutOwner(OnComplete);
}

FFlockRequestHandle UFlockParty::Kick(const FString& RemovedPlayerId, FFlockOnParty OnComplete)
{
	if (const TSharedPtr<FFlockParties> Pinned = Owner.Pin())
	{
		return Pinned->Kick(this, RemovedPlayerId, MoveTemp(OnComplete));
	}
	return RefuseWithoutOwner(OnComplete);
}

FFlockRequestHandle UFlockParty::MakeLeader(const FString& NewLeaderPlayerId, FFlockOnParty OnComplete)
{
	if (const TSharedPtr<FFlockParties> Pinned = Owner.Pin())
	{
		return Pinned->MakeLeader(this, NewLeaderPlayerId, MoveTemp(OnComplete));
	}
	return RefuseWithoutOwner(OnComplete);
}

FFlockRequestHandle UFlockParty::Update(TOptional<int32> NewMaxSize, TOptional<FFlockCommandData> NewSettings, FFlockOnParty OnComplete)
{
	if (const TSharedPtr<FFlockParties> Pinned = Owner.Pin())
	{
		return Pinned->Update(this, NewMaxSize, NewSettings, MoveTemp(OnComplete));
	}
	return RefuseWithoutOwner(OnComplete);
}

FFlockRequestHandle UFlockParty::Disband(FFlockOnParty OnComplete)
{
	if (const TSharedPtr<FFlockParties> Pinned = Owner.Pin())
	{
		return Pinned->Disband(this, MoveTemp(OnComplete));
	}
	return RefuseWithoutOwner(OnComplete);
}

void UFlockParty::Begin(const TWeakPtr<FFlockParties>& InOwner, const TSharedRef<FFlockAuthSession>& InSession, int32 InSignInNumber,
	const FString& InPlayerId, const FFlockPartyReading& Reading, const TArray<FString>& MemberIds)
{
	Owner = InOwner;
	AuthSession = InSession;
	SignInNumber = InSignInNumber;
	PlayerId = InPlayerId;
	Id = Reading.Id;
	CopyFrom(Reading, MemberIds);
}

void UFlockParty::TakeReading(const FFlockPartyReading& Reading, const TArray<FString>& MemberIds)
{
	bool bMembersChanged = Members.Num() != MemberIds.Num();
	for (int32 Index = 0; !bMembersChanged && Index < MemberIds.Num(); ++Index)
	{
		bMembersChanged = !Members[Index].PlayerId.Equals(MemberIds[Index], ESearchCase::CaseSensitive);
	}
	const bool bLeaderChanged = !LeaderPlayerId.Equals(Reading.LeaderPlayerId, ESearchCase::CaseSensitive);
	CopyFrom(Reading, MemberIds);

	if (bMembersChanged)
	{
		OnMembersChanged.Broadcast();
	}
	if (bLeaderChanged)
	{
		OnLeaderChanged.Broadcast();
	}
}

void UFlockParty::End(EFlockPartyEndReason Reason, bool bRaise)
{
	EndReasonSet = Reason;
	if (bRaise)
	{
		OnEnded.Broadcast(Reason);
	}
}

void UFlockParty::CopyFrom(const FFlockPartyReading& Reading, const TArray<FString>& MemberIds)
{
	InviteCode = Reading.InviteCode;
	LeaderPlayerId = Reading.LeaderPlayerId;
	MaxSize = Reading.MaxSize;
	Settings = Reading.Settings;
	Members.Reset(MemberIds.Num());
	for (const FString& MemberId : MemberIds)
	{
		FFlockPartyMember Member;
		Member.PlayerId = MemberId;
		Members.Add(Member);
	}
}

FFlockRequestHandle UFlockParty::RefuseWithoutOwner(const FFlockOnParty& OnComplete)
{
	if (OnComplete)
	{
		OnComplete(TFlockResult<UFlockParty*>::Fail(FFlockError::Make(EFlockErrorType::Validation,
			TEXT("This party has ended (SignedOut): Flock has shut down."))));
	}
	return FFlockRequestHandle();
}
