// Copyright 2022, Qwacks. All Rights Reserved.

#include "Analytics/FlockTerminationTracker.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"

const TCHAR* FFlockTerminationTracker::EventName = TEXT("app_termination");
const TCHAR* FFlockTerminationTracker::StateForeground = TEXT("foreground");
const TCHAR* FFlockTerminationTracker::StateBackground = TEXT("background");
const TCHAR* FFlockTerminationTracker::ClassBackgroundKill = TEXT("background_kill");
const TCHAR* FFlockTerminationTracker::ClassAbnormal = TEXT("abnormal");

namespace
{
	const TCHAR* FieldLastState = TEXT("last_state");
	const TCHAR* FieldSessionId = TEXT("session_id");
	const TCHAR* FieldServerSessionId = TEXT("server_session_id");
	const TCHAR* FieldPlayerId = TEXT("player_id");
	const TCHAR* FieldLastAliveUtc = TEXT("last_alive_utc");
	const TCHAR* FieldExceptionCount = TEXT("exception_count");
	const TCHAR* FieldAppVersion = TEXT("app_version");
	const TCHAR* FieldSdkVersion = TEXT("sdk_version");
}

FFlockTerminationTracker::FFlockTerminationTracker(bool bInEnabled, const FString& InMarkerPath, FClock InClock)
	: bEnabled(bInEnabled)
	, MarkerPath(InMarkerPath.IsEmpty() ? DefaultMarkerPath() : InMarkerPath)
	, Clock(InClock ? MoveTemp(InClock) : FClock([]() { return FDateTime::UtcNow(); }))
{
}

FString FFlockTerminationTracker::DefaultMarkerPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Flock"), TEXT("analytics"), TEXT("termination_marker.json"));
}

FString FFlockTerminationTracker::Classify(const FFlockTerminationMarker& InMarker)
{
	if (!InMarker.IsValid())
	{
		return FString();
	}
	return InMarker.LastState == StateBackground ? ClassBackgroundKill : ClassAbnormal;
}

bool FFlockTerminationTracker::ReadSurvivingMarker(FFlockTerminationMarker& OutMarker) const
{
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *MarkerPath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		// Unparseable: drop it so it cannot be re-read on every future launch.
		ClearMarker();
		return false;
	}

	FFlockTerminationMarker Parsed;
	Root->TryGetStringField(FieldLastState, Parsed.LastState);
	Root->TryGetStringField(FieldSessionId, Parsed.SessionId);
	Root->TryGetStringField(FieldServerSessionId, Parsed.ServerSessionId);
	Root->TryGetStringField(FieldPlayerId, Parsed.PlayerId);
	Root->TryGetStringField(FieldAppVersion, Parsed.AppVersion);
	Root->TryGetStringField(FieldSdkVersion, Parsed.SdkVersion);
	Root->TryGetNumberField(FieldExceptionCount, Parsed.ExceptionCount);

	FString LastAlive;
	if (Root->TryGetStringField(FieldLastAliveUtc, LastAlive) && !LastAlive.IsEmpty())
	{
		FDateTime::ParseIso8601(*LastAlive, Parsed.LastAliveUtc);
	}

	if (!Parsed.IsValid())
	{
		// Well-formed JSON but no session to attribute it to — same treatment as corrupt.
		ClearMarker();
		return false;
	}

	OutMarker = MoveTemp(Parsed);
	return true;
}

void FFlockTerminationTracker::BeginTracking(const FFlockTerminationMarker& Seed)
{
	if (!bEnabled)
	{
		return;
	}

	Marker = Seed;
	Marker.LastState = StateForeground;
	Marker.LastAliveUtc = Clock();
	Marker.ExceptionCount = Seed.ExceptionCount;
	PendingExceptionCount = 0;
	bTracking = true;
	WriteMarker();
}

void FFlockTerminationTracker::SetServerSessionId(const FString& InServerSessionId)
{
	if (!bTracking)
	{
		return;
	}
	Marker.ServerSessionId = InServerSessionId;
	WriteMarker();
}

void FFlockTerminationTracker::SetBackgrounded(bool bBackgrounded)
{
	if (!bTracking)
	{
		return;
	}
	// The state must be on disk BEFORE the app can be evicted, which is the whole point of writing
	// on the transition rather than on the next heartbeat.
	Marker.LastState = bBackgrounded ? StateBackground : StateForeground;
	Marker.LastAliveUtc = Clock();
	Marker.ExceptionCount += PendingExceptionCount;
	PendingExceptionCount = 0;
	WriteMarker();
}

void FFlockTerminationTracker::NoteException()
{
	if (!bTracking)
	{
		return;
	}
	// Deliberately not written through here: an exception storm must not become a write storm.
	++PendingExceptionCount;
}

void FFlockTerminationTracker::HandleHeartbeat()
{
	if (!bTracking)
	{
		return;
	}
	Marker.LastAliveUtc = Clock();
	Marker.ExceptionCount += PendingExceptionCount;
	PendingExceptionCount = 0;
	WriteMarker();
}

void FFlockTerminationTracker::StopTracking()
{
	if (!bTracking)
	{
		return;
	}
	bTracking = false;
	Marker = FFlockTerminationMarker();
	PendingExceptionCount = 0;
	ClearMarker();
}

void FFlockTerminationTracker::ClearMarker() const
{
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*MarkerPath);
}

void FFlockTerminationTracker::WriteMarker() const
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(FieldLastState, Marker.LastState);
	Root->SetStringField(FieldSessionId, Marker.SessionId);
	Root->SetStringField(FieldServerSessionId, Marker.ServerSessionId);
	Root->SetStringField(FieldPlayerId, Marker.PlayerId);
	Root->SetStringField(FieldLastAliveUtc, Marker.LastAliveUtc.ToIso8601());
	Root->SetNumberField(FieldExceptionCount, Marker.ExceptionCount);
	Root->SetStringField(FieldAppVersion, Marker.AppVersion);
	Root->SetStringField(FieldSdkVersion, Marker.SdkVersion);

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);

	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(MarkerPath));
	// Log-and-continue: failing to leave a tombstone costs one crash report, never the game.
	FFileHelper::SaveStringToFile(Json, *MarkerPath);
}
