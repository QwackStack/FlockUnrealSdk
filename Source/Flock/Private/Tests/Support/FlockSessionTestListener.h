// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Multiplayer/FlockMultiplayerSession.h"
#include "FlockSessionTestListener.generated.h"

/**
 * Bind target for the session tests: a session's events are dynamic delegates, which bind only to UFUNCTIONs. Records each
 * event and what the session's own state said inside the handler. Not guarded by WITH_AUTOMATION_TESTS: UHT registers it
 * unconditionally.
 */
UCLASS()
class UFlockSessionTestListener : public UObject
{
	GENERATED_BODY()

public:
	TWeakObjectPtr<UFlockMultiplayerSession> Session;
	int32 PlayersChanged = 0;
	int32 HostChanged = 0;
	int32 ConnectionChanged = 0;
	TArray<FString> Ended;
	/** What the session's own state said inside each handler, so an event raised before the session took its reading shows. */
	TArray<bool> EndedSawSessionOver;
	TArray<int32> PlayersSeen;
	TArray<FString> HostsSeen;
	TArray<bool> ConnectionSeen;
	/** Run inside every handler, after recording, to call back into the session from an event. */
	TFunction<void()> AlsoInHandler;

	void Listen(UFlockMultiplayerSession* InSession)
	{
		Session = InSession;
		InSession->OnPlayersChanged.AddDynamic(this, &UFlockSessionTestListener::HandlePlayersChanged);
		InSession->OnHostChanged.AddDynamic(this, &UFlockSessionTestListener::HandleHostChanged);
		InSession->OnConnectionChanged.AddDynamic(this, &UFlockSessionTestListener::HandleConnectionChanged);
		InSession->OnEnded.AddDynamic(this, &UFlockSessionTestListener::HandleEnded);
	}

	FString EndedAs() const { return FString::Join(Ended, TEXT(",")); }

	UFUNCTION()
	void HandlePlayersChanged()
	{
		++PlayersChanged;
		PlayersSeen.Add(Session.IsValid() ? Session->GetPlayers().Num() : -1);
		if (AlsoInHandler)
		{
			AlsoInHandler();
		}
	}

	UFUNCTION()
	void HandleHostChanged()
	{
		++HostChanged;
		HostsSeen.Add(Session.IsValid() ? Session->GetHostPlayerId() : FString());
		if (AlsoInHandler)
		{
			AlsoInHandler();
		}
	}

	UFUNCTION()
	void HandleConnectionChanged()
	{
		++ConnectionChanged;
		ConnectionSeen.Add(Session.IsValid() && Session->HasConnection());
		if (AlsoInHandler)
		{
			AlsoInHandler();
		}
	}

	UFUNCTION()
	void HandleEnded(const FString& Reason)
	{
		Ended.Add(Reason);
		EndedSawSessionOver.Add(Session.IsValid() && Session->HasEnded());
		if (AlsoInHandler)
		{
			AlsoInHandler();
		}
	}
};
