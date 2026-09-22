// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "ProtokitePlaytestConsent.h"

#include "Dom/JsonObject.h"
#include "ProtokitePlaytestConfig.h"
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
		EProtokitePlaytestConsentChoice Choice;
		const TCHAR* Wire;
	};

	/** One table for both directions, so a spelling cannot be read one way and written another. */
	constexpr FConsentSpelling ConsentSpellings[] = {
		{ EProtokitePlaytestConsentChoice::NotAnswered, TEXT("not_answered") },
		{ EProtokitePlaytestConsentChoice::VideoAndPlayData, TEXT("video_and_play_data") },
		{ EProtokitePlaytestConsentChoice::VideoOnly, TEXT("video_only") },
		{ EProtokitePlaytestConsentChoice::PlayDataOnly, TEXT("play_data_only") },
		{ EProtokitePlaytestConsentChoice::Nothing, TEXT("nothing") },
	};
}

bool ProtokitePlaytestConsent::AllowsVideoRecording(EProtokitePlaytestConsentChoice Choice)
{
	return Choice == EProtokitePlaytestConsentChoice::VideoAndPlayData || Choice == EProtokitePlaytestConsentChoice::VideoOnly;
}

bool ProtokitePlaytestConsent::AllowsPlayData(EProtokitePlaytestConsentChoice Choice)
{
	return Choice == EProtokitePlaytestConsentChoice::VideoAndPlayData || Choice == EProtokitePlaytestConsentChoice::PlayDataOnly;
}

bool ProtokitePlaytestConsent::AllowsFeature(EProtokitePlaytestConsentChoice Choice, const FString& FeatureName)
{
	// Letter for letter, the way the server spells its feature names.
	if (FeatureName.Equals(ProtokitePlaytestFeatures::VideoRecording, ESearchCase::CaseSensitive))
	{
		return AllowsVideoRecording(Choice);
	}
	if (FeatureName.Equals(ProtokitePlaytestFeatures::HeavyAnalytics, ESearchCase::CaseSensitive)
		|| FeatureName.Equals(ProtokitePlaytestFeatures::ExceptionCapturing, ESearchCase::CaseSensitive))
	{
		return AllowsPlayData(Choice);
	}
	// A feature nobody described to the player, because the server added it after this build: only the answer that
	// allows everything allows it.
	return Choice == EProtokitePlaytestConsentChoice::VideoAndPlayData;
}

bool ProtokitePlaytestConsent::IsAnswered(EProtokitePlaytestConsentChoice Choice)
{
	return Choice != EProtokitePlaytestConsentChoice::NotAnswered;
}

bool ProtokitePlaytestConsent::CollectsAnything(EProtokitePlaytestConsentChoice Choice)
{
	return AllowsVideoRecording(Choice) || AllowsPlayData(Choice);
}

FString ProtokitePlaytestConsent::ToWire(EProtokitePlaytestConsentChoice Choice)
{
	for (const FConsentSpelling& Spelling : ConsentSpellings)
	{
		if (Spelling.Choice == Choice)
		{
			return Spelling.Wire;
		}
	}
	return ToWire(EProtokitePlaytestConsentChoice::NotAnswered);
}

EProtokitePlaytestConsentChoice ProtokitePlaytestConsent::FromWire(const FString& Wire)
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
	return EProtokitePlaytestConsentChoice::NotAnswered;
}

FString ProtokitePlaytestConsent::Describe(EProtokitePlaytestConsentChoice Choice)
{
	switch (Choice)
	{
	case EProtokitePlaytestConsentChoice::NotAnswered:
		return TEXT("The player has not yet said what this playtest may collect, so nothing is collected.");
	case EProtokitePlaytestConsentChoice::VideoAndPlayData:
		return TEXT("The player let this playtest record the screen and collect play data.");
	case EProtokitePlaytestConsentChoice::VideoOnly:
		return TEXT("The player let this playtest record the screen, and no play data is collected.");
	case EProtokitePlaytestConsentChoice::PlayDataOnly:
		return TEXT("The player let this playtest collect play data, and the screen is not recorded.");
	case EProtokitePlaytestConsentChoice::Nothing:
		return TEXT("The player asked this playtest to collect nothing, so it collects nothing at all.");
	}
	return FString();
}

FProtokitePlaytestConsentFile::FProtokitePlaytestConsentFile(const FString& InPath)
	: Path(InPath)
{
}

FString FProtokitePlaytestConsentFile::GetDefaultPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ProtokitePlaytest"), TEXT("playtest_consent.json"));
}

EProtokitePlaytestConsentChoice FProtokitePlaytestConsentFile::Read() const
{
	FString Contents;
	if (!FFileHelper::LoadFileToString(Contents, *Path))
	{
		return EProtokitePlaytestConsentChoice::NotAnswered;
	}

	TSharedPtr<FJsonObject> Saved;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
	if (!FJsonSerializer::Deserialize(Reader, Saved) || !Saved.IsValid())
	{
		return EProtokitePlaytestConsentChoice::NotAnswered;
	}

	FString Wire;
	Saved->TryGetStringField(ConsentMemberName, Wire);
	return ProtokitePlaytestConsent::FromWire(Wire);
}

bool FProtokitePlaytestConsentFile::Forget() const
{
	// Every temporary file goes, however fresh, not only the ones old enough to be leftovers: a save from moments ago
	// would otherwise leave the answer the player asked to be rid of sitting beside the file that no longer holds it.
	FFlockTemporaryFiles::DeleteTemporaryFilesOf(Path);
	// Even a read-only file: nothing about the flag makes an answer the player has taken back worth keeping.
	IFileManager::Get().Delete(*Path, /*bRequireExists*/ false, /*bEvenReadOnly*/ true);
	return !IFileManager::Get().FileExists(*Path);
}

bool FProtokitePlaytestConsentFile::Save(EProtokitePlaytestConsentChoice Choice) const
{
	if (Choice == EProtokitePlaytestConsentChoice::NotAnswered)
	{
		// Forgetting the answer is asking to be asked again, so the file goes rather than holding "not answered".
		return Forget();
	}

	// A launch that was killed part-way through an earlier save can leave one of these behind.
	FFlockTemporaryFiles::DeleteLeftOverFilesOf(Path);

	const TSharedRef<FJsonObject> Saved = MakeShared<FJsonObject>();
	Saved->SetStringField(ConsentMemberName, ProtokitePlaytestConsent::ToWire(Choice));
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
