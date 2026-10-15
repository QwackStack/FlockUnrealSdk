// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Models/FlockMultiplayerModels.h"
#include "Multiplayer/FlockParty.h"
#include "FlockPartyTestListener.generated.h"

/**
 * Bind target for the party tests: a party's events are dynamic delegates, which bind only to UFUNCTIONs. Records each
 * event and, for Ended, the party's state as the handler saw it. Not guarded by WITH_AUTOMATION_TESTS: UHT registers it
 * unconditionally.
 */
UCLASS()
class UFlockPartyTestListener : public UObject
{
	GENERATED_BODY()

public:
	TWeakObjectPtr<UFlockParty> Party;
	int32 MembersChanged = 0;
	int32 LeaderChanged = 0;
	TArray<EFlockPartyEndReason> Ended;
	/** What the party's own state said inside each Ended handler: a handler must find it over. */
	TArray<bool> EndedSawPartyOver;
	/** Members and leader as the handler saw them, so an event raised before the party took its reading shows. */
	TArray<int32> MembersSeen;
	TArray<FString> LeadersSeen;
	/** What the party's search did, in order: "started", or "ended " and the outcome. */
	TArray<FString> SearchHeard;
	TArray<FFlockMatchmakingResult> SearchesEnded;
	/** IsSearching as each members handler saw it, so a search state taken after the event shows. */
	TArray<bool> SearchingSeen;
	/** Run inside every handler, after recording, to call back into the party from an event. */
	TFunction<void()> AlsoInHandler;

	void Listen(UFlockParty* InParty)
	{
		Party = InParty;
		InParty->OnMembersChanged.AddDynamic(this, &UFlockPartyTestListener::HandleMembersChanged);
		InParty->OnLeaderChanged.AddDynamic(this, &UFlockPartyTestListener::HandleLeaderChanged);
		InParty->OnEnded.AddDynamic(this, &UFlockPartyTestListener::HandleEnded);
		InParty->OnSearchStarted.AddDynamic(this, &UFlockPartyTestListener::HandleSearchStarted);
		InParty->OnSearchEnded.AddDynamic(this, &UFlockPartyTestListener::HandleSearchEnded);
	}

	int32 SearchesStarted() const { return SearchHeard.FilterByPredicate([](const FString& Heard) { return Heard == TEXT("started"); }).Num(); }

	UFUNCTION()
	void HandleSearchStarted()
	{
		SearchHeard.Add(TEXT("started"));
		if (AlsoInHandler)
		{
			AlsoInHandler();
		}
	}

	UFUNCTION()
	void HandleSearchEnded(const FFlockMatchmakingResult& Result)
	{
		SearchHeard.Add(TEXT("ended ") + Result.Outcome);
		SearchesEnded.Add(Result);
		if (AlsoInHandler)
		{
			AlsoInHandler();
		}
	}

	UFUNCTION()
	void HandleMembersChanged()
	{
		++MembersChanged;
		MembersSeen.Add(Party.IsValid() ? Party->GetMembers().Num() : -1);
		SearchingSeen.Add(Party.IsValid() && Party->IsSearching());
		if (AlsoInHandler)
		{
			AlsoInHandler();
		}
	}

	UFUNCTION()
	void HandleLeaderChanged()
	{
		++LeaderChanged;
		LeadersSeen.Add(Party.IsValid() ? Party->GetLeaderPlayerId() : FString());
		if (AlsoInHandler)
		{
			AlsoInHandler();
		}
	}

	UFUNCTION()
	void HandleEnded(EFlockPartyEndReason Reason)
	{
		Ended.Add(Reason);
		EndedSawPartyOver.Add(Party.IsValid() && Party->HasEnded());
		if (AlsoInHandler)
		{
			AlsoInHandler();
		}
	}
};
