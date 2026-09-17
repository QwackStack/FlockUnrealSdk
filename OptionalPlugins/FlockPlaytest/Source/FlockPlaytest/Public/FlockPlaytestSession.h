// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "FlockPlaytestIdentity.h"
#include "FlockPlaytestSession.generated.h"

/** The longest Flock session id Protokite's session start accepts. */
namespace FlockPlaytestSessionLimits
{
	inline constexpr int32 FlockSessionIdLength = 26;
}

/** Where this launch's Protokite session is. A launch starts at most one. */
enum class EFlockPlaytestSessionState : uint8
{
	/** Nothing has been sent yet: waiting for the playtest config and for a Flock session to reach the server. */
	NotStarted,

	/** The start request is on its way. */
	Starting,

	/** Protokite created the session, and GetPlaytestSessionId() names it. */
	Started,

	/** The session was ended: the game instance shut down, or EndPlaytestSession was called. */
	Ended,

	/** No start was sent, because there was neither a Steam id nor a device id to send. Not tried again this launch. */
	NoPlayerIdentity,

	/**
	 * Protokite did not create the session, or its answer never arrived. Not tried again this launch, because a start
	 * that reached Protokite may already have created one.
	 */
	StartFailed,
};

/** Protokite's answer to a session start. */
USTRUCT()
struct FLOCKPLAYTEST_API FFlockPlaytestSessionStartResult
{
	GENERATED_BODY()

	/** The session Protokite created. */
	UPROPERTY()
	FString SessionId;

	/**
	 * Reads the answer. Fails when session_id is missing, empty or holds whitespace: every later call names the session
	 * by it, so an answer without a usable one is not a started session.
	 */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPlaytestSessionStartResult& OutResult, FString& OutError);
};

/** Protokite's answer to a session end, which carries nothing: it answers 204 with no body. */
USTRUCT()
struct FLOCKPLAYTEST_API FFlockPlaytestSessionEndResult
{
	GENERATED_BODY()

	/** Accepts any JSON object, since the answer carries nothing to read. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPlaytestSessionEndResult& OutResult, FString& OutError);
};

/** Protokite's answer to a feedback form, which carries the stored response; nothing here needs reading back. */
USTRUCT()
struct FLOCKPLAYTEST_API FFlockPlaytestFormSubmitResult
{
	GENERATED_BODY()

	/** Accepts any JSON object: that the server took it is the whole answer. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPlaytestFormSubmitResult& OutResult, FString& OutError);
};

/** Everything a session start sends apart from the request headers. */
struct FLOCKPLAYTEST_API FFlockPlaytestSessionStartRequest
{
	FFlockPlaytestIdentity Identity;

	/** The Flock session this playtest session belongs to; left out of the request when empty. */
	FString FlockSessionId;

	/** Facts about the build and machine, sent as extra_debug. */
	TMap<FString, FString> DebugInfo;

	/** The request body: steam_id or device_id, player_name, flock_session_id and extra_debug. Empty members are left out. */
	FString ToJson() const;
};

/**
 * The extra_debug facts a session start sends: engine_version, build_configuration, gpu, map and sdk_version. The GPU
 * and the map are left out when they are not known.
 */
FLOCKPLAYTEST_API TMap<FString, FString> MakePlaytestSessionDebugInfo(const FString& MapName);
