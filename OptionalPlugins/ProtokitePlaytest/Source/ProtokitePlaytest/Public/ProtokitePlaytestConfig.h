// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "ProtokitePlaytestConfig.generated.h"

/**
 * The feature switches a playtest's config can carry. The server owns this list and may add to it, so compare
 * against these names rather than typing them.
 */
namespace ProtokitePlaytestFeatures
{
	inline constexpr const TCHAR* VideoRecording = TEXT("video_recording");
	inline constexpr const TCHAR* ExceptionCapturing = TEXT("exception_capturing");
	inline constexpr const TCHAR* HeavyAnalytics = TEXT("heavy_analytics");
}

/**
 * The kinds of question a playtest feedback form can ask. A field keeps its kind as text: a server newer than
 * this plugin may send a kind it does not know, and that must not stop the rest of the form being read.
 */
namespace ProtokitePlaytestFormFieldTypes
{
	inline constexpr const TCHAR* Text = TEXT("text");
	inline constexpr const TCHAR* TextArea = TEXT("textarea");
	inline constexpr const TCHAR* Rating = TEXT("rating");
	inline constexpr const TCHAR* Select = TEXT("select");
	inline constexpr const TCHAR* Checkbox = TEXT("checkbox");
}

/** The range a rating question takes, as the server checks it. */
namespace ProtokitePlaytestRatings
{
	inline constexpr int32 Lowest = 1;
	inline constexpr int32 Highest = 5;
}

/** One question on a playtest feedback form. */
USTRUCT(BlueprintType)
struct PROTOKITEPLAYTEST_API FProtokitePlaytestFormField
{
	GENERATED_BODY()

	/** The key the answer is submitted under. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString Id;

	/** What kind of question this is: one of ProtokitePlaytestFormFieldTypes, or a kind this plugin does not know. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString Type;

	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString Label;

	/** Whether an answer is needed. True when the server does not say, which is the server's own default. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	bool Required = true;

	/** Extra guidance shown with the question; empty when there is none. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString HelpText;

	/** The choices for a select question; empty for other kinds. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	TArray<FString> Options;

	/** Whether this question is of Kind, one of ProtokitePlaytestFormFieldTypes. Letter for letter, as the server names kinds. */
	bool IsOfKind(const TCHAR* Kind) const { return Type.Equals(Kind, ESearchCase::CaseSensitive); }
};

/** A playtest's published feedback form. */
USTRUCT(BlueprintType)
struct PROTOKITEPLAYTEST_API FProtokitePlaytestForm
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString TestId;

	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString GameId;

	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString Title;

	/** Shown under the title; empty when there is none. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString Description;

	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	bool IsPublished = true;

	/** The questions, in the order the studio arranged them. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	TArray<FProtokitePlaytestFormField> Fields;

	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString UpdatedAt;
};

/** Everything a playtest build needs from Protokite before a session starts. */
USTRUCT(BlueprintType)
struct PROTOKITEPLAYTEST_API FProtokitePlaytestConfig
{
	GENERATED_BODY()

	/** The Protokite playtest this build is linked to. Never empty in a config that was read successfully. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString TestId;

	/** The event name the server records by itself when a session starts; the SDK must never send it. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString SessionStartedEvent;

	/** The Flock game version the playtest is linked to; empty when the server does not say. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString FlockGameVersionId;

	/** The feature switches exactly as the server sent them, names unchanged. Read them through IsFeatureEnabled. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	TMap<FString, bool> Features;

	/** The published feedback form. Only meaningful when HasForm() is true. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FProtokitePlaytestForm Form;

	/**
	 * True only when the server sent FeatureName as true. A feature the config does not mention is off: a
	 * playtest saved before that feature existed never had the chance to turn it on.
	 */
	bool IsFeatureEnabled(const FString& FeatureName) const;

	/** Whether the playtest has a published form. False means show no form at all, not an empty one. */
	bool HasForm() const { return !Form.Id.IsEmpty(); }

	/** Reads the config from the server's JSON. Fails when test_id is missing or empty: nothing works without it. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FProtokitePlaytestConfig& OutConfig, FString& OutError);
};
