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

/** A search's completion: how it ended (matched, with the session, or how it ended without a match), or a failure; the game's own cancel completes it as Cancelled. */
using FFlockOnMatchmakingResult = TFunction<void(TFlockResult<FFlockMatchmakingResult>)>;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFlockPartyChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockPartyEnded, EFlockPartyEndReason, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockPartySearchEnded, const FFlockMatchmakingResult&, Result);

/** What a reading changed about a party, raised once everything is in place. */
struct FFlockPartyChanges
{
	bool bMembers = false;
	bool bLeader = false;
};

/**
 * The player's party, read again every Party Refresh Seconds (Project Settings > Plugins > Flock SDK Settings > Multiplayer; 0 turns that off), after each change
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

	/** True while the party searches for a match, as this game knows: its own FindMatch, or the leader's search seen by the party's refresh. */
	UFUNCTION(BlueprintPure, Category = "Flock|Multiplayer")
	bool IsSearching() const;

	/** The party started searching for a match. Every member's game hears it: the leader's from its call, the others' from the party's refresh (Party Refresh Seconds) or their own FindMatch. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Multiplayer")
	FFlockPartyChanged OnSearchStarted;

	/** The party's search ended: matched, with the session the match seats this player in, or how it ended without a match. Raised before the FindMatch call waiting for it completes. */
	UPROPERTY(BlueprintAssignable, Category = "Flock|Multiplayer")
	FFlockPartySearchEnded OnSearchEnded;

	/**
	 * Searches for a match as the party, matched as one unit and never split. The leader's game starts the search in the queue
	 * named QueueName (letter case included); another member's game waits for the leader's search (it may pass an empty name)
	 * and gets the same match; the wait ends (Validation) if the party ends first, or if the player comes to lead it. A player joining or leaving the party during the search stops it (party_changed). Cancelling
	 * the handle cancels the party's search on the server, for every member, and completes the call as Cancelled.
	 */
	FFlockRequestHandle FindMatch(const FString& QueueName, const FFlockMatchmakingOptions& Options, FFlockOnMatchmakingResult OnComplete);

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
	friend class FFlockMatchmaking;

	/** Fills a party the owner has just made; called once, before anyone else sees it. */
	void Begin(const TWeakPtr<FFlockParties>& InOwner, const TSharedRef<FFlockAuthSession>& InSession, int32 InSignInNumber,
		const FString& InPlayerId, const FFlockPartyReading& Reading, const TArray<FString>& MemberIds);

	/** Takes a newer reading and says what changed; the owner raises the changes once everything else (the search's state) is in place. */
	FFlockPartyChanges TakeReading(const FFlockPartyReading& Reading, const TArray<FString>& MemberIds);
	void RaiseChanges(const FFlockPartyChanges& Changes);
	void RaiseSearchStarted();
	void RaiseSearchEnded(const FFlockMatchmakingResult& Result);

	/** Set once, by the owner. */
	void End(EFlockPartyEndReason Reason, bool bRaise);

	void CopyFrom(const FFlockPartyReading& Reading, const TArray<FString>& MemberIds);

	/** The party's calls when its owner is gone (Flock shut down): refused, as for any party that has ended. */
	FFlockRequestHandle RefuseWithoutOwner(const FFlockOnParty& OnComplete);

	TWeakPtr<FFlockParties> Owner;
	TWeakPtr<FFlockAuthSession> AuthSession;
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
