// Copyright 2022, Qwacks. All Rights Reserved.

#include "Analytics/FlockSession.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const TCHAR* SessionNumberField = TEXT("session_number");

	/** Empty rather than the epoch, so "never happened" is distinguishable on the wire. */
	FString ToIsoOrEmpty(const FDateTime& Value)
	{
		return Value == FDateTime::MinValue() ? FString() : Value.ToIso8601();
	}
}

FFlockSession::FFlockSession(const FFlockAnalyticsConfig& InConfig, const FString& InStateFilePath, FClock InClock)
	: Config(InConfig)
	, StateFilePath(InStateFilePath.IsEmpty() ? DefaultStatePath() : InStateFilePath)
	, Clock(InClock ? MoveTemp(InClock) : FClock([]() { return FDateTime::UtcNow(); }))
{
	LoadSessionNumber();
}

FString FFlockSession::DefaultStatePath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Flock"), TEXT("analytics"), TEXT("session_state.json"));
}

void FFlockSession::LoadSessionNumber()
{
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *StateFilePath))
	{
		return;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		// Corrupt state just restarts the count; it is a statistic, not a correctness input.
		return;
	}

	int32 Stored = 0;
	if (Root->TryGetNumberField(SessionNumberField, Stored) && Stored > 0)
	{
		SessionNumber = Stored;
	}
}

void FFlockSession::SaveSessionNumber() const
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(SessionNumberField, SessionNumber);

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);

	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(StateFilePath));
	FFileHelper::SaveStringToFile(Json, *StateFilePath);
}

void FFlockSession::ResetAccounting()
{
	DurationSeconds = 0.f;
	TotalPauseDurationSeconds = 0.f;
	PauseCount = 0;
	ScreensViewed = 0;
	ScreenNames.Reset();
	FpsWindowSeconds = 0.f;
	FpsWindowFrames = 0;
	FpsSampleTotal = 0.f;
	FpsSampleCount = 0;
	MinFps = 0.f;
	MaxFps = 0.f;
	EndTimeUtc = FDateTime::MinValue();
	LastHeartbeatUtc = FDateTime::MinValue();
	PauseStartUtc = FDateTime::MinValue();
	bPaused = false;
}

FString FFlockSession::Start(const FString& InPlayerId)
{
	if (bActive)
	{
		return SessionId;
	}

	ResetAccounting();
	PlayerId = InPlayerId;
	ServerSessionId.Reset();
	SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	StartTimeUtc = Clock();
	++SessionNumber;
	SaveSessionNumber();
	bActive = true;
	return SessionId;
}

void FFlockSession::End()
{
	if (!bActive)
	{
		return;
	}
	// Close an open pause so the pause total is complete even when the app is killed backgrounded.
	if (bPaused)
	{
		Resume();
	}
	EndTimeUtc = Clock();
	bActive = false;
}

void FFlockSession::Tick(float DeltaSeconds)
{
	if (!bActive || bPaused || DeltaSeconds <= 0.f)
	{
		return;
	}
	DurationSeconds += DeltaSeconds;
	SampleFps(DeltaSeconds);
}

void FFlockSession::SampleFps(float DeltaSeconds)
{
	if (!Config.bTrackFps || Config.FpsSampleIntervalSeconds <= 0.f)
	{
		return;
	}

	FpsWindowSeconds += DeltaSeconds;
	++FpsWindowFrames;
	if (FpsWindowSeconds < Config.FpsSampleIntervalSeconds)
	{
		return;
	}

	const float Fps = FpsWindowFrames / FpsWindowSeconds;
	FpsSampleTotal += Fps;
	++FpsSampleCount;
	// First sample seeds both bounds; zero would otherwise pin MinFps forever.
	MinFps = FpsSampleCount == 1 ? Fps : FMath::Min(MinFps, Fps);
	MaxFps = FpsSampleCount == 1 ? Fps : FMath::Max(MaxFps, Fps);

	FpsWindowSeconds = 0.f;
	FpsWindowFrames = 0;
}

void FFlockSession::Pause()
{
	if (!bActive || bPaused)
	{
		return;
	}
	bPaused = true;
	PauseStartUtc = Clock();
	++PauseCount;
}

bool FFlockSession::Resume()
{
	if (!bActive || !bPaused)
	{
		return false;
	}
	const double AwaySeconds = (Clock() - PauseStartUtc).GetTotalSeconds();
	TotalPauseDurationSeconds += static_cast<float>(AwaySeconds);
	bPaused = false;
	PauseStartUtc = FDateTime::MinValue();

	// A timeout of zero or less means "never rotate on backgrounding".
	return Config.SessionTimeoutSeconds > 0.f && AwaySeconds >= Config.SessionTimeoutSeconds;
}

void FFlockSession::RecordScreenView(const FString& ScreenName)
{
	if (!bActive || ScreenName.IsEmpty())
	{
		return;
	}
	++ScreensViewed;
	// The count keeps rising past the cap; only the name list stops growing, so a screen-thrashing
	// session cannot grow the payload without bound.
	if (ScreenNames.Num() < MaxTrackedScreenNames)
	{
		ScreenNames.Add(ScreenName);
	}
}

void FFlockSession::MarkHeartbeat()
{
	if (!bActive)
	{
		return;
	}
	LastHeartbeatUtc = Clock();
}

bool FFlockSession::IsBounce() const
{
	return Config.BounceThresholdSeconds > 0.f && DurationSeconds < Config.BounceThresholdSeconds;
}

FFlockSessionSnapshot FFlockSession::TakeSnapshot() const
{
	FFlockSessionSnapshot Snapshot;
	Snapshot.SessionId = SessionId;
	Snapshot.ServerSessionId = ServerSessionId;
	Snapshot.PlayerId = PlayerId;
	Snapshot.SessionNumber = SessionNumber;
	Snapshot.StartTimeUtc = ToIsoOrEmpty(StartTimeUtc);
	Snapshot.EndTimeUtc = ToIsoOrEmpty(EndTimeUtc);
	Snapshot.LastHeartbeatUtc = ToIsoOrEmpty(LastHeartbeatUtc);
	Snapshot.DurationSeconds = DurationSeconds;
	Snapshot.TotalPauseDurationSeconds = TotalPauseDurationSeconds;
	Snapshot.PauseCount = PauseCount;
	Snapshot.ScreensViewed = ScreensViewed;
	Snapshot.ScreenNames = ScreenNames;
	Snapshot.AverageFps = FpsSampleCount > 0 ? FpsSampleTotal / FpsSampleCount : 0.f;
	Snapshot.MinFps = MinFps;
	Snapshot.MaxFps = MaxFps;
	Snapshot.IsActive = bActive;
	Snapshot.IsBounce = IsBounce();
	Snapshot.IsFirstSession = SessionNumber == 1;
	return Snapshot;
}
