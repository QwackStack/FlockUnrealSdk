// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockPlaytestIdentity.h"

/**
 * One filled-in feedback form on its way to Protokite, and everything a later launch would need to send it.
 *
 * It carries the Protokite URL and the Game Version ID the session ran under for the same reason a kept recording does:
 * a launch that sends this may not be the one that filled it in, and Protokite finds the session's playtest from that
 * version. **It never carries an API key** -- whichever launch sends it uses its own.
 */
struct FLOCKPLAYTEST_API FFlockPlaytestFormSubmission
{
	/** The Protokite session the answers belong to. Empty when none had started, which the server allows. */
	FString PlaytestSessionId;

	/** Who filled it in. The server refuses a submission with neither a Steam id nor a device id. */
	FFlockPlaytestIdentity Identity;

	/** The answers, already shaped the way the server stores them. */
	FString AnswersJson;

	FString ProtokiteApiUrl;

	/** The Game Version ID the session started with, for a launch that sends this later. */
	FString FlockGameVersionId;

	/** The request body: session_id, steam_id or device_id, and answers. Empty members are left out. */
	FString ToJson() const;

	/** The whole submission, for keeping on disk until it can be sent. */
	FString ToSavedJson() const;

	/** Reads one back off disk. Fails when it carries no answers or nobody to attribute them to. */
	static bool FromSavedJson(const FString& Json, FFlockPlaytestFormSubmission& OutSubmission, FString& OutError);

	/**
	 * Whether sending this again would replace the first rather than adding a second.
	 *
	 * **The server overwrites by form and session**, so a re-send naming the same session lands on the same row -- which
	 * is also what makes "edit my report" work. With no session there is nothing to match on and every send makes
	 * another row, so one without a session must never be retried.
	 */
	bool CanBeSentAgainSafely() const { return !PlaytestSessionId.IsEmpty(); }
};

/**
 * The question a server complaint is about, read out of its message.
 *
 * Protokite reports a refused answer as a sentence naming the question in quotes ("Missing required answer 'steps'"),
 * with no field of its own to read, so the id is taken from the message or the complaint cannot be shown against the
 * question it belongs to. Returns empty when the message names nothing, which is how a general refusal reads.
 */
FLOCKPLAYTEST_API FString FlockPlaytestFindFieldIdInComplaint(const FString& Message);
