// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockPlaytestConsent.generated.h"

/**
 * What the player let this playtest collect while they play.
 *
 * **This is the playtest's own question and nothing else's.** It decides what a playtest build records and sends to
 * Protokite, and it is deliberately separate from the Flock SDK's analytics consent, which belongs to the game and is
 * asked in the game's own words. Answering here changes neither that decision nor anything else the game collects, and
 * a game that never runs a playtest never asks this at all.
 *
 * Nothing is collected until the player has answered, and Nothing collects nothing at all: a build whose player chose
 * it behaves exactly like one with Enable Playtesting turned off.
 */
UENUM(BlueprintType)
enum class EFlockPlaytestConsentChoice : uint8
{
	/** The player has not answered yet. Nothing is collected while this is the answer. */
	NotAnswered,

	/** The screen is recorded, and play data is collected. */
	VideoAndPlayData,

	/** The screen is recorded. No play data is collected. */
	VideoOnly,

	/** Play data is collected. The screen is not recorded. */
	PlayDataOnly,

	/** Nothing is collected: no recording, no play data, and no Protokite session for the launch. */
	Nothing,
};

/**
 * The rules the choice carries, with no widget, no file and no network anywhere near them: what each answer allows,
 * and how it is spelt on the wire and to a player.
 */
namespace FlockPlaytestConsent
{
	/** Whether Choice lets the playtest record the game's screen. */
	FLOCKPLAYTEST_API bool AllowsVideoRecording(EFlockPlaytestConsentChoice Choice);

	/**
	 * Whether Choice lets the playtest collect play data: the performance windows, the level loads, the game's own
	 * playtest events, and the exceptions a playtest asks the Flock SDK for.
	 */
	FLOCKPLAYTEST_API bool AllowsPlayData(EFlockPlaytestConsentChoice Choice);

	/**
	 * Whether Choice lets the playtest run FeatureName, one of FlockPlaytestFeatures.
	 *
	 * **A feature name this plugin does not know runs only for VideoAndPlayData.** The server owns that list and may
	 * add to it, and a player cannot have agreed to something nobody described to them -- so a new feature is allowed
	 * only by the answer that allows everything, rather than being guessed into one of the two halves.
	 */
	FLOCKPLAYTEST_API bool AllowsFeature(EFlockPlaytestConsentChoice Choice, const FString& FeatureName);

	/** Whether the player has answered at all. Nothing counts as an answer; NotAnswered does not. */
	FLOCKPLAYTEST_API bool IsAnswered(EFlockPlaytestConsentChoice Choice);

	/** Whether anything at all may be collected: false while unanswered, and false for Nothing. */
	FLOCKPLAYTEST_API bool CollectsAnything(EFlockPlaytestConsentChoice Choice);

	/** How Choice is spelt in the saved decision and in what a session start sends. */
	FLOCKPLAYTEST_API FString ToWire(EFlockPlaytestConsentChoice Choice);

	/** Reads a wire spelling. Anything else, a different letter case included, is NotAnswered: the player is asked again. */
	FLOCKPLAYTEST_API EFlockPlaytestConsentChoice FromWire(const FString& Wire);

	/** One sentence saying what a choice allows, in the same words the player was shown. */
	FLOCKPLAYTEST_API FString Describe(EFlockPlaytestConsentChoice Choice);
}

/**
 * The player's answer, kept in a small file so it holds from one launch of this playtest build to the next.
 *
 * A file that is missing, unreadable, or holds anything else reads as NotAnswered, so the player is asked again and
 * nothing is collected meanwhile. That is the safe direction for every way the file can go wrong: losing an answer
 * costs one question, while guessing one would collect from somebody who never agreed to it.
 */
class FLOCKPLAYTEST_API FFlockPlaytestConsentFile
{
public:
	explicit FFlockPlaytestConsentFile(const FString& InPath);

	/** FlockPlaytest/playtest_consent.json in the project's Saved folder. */
	static FString GetDefaultPath();

	const FString& GetPath() const { return Path; }

	/** The answer the file holds, or NotAnswered when it holds none this plugin can read. */
	EFlockPlaytestConsentChoice Read() const;

	/**
	 * Saves the answer, and removes the file for NotAnswered, which is how a player asks to be asked again. Returns
	 * whether the answer is what the file now holds.
	 *
	 * **A save that fails forgets whatever was saved before**, rather than leaving an answer the player has replaced
	 * for the next launch to collect under.
	 */
	bool Save(EFlockPlaytestConsentChoice Choice) const;

	/**
	 * Removes the answer and every temporary file of it, so nothing on disk still holds it. Returns whether the file
	 * is gone.
	 */
	bool Forget() const;

private:
	FString Path;
};
