// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Models/FlockJsonData.h"
#include "FlockMultiplayerModels.generated.h"

/** One row of the game's matchmaking queue list, as much of it as the SDK reads. */
USTRUCT()
struct FLOCK_API FFlockMatchmakingQueue
{
	GENERATED_BODY()

	UPROPERTY()
	FString Id;

	UPROPERTY()
	FString Name;
};

/** Why a party stopped being the player's. */
UENUM(BlueprintType)
enum class EFlockPartyEndReason : uint8
{
	/** The party has not ended. */
	None,
	/** The player left it with Leave. */
	Left,
	/** The player disbanded it with Disband. */
	Disbanded,
	/** The server no longer lists the player in it: removed by the leader, the party disbanded, or the player left it from another device. */
	Removed,
	/** The sign-in the party belonged to ended: a sign-out, another player signing in, or Flock shutting down. The player is still in the party on the server. */
	SignedOut,
};

/** One player in a party. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockPartyMember
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock|Multiplayer")
	FString PlayerId;
};

/**
 * A party as a party route answers it. Only the two reads (the player's party, a party by id) carry its members, in the
 * order they joined; the other answers leave MemberIds empty. Settings are the game's own data, kept with its keys as sent.
 */
USTRUCT()
struct FLOCK_API FFlockPartyReading
{
	GENERATED_BODY()

	UPROPERTY()
	FString Id;

	UPROPERTY()
	FString LeaderPlayerId;

	UPROPERTY()
	FString InviteCode;

	UPROPERTY()
	int32 MaxSize = 0;

	UPROPERTY()
	FFlockJsonData Settings;

	UPROPERTY()
	TArray<FString> MemberIds;

	/** Reads a party; one that names no party (no id) is not one, and fails the read. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPartyReading& OutStruct, FString& OutError);
};

/** The answer to "the player's party": the party, or none (the server answers a null result). */
USTRUCT()
struct FLOCK_API FFlockMyPartyReading
{
	GENERATED_BODY()

	UPROPERTY()
	bool bInParty = false;

	UPROPERTY()
	FFlockPartyReading Party;

	/** Reads the envelope: a null result is "in no party"; no result member at all is not the server's answer. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockMyPartyReading& OutStruct, FString& OutError);
};
