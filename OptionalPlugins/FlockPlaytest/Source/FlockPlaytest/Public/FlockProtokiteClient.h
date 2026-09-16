// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockPlaytestConfig.h"
#include "FlockPlaytestRecordingUpload.h"
#include "FlockPlaytestSession.h"
#include "Http/FlockProviderBase.h"

/**
 * Calls the Protokite API for the playtest plugin, over the Flock SDK's HTTP client, retry handling and error
 * model. Every call carries the Flock SDK's own request headers: Protokite identifies the game and the playtest
 * from the game's API key and version id, and never from a player's sign-in.
 */
class FLOCKPLAYTEST_API FFlockProtokiteClient : public FFlockProviderBase
{
public:
	using FFlockProviderBase::FFlockProviderBase;

	/** The playtest-config address for a Protokite API base URL, with or without trailing slashes. */
	static FString MakePlaytestConfigUrl(const FString& ProtokiteApiUrl);

	/** The session-start address for a Protokite API base URL, with or without trailing slashes. */
	static FString MakePlaytestSessionStartUrl(const FString& ProtokiteApiUrl);

	/** The address that ends one session. The session id is percent-encoded. */
	static FString MakePlaytestSessionEndUrl(const FString& ProtokiteApiUrl, const FString& PlaytestSessionId);

	/**
	 * Fetches the playtest this build's version is linked to.
	 *
	 * Retried when Protokite or the network fails, never when Protokite refuses (401, 404, 422): those answers
	 * do not change on a second try. Outcomes are not logged here; the caller reports each one at the level
	 * its meaning for playtesting deserves.
	 */
	FFlockRequestHandle FetchPlaytestConfig(const FString& ProtokiteApiUrl, const TMap<FString, FString>& RequestHeaders,
		TFunction<void(TFlockResult<FFlockPlaytestConfig>)> OnComplete);

	/**
	 * Starts a playtest session. Sent once and never retried, whatever the Flock SDK's retry settings say: every
	 * start creates a session, so a second attempt after an answer that got lost would leave two. An answer without a
	 * usable session_id fails. Outcomes are not logged here.
	 */
	FFlockRequestHandle StartPlaytestSession(const FString& ProtokiteApiUrl, const TMap<FString, FString>& RequestHeaders,
		const FFlockPlaytestSessionStartRequest& Request, TFunction<void(TFlockResult<FFlockPlaytestSessionStartResult>)> OnComplete);

	/**
	 * Ends a playtest session. Protokite ignores an end for a session that has already ended, so this is retried like
	 * a read. Its 204 answer with no body is a success. Outcomes are not logged here.
	 */
	FFlockRequestHandle EndPlaytestSession(const FString& ProtokiteApiUrl, const TMap<FString, FString>& RequestHeaders,
		const FString& PlaytestSessionId, TFunction<void(TFlockResult<FFlockPlaytestSessionEndResult>)> OnComplete);

	/** The address a session's recording upload link is asked for. */
	static FString MakeRecordingUploadUrl(const FString& ProtokiteApiUrl, const FString& PlaytestSessionId);

	/**
	 * Asks Protokite where to put a session's recording. The answer is a short-lived presigned link, so this is asked
	 * only once the recording is finished and the upload can start at once.
	 *
	 * **A successful answer here does not mean the recording was uploaded**: Protokite counts the session as having a
	 * recording from the moment it issues a link, and only the PUT that follows says the bytes arrived.
	 *
	 * Retried like a read. Issuing a link creates nothing a second request would duplicate -- unlike a session start --
	 * and the caller asks for a fresh link for every attempt anyway. Outcomes are not logged here.
	 */
	FFlockRequestHandle RequestRecordingUploadLink(const FString& ProtokiteApiUrl, const TMap<FString, FString>& RequestHeaders,
		const FString& PlaytestSessionId, const FString& ContentType,
		TFunction<void(TFlockResult<FFlockPlaytestRecordingUploadLink>)> OnComplete);

private:
	/** Joins a route onto the base URL. Only trailing slashes are removed, because a URL with whitespace is refused before this. */
	static FString JoinProtokiteUrl(const FString& ProtokiteApiUrl, const FString& Route);
};
