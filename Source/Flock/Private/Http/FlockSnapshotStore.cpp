// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Http/FlockSnapshotStore.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const TCHAR* const SnapshotExtension = TEXT(".json");
}

const TCHAR* const FFlockSnapshotStore::BootstrapScope = TEXT("bootstrap");

FFlockSnapshotStore::FFlockSnapshotStore(const FString& InRootDirectory, const TSharedRef<IFlockLogger>& InLogger,
	const FString& InSdkVersion)
	: Root(InRootDirectory.IsEmpty() ? DefaultRoot() : InRootDirectory)
	, Logger(InLogger)
	, SdkVersion(InSdkVersion)
{
}

FString FFlockSnapshotStore::DefaultRoot()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Flock"), TEXT("snapshots"));
}

void FFlockSnapshotStore::Write(const FString& Scope, const FString& Key, const FString& Payload)
{
	// Parse the payload as a JSON value so it nests as real structure in the envelope (readable, not a
	// double-escaped blob). A response is always valid JSON; anything else is a caller bug, so drop it.
	TSharedPtr<FJsonValue> PayloadValue;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Payload);
	if (!FJsonSerializer::Deserialize(Reader, PayloadValue) || !PayloadValue.IsValid())
	{
		Logger->LogWarning(FString::Printf(TEXT("Snapshot write skipped for %s/%s: payload is not valid JSON"), *Scope, *Key));
		return;
	}

	const TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetNumberField(TEXT("v"), EnvelopeVersion);
	Envelope->SetStringField(TEXT("sdk"), SdkVersion);
	Envelope->SetStringField(TEXT("stored_at_utc"), FDateTime::UtcNow().ToIso8601());
	Envelope->SetField(TEXT("data"), PayloadValue);

	FString Serialized;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Envelope, Writer))
	{
		Logger->LogWarning(FString::Printf(TEXT("Snapshot write failed for %s/%s: could not serialize envelope"), *Scope, *Key));
		return;
	}

	const FString Path = BuildPath(Scope, Key);
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(Path));

	// Write to a temp then atomically replace, so a crash mid-write can't leave the prior snapshot deleted
	// and the new one unwritten (the delete-then-move window).
	const FString TmpPath = Path + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(Serialized, *TmpPath))
	{
		Logger->LogWarning(FString::Printf(TEXT("Snapshot write failed for %s/%s: could not write temp file"), *Scope, *Key));
		return;
	}
	if (!IFileManager::Get().Move(*Path, *TmpPath, /*bReplace*/ true))
	{
		Logger->LogWarning(FString::Printf(TEXT("Snapshot write failed for %s/%s: could not replace target"), *Scope, *Key));
		TryDelete(TmpPath);
	}
}

bool FFlockSnapshotStore::TryRead(const FString& Scope, const FString& Key, FString& OutPayload) const
{
	const FString Path = BuildPath(Scope, Key);
	if (!IFileManager::Get().FileExists(*Path))
	{
		return false;
	}

	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *Path))
	{
		// Present but unreadable: a miss, not an error, and not deleted (a transient lock shouldn't drop it).
		return false;
	}

	TSharedPtr<FJsonObject> Envelope;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(FileContent);
	if (!FJsonSerializer::Deserialize(Reader, Envelope) || !Envelope.IsValid())
	{
		Logger->LogWarning(FString::Printf(TEXT("Snapshot read failed for %s/%s: corrupt envelope, dropping"), *Scope, *Key));
		TryDelete(Path);
		return false;
	}

	double Version = 0.0;
	const TSharedPtr<FJsonValue> DataValue = Envelope->TryGetField(TEXT("data"));
	if (!Envelope->TryGetNumberField(TEXT("v"), Version) || static_cast<int32>(Version) != EnvelopeVersion
		|| !DataValue.IsValid() || DataValue->Type == EJson::Null)
	{
		// Stale format or missing payload: invalidate so the next fetch repopulates in the current format.
		TryDelete(Path);
		return false;
	}

	FString Serialized;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(DataValue.ToSharedRef(), FString(), Writer))
	{
		TryDelete(Path);
		return false;
	}

	OutPayload = MoveTemp(Serialized);
	return true;
}

void FFlockSnapshotStore::DeleteScope(const FString& Scope)
{
	const FString Path = FPaths::Combine(Root, SanitizeScope(Scope));
	if (IFileManager::Get().DirectoryExists(*Path))
	{
		IFileManager::Get().DeleteDirectory(*Path, /*RequireExists*/ false, /*Tree*/ true);
	}
}

void FFlockSnapshotStore::PruneOtherVersions(const FString& KeepGameVersionId)
{
	if (KeepGameVersionId.IsEmpty() || !IFileManager::Get().DirectoryExists(*Root))
	{
		return;
	}

	const FString Keep = Sanitize(KeepGameVersionId);
	TArray<FString> Directories;
	IFileManager::Get().FindFiles(Directories, *(Root / TEXT("*")), /*Files*/ false, /*Directories*/ true);
	for (const FString& Dir : Directories)
	{
		if (Dir.Equals(Keep, ESearchCase::CaseSensitive) || Dir.Equals(BootstrapScope, ESearchCase::CaseSensitive))
		{
			continue;
		}
		IFileManager::Get().DeleteDirectory(*FPaths::Combine(Root, Dir), /*RequireExists*/ false, /*Tree*/ true);
	}
}

FString FFlockSnapshotStore::BuildPath(const FString& Scope, const FString& Key) const
{
	return FPaths::Combine(Root, SanitizeScope(Scope), FString::Printf(TEXT("%s_%s%s"), *Sanitize(Key), *Hash8(Key), SnapshotExtension));
}

FString FFlockSnapshotStore::SanitizeScope(const FString& Scope)
{
	// Each path segment is sanitized independently so the "/" separators in a "<version>/<category>" scope
	// survive as real directory boundaries.
	TArray<FString> Segments;
	Scope.ParseIntoArray(Segments, TEXT("/"), /*CullEmpty*/ true);
	FString Result;
	for (const FString& Segment : Segments)
	{
		Result = Result.IsEmpty() ? Sanitize(Segment) : FPaths::Combine(Result, Sanitize(Segment));
	}
	return Result.IsEmpty() ? Sanitize(Scope) : Result;
}

FString FFlockSnapshotStore::Sanitize(const FString& Id)
{
	if (Id.IsEmpty())
	{
		return TEXT("_");
	}

	FString Result;
	Result.Reserve(Id.Len());
	for (const TCHAR Char : Id)
	{
		const bool bSafe = (Char >= 'a' && Char <= 'z') || (Char >= 'A' && Char <= 'Z')
			|| (Char >= '0' && Char <= '9') || Char == '-' || Char == '_';
		Result.AppendChar(bSafe ? Char : TEXT('_'));
	}
	// Cap the readable prefix; the hash appended by BuildPath keeps distinct keys apart past the cap.
	return Result.Len() <= 64 ? Result : Result.Left(64);
}

FString FFlockSnapshotStore::Hash8(const FString& Key)
{
	// SHA-1 over the UTF-8 key, first 4 bytes as hex: disambiguates keys that sanitize to the same prefix.
	const FTCHARToUTF8 Utf8(*Key);
	FSHA1 Sha;
	Sha.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Sha.Final();
	uint8 Digest[FSHA1::DigestSize];
	Sha.GetHash(Digest);
	return FString::Printf(TEXT("%02x%02x%02x%02x"), Digest[0], Digest[1], Digest[2], Digest[3]);
}

void FFlockSnapshotStore::TryDelete(const FString& Path)
{
	if (IFileManager::Get().FileExists(*Path))
	{
		IFileManager::Get().Delete(*Path, /*RequireExists*/ false, /*EvenReadOnly*/ true);
	}
}
