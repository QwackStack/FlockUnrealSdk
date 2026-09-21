// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestConsent.h"

#include "Dom/JsonObject.h"
#include "FlockPlaytestConfig.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockTemporaryFiles.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	/** The member the answer is saved and sent under. */
	constexpr const TCHAR* ConsentMemberName = TEXT("playtest_consent");

	struct FConsentSpelling
	{
		EFlockPlaytestConsentChoice Choice;
		const TCHAR* Wire;
	};

	/** One table for both directions, so a spelling cannot be read one way and written another. */
	constexpr FConsentSpelling ConsentSpellings[] = {
		{ EFlockPlaytestConsentChoice::NotAnswered, TEXT("not_answered") },
		{ EFlockPlaytestConsentChoice::VideoAndPlayData, TEXT("video_and_play_data") },
		{ EFlockPlaytestConsentChoice::VideoOnly, TEXT("video_only") },
		{ EFlockPlaytestConsentChoice::PlayDataOnly, TEXT("play_data_only") },
		{ EFlockPlaytestConsentChoice::Nothing, TEXT("nothing") },
	};
}

bool FlockPlaytestConsent::AllowsVideoRecording(EFlockPlaytestConsentChoice Choice)
{
	return Choice == EFlockPlaytestConsentChoice::VideoAndPlayData || Choice == EFlockPlaytestConsentChoice::VideoOnly;
}

bool FlockPlaytestConsent::AllowsPlayData(EFlockPlaytestConsentChoice Choice)
{
	return Choice == EFlockPlaytestConsentChoice::VideoAndPlayData || Choice == EFlockPlaytestConsentChoice::PlayDataOnly;
}

bool FlockPlaytestConsent::AllowsFeature(EFlockPlaytestConsentChoice Choice, const FString& FeatureName)
{
	// Letter for letter, the way the server spells its feature names.
	if (FeatureName.Equals(FlockPlaytestFeatures::VideoRecording, ESearchCase::CaseSensitive))
	{
		return AllowsVideoRecording(Choice);
	}
	if (FeatureName.Equals(FlockPlaytestFeatures::HeavyAnalytics, ESearchCase::CaseSensitive)
		|| FeatureName.Equals(FlockPlaytestFeatures::ExceptionCapturing, ESearchCase::CaseSensitive))
	{
		return AllowsPlayData(Choice);
	}
	// A feature nobody described to the player, because the server added it after this build: only the answer that
	// allows everything allows it.
	return Choice == EFlockPlaytestConsentChoice::VideoAndPlayData;
}

bool FlockPlaytestConsent::IsAnswered(EFlockPlaytestConsentChoice Choice)
{
	return Choice != EFlockPlaytestConsentChoice::NotAnswered;
}

bool FlockPlaytestConsent::CollectsAnything(EFlockPlaytestConsentChoice Choice)
{
	return AllowsVideoRecording(Choice) || AllowsPlayData(Choice);
}

FString FlockPlaytestConsent::ToWire(EFlockPlaytestConsentChoice Choice)
{
	for (const FConsentSpelling& Spelling : ConsentSpellings)
	{
		if (Spelling.Choice == Choice)
		{
			return Spelling.Wire;
		}
	}
	return ToWire(EFlockPlaytestConsentChoice::NotAnswered);
}

EFlockPlaytestConsentChoice FlockPlaytestConsent::FromWire(const FString& Wire)
{
	for (const FConsentSpelling& Spelling : ConsentSpellings)
	{
		// Letter for letter: a spelling that is not one of these is an answer this build cannot be sure of, and the
		// player is asked again rather than having one read into it.
		if (Wire.Equals(Spelling.Wire, ESearchCase::CaseSensitive))
		{
			return Spelling.Choice;
		}
	}
	return EFlockPlaytestConsentChoice::NotAnswered;
}

FString FlockPlaytestConsent::Describe(EFlockPlaytestConsentChoice Choice)
{
	switch (Choice)
	{
	case EFlockPlaytestConsentChoice::NotAnswered:
		return TEXT("The player has not yet said what this playtest may collect, so nothing is collected.");
	case EFlockPlaytestConsentChoice::VideoAndPlayData:
		return TEXT("The player let this playtest record the screen and collect play data.");
	case EFlockPlaytestConsentChoice::VideoOnly:
		return TEXT("The player let this playtest record the screen, and no play data is collected.");
	case EFlockPlaytestConsentChoice::PlayDataOnly:
		return TEXT("The player let this playtest collect play data, and the screen is not recorded.");
	case EFlockPlaytestConsentChoice::Nothing:
		return TEXT("The player asked this playtest to collect nothing, so it collects nothing at all.");
	}
	return FString();
}

FFlockPlaytestConsentFile::FFlockPlaytestConsentFile(const FString& InPath)
	: Path(InPath)
{
}

FString FFlockPlaytestConsentFile::GetDefaultPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FlockPlaytest"), TEXT("playtest_consent.json"));
}

EFlockPlaytestConsentChoice FFlockPlaytestConsentFile::Read() const
{
	FString Contents;
	if (!FFileHelper::LoadFileToString(Contents, *Path))
	{
		return EFlockPlaytestConsentChoice::NotAnswered;
	}

	TSharedPtr<FJsonObject> Saved;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
	if (!FJsonSerializer::Deserialize(Reader, Saved) || !Saved.IsValid())
	{
		return EFlockPlaytestConsentChoice::NotAnswered;
	}

	FString Wire;
	Saved->TryGetStringField(ConsentMemberName, Wire);
	return FlockPlaytestConsent::FromWire(Wire);
}

bool FFlockPlaytestConsentFile::Forget() const
{
	// Every temporary file goes, however fresh, not only the ones old enough to be leftovers: a save from moments ago
	// would otherwise leave the answer the player asked to be rid of sitting beside the file that no longer holds it.
	FFlockTemporaryFiles::DeleteTemporaryFilesOf(Path);
	// Even a read-only file: nothing about the flag makes an answer the player has taken back worth keeping.
	IFileManager::Get().Delete(*Path, /*bRequireExists*/ false, /*bEvenReadOnly*/ true);
	return !IFileManager::Get().FileExists(*Path);
}

bool FFlockPlaytestConsentFile::Save(EFlockPlaytestConsentChoice Choice) const
{
	if (Choice == EFlockPlaytestConsentChoice::NotAnswered)
	{
		// Forgetting the answer is asking to be asked again, so the file goes rather than holding "not answered".
		return Forget();
	}

	// A launch that was killed part-way through an earlier save can leave one of these behind.
	FFlockTemporaryFiles::DeleteLeftOverFilesOf(Path);

	const TSharedRef<FJsonObject> Saved = MakeShared<FJsonObject>();
	Saved->SetStringField(ConsentMemberName, FlockPlaytestConsent::ToWire(Choice));
	// Written for whoever opens the file, never read back: only the answer decides anything.
	Saved->SetStringField(TEXT("answered_at"), FDateTime::UtcNow().ToIso8601());

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);

	// Saved beside the file and moved over it, the way every other file this plugin keeps is written, so a launch that
	// ends mid-save never leaves a half-written answer to be read next time.
	if (FJsonSerializer::Serialize(Saved, Writer)
		&& FFlockTemporaryFiles::SaveThenMove(Json, Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		&& Read() == Choice)
	{
		return true;
	}

	// **A save that failed must not leave an older answer standing.** Whatever is on disk is the answer this player
	// has replaced, and keeping it would have the next launch collect under an answer they changed their mind about --
	// silently, since a file that holds an answer is never questioned. Forgetting it costs one more question instead.
	Forget();
	return false;
}
