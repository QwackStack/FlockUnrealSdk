// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Models/FlockMultiplayerModels.h"
#include "Multiplayer/FlockGameModes.h"
#include "Multiplayer/FlockMultiplayerSession.h"
#include "Multiplayer/FlockParty.h"
#include "UObject/Object.h"
#include "FlockMultiplayerNodeTestListener.generated.h"

/**
 * Bind target for the multiplayer node tests: a node's pins are dynamic delegates, which bind only to UFUNCTIONs. Each handler
 * records its own name in Fired, in order, with what its pin carried, so a test reads which pin fired and how often. Not guarded
 * by WITH_AUTOMATION_TESTS: UHT registers it unconditionally.
 */
UCLASS()
class UFlockMultiplayerNodeTestListener : public UObject
{
	GENERATED_BODY()

public:
	TArray<FString> Fired;

	UPROPERTY()
	TObjectPtr<UFlockParty> Party;

	UPROPERTY()
	TObjectPtr<UFlockMultiplayerSession> Session;

	UPROPERTY()
	FFlockMatchmakingResult Match;

	FFlockMultiplayerSessionJoinToken JoinToken;
	FString PlayerId;
	FFlockDirectAddress Address;
	FFlockTravelToSessionResult Travel;
	FFlockError Error;

	FString FiredText() const { return FString::Join(Fired, TEXT(",")); }

	UFUNCTION() void PartySucceeded(UFlockParty* InParty, const FFlockError& InError) { TakeParty(TEXT("success"), InParty, InError); }
	UFUNCTION() void PartyFound(UFlockParty* InParty, const FFlockError& InError) { TakeParty(TEXT("party"), InParty, InError); }
	UFUNCTION() void NoParty(UFlockParty* InParty, const FFlockError& InError) { TakeParty(TEXT("no-party"), InParty, InError); }
	UFUNCTION() void PartyFailed(UFlockParty* InParty, const FFlockError& InError) { TakeParty(TEXT("failure"), InParty, InError); }

	UFUNCTION() void SessionSucceeded(UFlockMultiplayerSession* InSession, const FFlockError& InError) { TakeSession(TEXT("success"), InSession, InError); }
	UFUNCTION() void SessionFound(UFlockMultiplayerSession* InSession, const FFlockError& InError) { TakeSession(TEXT("session"), InSession, InError); }
	UFUNCTION() void NoSession(UFlockMultiplayerSession* InSession, const FFlockError& InError) { TakeSession(TEXT("no-session"), InSession, InError); }
	UFUNCTION() void ConnectionPublished(UFlockMultiplayerSession* InSession, const FFlockError& InError) { TakeSession(TEXT("connection"), InSession, InError); }
	UFUNCTION() void SessionEnded(UFlockMultiplayerSession* InSession, const FFlockError& InError) { TakeSession(TEXT("session-ended"), InSession, InError); }
	UFUNCTION() void TimedOut(UFlockMultiplayerSession* InSession, const FFlockError& InError) { TakeSession(TEXT("timed-out"), InSession, InError); }
	UFUNCTION() void SessionFailed(UFlockMultiplayerSession* InSession, const FFlockError& InError) { TakeSession(TEXT("failure"), InSession, InError); }

	UFUNCTION() void Matched(const FFlockMatchmakingResult& InResult, const FFlockError& InError) { TakeMatch(TEXT("matched"), InResult, InError); }
	UFUNCTION() void NoMatch(const FFlockMatchmakingResult& InResult, const FFlockError& InError) { TakeMatch(TEXT("no-match"), InResult, InError); }
	UFUNCTION() void MatchFailed(const FFlockMatchmakingResult& InResult, const FFlockError& InError) { TakeMatch(TEXT("failure"), InResult, InError); }

	UFUNCTION() void TokenGiven(const FFlockMultiplayerSessionJoinToken& InToken, const FFlockError& InError) { Fired.Add(TEXT("success")); JoinToken = InToken; Error = InError; }
	UFUNCTION() void TokenFailed(const FFlockMultiplayerSessionJoinToken& InToken, const FFlockError& InError) { Fired.Add(TEXT("failure")); JoinToken = InToken; Error = InError; }

	UFUNCTION() void Verified(const FString& InPlayerId, const FFlockError& InError) { Fired.Add(TEXT("success")); PlayerId = InPlayerId; Error = InError; }
	UFUNCTION() void VerifyFailed(const FString& InPlayerId, const FFlockError& InError) { Fired.Add(TEXT("failure")); PlayerId = InPlayerId; Error = InError; }

	UFUNCTION() void AddressFound(const FFlockDirectAddress& InAddress, const FFlockError& InError) { Fired.Add(TEXT("success")); Address = InAddress; Error = InError; }
	UFUNCTION() void AddressFailed(const FFlockDirectAddress& InAddress, const FFlockError& InError) { Fired.Add(TEXT("failure")); Address = InAddress; Error = InError; }

	UFUNCTION() void Joined(const FFlockTravelToSessionResult& InResult, const FFlockError& InError) { TakeTravel(TEXT("joined"), InResult, InError); }
	UFUNCTION() void NotJoined(const FFlockTravelToSessionResult& InResult, const FFlockError& InError) { TakeTravel(TEXT("not-joined"), InResult, InError); }
	UFUNCTION() void TravelFailed(const FFlockTravelToSessionResult& InResult, const FFlockError& InError) { TakeTravel(TEXT("failure"), InResult, InError); }

private:
	void TakeParty(const TCHAR* Pin, UFlockParty* InParty, const FFlockError& InError) { Fired.Add(Pin); Party = InParty; Error = InError; }
	void TakeSession(const TCHAR* Pin, UFlockMultiplayerSession* InSession, const FFlockError& InError) { Fired.Add(Pin); Session = InSession; Error = InError; }
	void TakeMatch(const TCHAR* Pin, const FFlockMatchmakingResult& InResult, const FFlockError& InError) { Fired.Add(Pin); Match = InResult; Error = InError; }
	void TakeTravel(const TCHAR* Pin, const FFlockTravelToSessionResult& InResult, const FFlockError& InError) { Fired.Add(Pin); Travel = InResult; Error = InError; }
};

/** Flock Game Mode Base with the game's own check scripted by the test: approves or refuses with Reason, and counts the asks. */
UCLASS(NotBlueprintable)
class AFlockTestGameModeBase : public AFlockGameModeBase
{
	GENERATED_BODY()

public:
	bool bApproves = true;
	FString Reason;
	int32 Asked = 0;
	FFlockJoiningPlayer LastAsked;

	virtual bool ApproveJoiningPlayer_Implementation(const FFlockJoiningPlayer& JoiningPlayer, FString& RefusedReason) override
	{
		++Asked;
		LastAsked = JoiningPlayer;
		RefusedReason = Reason;
		return bApproves;
	}
};

/** Flock Game Mode with the game's own check scripted by the test, as above. */
UCLASS(NotBlueprintable)
class AFlockTestGameMode : public AFlockGameMode
{
	GENERATED_BODY()

public:
	bool bApproves = true;
	FString Reason;
	int32 Asked = 0;
	FFlockJoiningPlayer LastAsked;

	virtual bool ApproveJoiningPlayer_Implementation(const FFlockJoiningPlayer& JoiningPlayer, FString& RefusedReason) override
	{
		++Asked;
		LastAsked = JoiningPlayer;
		RefusedReason = Reason;
		return bApproves;
	}
};
