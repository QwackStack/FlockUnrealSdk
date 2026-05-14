#include "Qwack_ue_Sdk/Session/FlockSessionStore.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	constexpr const TCHAR* ActiveFileName = TEXT("active.json");
	constexpr const TCHAR* PendingSubDir = TEXT("pending");
	constexpr const TCHAR* PendingExt = TEXT(".json");
}

FFlockSessionStore::FFlockSessionStore(const FString& InRoot)
	: Root(InRoot)
{
	IFileManager::Get().MakeDirectory(*Root, /*Tree=*/true);
	IFileManager::Get().MakeDirectory(*PendingDir(), /*Tree=*/true);
}

FString FFlockSessionStore::ActivePath() const
{
	return Root / ActiveFileName;
}

FString FFlockSessionStore::PendingDir() const
{
	return Root / PendingSubDir;
}

FString FFlockSessionStore::PendingPathFor(const FString& Filename) const
{
	return PendingDir() / Filename;
}

bool FFlockSessionStore::WriteActive(const FFlockSessionMarker& Marker) const
{
	return WriteJsonAtomic(ActivePath(), Marker);
}

bool FFlockSessionStore::TryReadActive(FFlockSessionMarker& OutMarker) const
{
	const FString Path = ActivePath();
	if (!IFileManager::Get().FileExists(*Path))
	{
		return false;
	}
	return ReadJsonFile(Path, OutMarker);
}

void FFlockSessionStore::DeleteActive() const
{
	IFileManager::Get().Delete(*ActivePath(), /*bRequireExists=*/false, /*bEvenReadOnly=*/false, /*bQuiet=*/true);
}

FString FFlockSessionStore::RotateActiveToPending() const
{
	const FString Src = ActivePath();
	if (!IFileManager::Get().FileExists(*Src))
	{
		return FString();
	}

	// Zero-padded ticks + GUID — lexical sort = chronological.
	const FString Name = FString::Printf(TEXT("%020lld_%s%s"),
		FDateTime::UtcNow().GetTicks(),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits),
		PendingExt);
	const FString Dst = PendingPathFor(Name);

	if (!IFileManager::Get().Move(*Dst, *Src, /*bReplace=*/true))
	{
		return FString();
	}
	return Name;
}

// =====================================================================
// Pending markers
// =====================================================================

TArray<FString> FFlockSessionStore::ListPending() const
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(PendingDir() / FString(TEXT("*")) + PendingExt), /*Files=*/true, /*Directories=*/false);
	Files.Sort();
	return Files;
}

bool FFlockSessionStore::TryReadPending(const FString& Filename, FFlockSessionMarker& OutMarker) const
{
	return ReadJsonFile(PendingPathFor(Filename), OutMarker);
}

void FFlockSessionStore::DeletePending(const FString& Filename) const
{
	IFileManager::Get().Delete(*PendingPathFor(Filename), /*bRequireExists=*/false, /*bEvenReadOnly=*/false, /*bQuiet=*/true);
}

bool FFlockSessionStore::WriteJsonAtomic(const FString& AbsPath, const FFlockSessionMarker& Marker)
{
	const FString Body = SerializeMarker(Marker);
	const FString TmpAbs = AbsPath + TEXT(".tmp");

	// UE's default is UTF-16 with BOM, which JSON parsers reject.
	if (!FFileHelper::SaveStringToFile(Body, *TmpAbs, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return false;
	}

	if (!IFileManager::Get().Move(*AbsPath, *TmpAbs, /*bReplace=*/true))
	{
		IFileManager::Get().Delete(*TmpAbs, /*bRequireExists=*/false, /*bEvenReadOnly=*/false, /*bQuiet=*/true);
		return false;
	}
	return true;
}

bool FFlockSessionStore::ReadJsonFile(const FString& AbsPath, FFlockSessionMarker& OutMarker)
{
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *AbsPath))
	{
		return false;
	}
	return DeserializeMarker(Json, OutMarker);
}

FString FFlockSessionStore::SerializeMarker(const FFlockSessionMarker& Marker)
{
	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("session_id"), Marker.SessionId);
	Obj->SetStringField(TEXT("player_id"), Marker.PlayerId);
	Obj->SetStringField(TEXT("game_version_id"), Marker.GameVersionId);
	Obj->SetStringField(TEXT("started_at"), Marker.StartedAt.ToIso8601());
	Obj->SetStringField(TEXT("last_seen_at"), Marker.LastSeenAt.ToIso8601());
	Obj->SetNumberField(TEXT("screens_viewed"), Marker.ScreensViewed);

	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Obj, Writer);
	return Out;
}

bool FFlockSessionStore::DeserializeMarker(const FString& Json, FFlockSessionMarker& OutMarker)
{
	if (Json.IsEmpty()) return false;

	TSharedPtr<FJsonObject> Obj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		return false;
	}

	FFlockSessionMarker M;
	Obj->TryGetStringField(TEXT("session_id"), M.SessionId);
	Obj->TryGetStringField(TEXT("player_id"), M.PlayerId);
	Obj->TryGetStringField(TEXT("game_version_id"), M.GameVersionId);

	FString StartedIso, LastSeenIso;
	if (Obj->TryGetStringField(TEXT("started_at"), StartedIso))
	{
		FDateTime::ParseIso8601(*StartedIso, M.StartedAt);
	}
	if (Obj->TryGetStringField(TEXT("last_seen_at"), LastSeenIso))
	{
		FDateTime::ParseIso8601(*LastSeenIso, M.LastSeenAt);
	}

	int32 Screens = 0;
	if (Obj->TryGetNumberField(TEXT("screens_viewed"), Screens))
	{
		M.ScreensViewed = Screens;
	}

	// No session_id = nothing to address on the server.
	if (M.SessionId.IsEmpty())
	{
		return false;
	}

	OutMarker = M;
	return true;
}
