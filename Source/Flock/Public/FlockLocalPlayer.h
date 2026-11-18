// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "FlockEventModels.h"
#include "Providers/FlockAuthProvider.h"
#include "Providers/FlockCommandProvider.h"
#include "Providers/FlockLeaderboardProvider.h"
#include "Providers/FlockPlayerProvider.h"
#include "FlockLocalPlayer.generated.h"

class UFlockEvents;

/**
 * One of several players signed in at the same time on this machine, for couch and booth games where players take turns or
 * play side by side. Made with UFlockSubsystem::AddLocalPlayer (Blueprint: Add Local Player) and ended with RemoveLocalPlayer.
 * Not the engine's ULocalPlayer, which is a split-screen viewport: a Flock local player is a sign-in.
 *
 *     UFlockLocalPlayer* Sara = Flock->AddLocalPlayer();
 *     Sara->GetAuthProvider()->LoginWithDevice(TEXT("typed-name:sara"), [](TFlockResult<FFlockPlayerLoginResponse> Result) {});
 *
 * **Each local player has a sign-in of its own.** The providers below are the ones the subsystem offers, built over this
 * player's own sign-in and HTTP client, so one player signing in never changes who another player's calls act for. The player
 * signed in through the subsystem is a separate player again, and is not affected either. In Blueprint, the Flock auth,
 * player, command and leaderboard nodes take this object on their Local Player pin.
 *
 * **Kept in memory only.** A local player's sign-in is not saved, so it ends with the game; sign them in again on the next
 * launch. **Their sign-ins and sign-outs are raised on GetEvents(), not on the subsystem's events**, and analytics belongs to
 * the player signed in through the subsystem alone.
 */
// Made only by AddLocalPlayer: one constructed any other way has no sign-in or providers.
UCLASS(BlueprintType, meta = (DontUseGenericSpawnObject = "true"))
class FLOCK_API UFlockLocalPlayer : public UObject
{
	GENERATED_BODY()

	friend class UFlockSubsystem;

public:
	/** Signing in, registering, linking and signing out, for this player only. */
	FFlockAuthProvider* GetAuthProvider() const { return AuthProvider.Get(); }

	/** This player's saved data. */
	FFlockPlayerProvider* GetPlayerProvider() const { return PlayerProvider.Get(); }

	/** Writing to this player's saved data. */
	FFlockCommandProvider* GetCommandProvider() const { return CommandProvider.Get(); }

	/** Where this player stands on a board. */
	FFlockLeaderboardProvider* GetLeaderboardProvider() const { return LeaderboardProvider.Get(); }

	/** This player's own sign-in, sign-out and session events. */
	UFUNCTION(BlueprintPure, Category = "Flock|Local Players")
	UFlockEvents* GetEvents() const { return Events; }

	/** The signed-in player's id, or empty before they sign in and once they are removed. */
	UFUNCTION(BlueprintPure, Category = "Flock|Local Players")
	FString GetPlayerId() const;

	/** Whether this local player is signed in. */
	UFUNCTION(BlueprintPure, Category = "Flock|Local Players")
	bool IsAuthenticated() const;

private:
	/**
	 * Ends this player for good: its sign-in ends in memory and its HTTP client closes, so requests on the wire finish and every
	 * later call, a retry or a late sign-in answer included, fails with Reason and Fix; Logged Out is raised on its own events
	 * when it was signed in. Only the first call counts.
	 */
	void Release(const FString& Reason, const FString& Fix);

	/** A queue of writes kept for this player while offline is sent once they sign in. */
	UFUNCTION()
	void HandleAuthenticated(const FFlockAuthInfo& Info);

	UPROPERTY(Transient)
	TObjectPtr<UFlockEvents> Events;

	bool bReleased = false;

	// Shared, like the subsystem's: completions on their way hold what they need, so a player let go mid-request stays safe.
	TSharedPtr<FFlockHttpClient> HttpClient;
	TSharedPtr<FFlockAuthSession> AuthSession;
	TUniquePtr<FFlockAuthProvider> AuthProvider;
	TSharedPtr<FFlockPlayerProvider> PlayerProvider;
	TSharedPtr<FFlockCommandProvider> CommandProvider;
	TSharedPtr<FFlockLeaderboardProvider> LeaderboardProvider;
};
