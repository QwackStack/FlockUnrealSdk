// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockResult.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockJsonData.h"
#include "Models/FlockMultiplayerModels.h"
#include "FlockParty.generated.h"

class FFlockAuthSession;
class FFlockParties;
class UFlockParty;

/** A party call's completion: the party as it stands once the call is done (ended, after a leave or disband). */
using FFlockOnParty = TFunction<void(TFlockResult<UFlockParty*>)>;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFlockPartyChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockPartyEnded, EFlockPartyEndReason, Reason);

/**
 * The player's party, read again every Party Refresh Seconds (Project Settings > Flock; 0 turns that off), after each change
 * this game makes, and by GetMyParty. The SDK keeps it alive while it is the player's; a game that keeps one past its end
 * holds it in a UPROPERTY. Game-thread only.
 */
UCLASS(BlueprintType)
class FLOCK_API UFlockParty : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	FString GetId() const { return Id; }

	/** The code other players join with (Join Party). Letter case does not matter. */
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	FString GetInviteCode() const { return InviteCode; }

	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	FString GetLeaderPlayerId() const { return LeaderPlayerId; }

	/** True when the signed-in player leads the party: only the leader can remove players, hand over leadership, update or disband it. */
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	bool IsLeader() const { return LeaderPlayerId.Equals(PlayerId, ESearchCase::CaseSensitive); }

	/** The players in the party, in the order they joined. When the leader leaves, the first of the others leads. */
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	TArray<FFlockPartyMember> GetMembers() const { return Members; }

	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	int32 GetMaxSize() const { return MaxSize; }

	/** The game's own data on the party, as the leader last set it with Update; its keys as sent. */
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	FFlockJsonData GetSettings() const { return Settings; }

	/** None while the party is the player's; why it stopped being so once it has ended. */
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	EFlockPartyEndReason GetEndReason() const;

	/** True once the party is no longer the player's here; its calls are then refused and it is no longer refreshed. */
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	bool HasEnded() const { return GetEndReason() != EFlockPartyEndReason::None; }

	/** The players in the party changed. Raised with the party already up to date. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Multiplayer")
	FFlockPartyChanged OnMembersChanged;

	/** Another player leads the party now. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Multiplayer")
	FFlockPartyChanged OnLeaderChanged;

	/** The party stopped being the player's, raised once, with the party already ended. Not raised when Flock shuts down. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Multiplayer")
	FFlockPartyEnded OnEnded;

	/** Leaves the party. If the server says the player is already out of it, that counts as leaving. When the leader leaves, the first of the others leads; the last one out disbands it. */
	FFlockRequestHandle Leave(FFlockOnParty OnComplete);

	/** Removes another player from the party. Leader only; the leader cannot remove themselves. */
	FFlockRequestHandle Kick(const FString& RemovedPlayerId, FFlockOnParty OnComplete);

	/** Hands leadership to another player in the party. Leader only. */
	FFlockRequestHandle MakeLeader(const FString& NewLeaderPlayerId, FFlockOnParty OnComplete);

	/** Changes the party's size, its settings or both; leave one unset to keep it. New settings replace all of the old ones. Leader only. */
	FFlockRequestHandle Update(TOptional<int32> NewMaxSize, TOptional<FFlockCommandData> NewSettings, FFlockOnParty OnComplete);

	/** Disbands the party, so every player in it is free to make or join another. Leader only. */
	FFlockRequestHandle Disband(FFlockOnParty OnComplete);

private:
	friend class FFlockParties;

	/** Fills a party the owner has just made; called once, before anyone else sees it. */
	void Begin(const TWeakPtr<FFlockParties>& InOwner, const TSharedRef<FFlockAuthSession>& InSession, int32 InSignInNumber,
		const FString& InPlayerId, const FFlockPartyReading& Reading, const TArray<FString>& MemberIds);

	/** Takes a newer reading, then says what changed; events are raised once everything is in place. */
	void TakeReading(const FFlockPartyReading& Reading, const TArray<FString>& MemberIds);

	/** Set once, by the owner. */
	void End(EFlockPartyEndReason Reason, bool bRaise);

	void CopyFrom(const FFlockPartyReading& Reading, const TArray<FString>& MemberIds);

	/** The party's calls when its owner is gone (Flock shut down): refused, as for any party that has ended. */
	FFlockRequestHandle RefuseWithoutOwner(const FFlockOnParty& OnComplete);

	TWeakPtr<FFlockParties> Owner;
	TWeakPtr<FFlockAuthSession> Session;
	int32 SignInNumber = 0;
	FString PlayerId;
	/** Set once, by the owner, when the party ends; a sign-in that ended reads as SignedOut before then. */
	EFlockPartyEndReason EndReasonSet = EFlockPartyEndReason::None;

	FString Id;
	FString InviteCode;
	FString LeaderPlayerId;
	int32 MaxSize = 0;
	FFlockJsonData Settings;
	TArray<FFlockPartyMember> Members;
};
