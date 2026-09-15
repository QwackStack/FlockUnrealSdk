// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "FlockPlaytestConfig.generated.h"

/**
 * The feature switches a playtest's config can carry. The server owns this list and may add to it, so compare
 * against these names rather than typing them.
 */
namespace FlockPlaytestFeatures
{
	inline constexpr const TCHAR* VideoRecording = TEXT("video_recording");
	inline constexpr const TCHAR* ExceptionCapturing = TEXT("exception_capturing");
	inline constexpr const TCHAR* HeavyAnalytics = TEXT("heavy_analytics");
}

/**
 * The kinds of question a playtest feedback form can ask. A field keeps its kind as text: a server newer than
 * this plugin may send a kind it does not know, and that must not stop the rest of the form being read.
 */
namespace FlockPlaytestFormFieldTypes
{
	inline constexpr const TCHAR* Text = TEXT("text");
	inline constexpr const TCHAR* TextArea = TEXT("textarea");
	inline constexpr const TCHAR* Rating = TEXT("rating");
	inline constexpr const TCHAR* Select = TEXT("select");
	inline constexpr const TCHAR* Checkbox = TEXT("checkbox");
}

/** One question on a playtest feedback form. */
USTRUCT(BlueprintType)
struct FLOCKPLAYTEST_API FFlockPlaytestFormField
{
	GENERATED_BODY()

	/** The key the answer is submitted under. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString Id;

	/** What kind of question this is: one of FlockPlaytestFormFieldTypes, or a kind this plugin does not know. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString Type;

	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString Label;

	/** Whether an answer is needed. True when the server does not say, which is the server's own default. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	bool Required = true;

	/** Extra guidance shown with the question; empty when there is none. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString HelpText;

	/** The choices for a select question; empty for other kinds. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	TArray<FString> Options;
};

/** A playtest's published feedback form. */
USTRUCT(BlueprintType)
struct FLOCKPLAYTEST_API FFlockPlaytestForm
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString TestId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString GameId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString Title;

	/** Shown under the title; empty when there is none. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString Description;

	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	bool IsPublished = true;

	/** The questions, in the order the studio arranged them. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	TArray<FFlockPlaytestFormField> Fields;

	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString UpdatedAt;
};

/** Everything a playtest build needs from Protokite before a session starts. */
USTRUCT(BlueprintType)
struct FLOCKPLAYTEST_API FFlockPlaytestConfig
{
	GENERATED_BODY()

	/** The Protokite playtest this build is linked to. Never empty in a config that was read successfully. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString TestId;

	/** The event name the server records by itself when a session starts; the SDK must never send it. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString SessionStartedEvent;

	/** The Flock game version the playtest is linked to; empty when the server does not say. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString FlockGameVersionId;

	/** The feature switches exactly as the server sent them, names unchanged. Read them through IsFeatureEnabled. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	TMap<FString, bool> Features;

	/** The published feedback form. Only meaningful when HasForm() is true. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FFlockPlaytestForm Form;

	/**
	 * True only when the server sent FeatureName as true. A feature the config does not mention is off: a
	 * playtest saved before that feature existed never had the chance to turn it on.
	 */
	bool IsFeatureEnabled(const FString& FeatureName) const;

	/** Whether the playtest has a published form. False means show no form at all, not an empty one. */
	bool HasForm() const { return !Form.Id.IsEmpty(); }

	/** Reads the config from the server's JSON. Fails when test_id is missing or empty: nothing works without it. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPlaytestConfig& OutConfig, FString& OutError);
};
