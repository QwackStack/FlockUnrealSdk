// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Models/FlockAnalyticsModels.h"
#include "FlockEventModels.generated.h"

/** How the player authenticated (see UFlockEvents::OnAuthenticated). */
UENUM(BlueprintType)
enum class EFlockAuthMethod : uint8
{
	Email,
	Device,
	Google,
	Apple,
	Steam,
	Facebook,
	Discord,
	/** Restored from the token store at startup. */
	SessionRestore,
};

/** Payload of UFlockEvents::OnAuthenticated. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockAuthInfo
{
	GENERATED_BODY()

	/** Player id from the access-token claims. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	EFlockAuthMethod Method = EFlockAuthMethod::Email;
};

/** Why a session ended (see UFlockEvents::OnSessionEnded). */
UENUM(BlueprintType)
enum class EFlockSessionEndReason : uint8
{
	/** The player logged out or auth tokens were cleared. */
	Logout,
	/** Backgrounded past the session timeout. */
	Timeout,
	/** The application quit. */
	Quit,
	/** A new session replaced this one. */
	Restarted,
	/** Ended explicitly via the analytics provider. */
	Manual,
};

/** Payload of UFlockEvents::OnSessionEnded. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockSessionEndedArgs
{
	GENERATED_BODY()

	/** Final session metrics (duration, screens, pauses, FPS). */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockSessionSnapshot Snapshot;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	EFlockSessionEndReason Reason = EFlockSessionEndReason::Manual;
};
