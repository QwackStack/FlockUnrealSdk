// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Analytics/FlockSession.h"

#include "Analytics/FlockAnalyticsJson.h"
#include "Http/FlockJsonUtils.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockTemporaryFiles.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const TCHAR* SessionNumberField = TEXT("session_number");
	const TCHAR* ActiveSessionField = TEXT("active_session");
	/** Inside the open record: its session_end was recorded when it ended, so a later launch records none. */
	const TCHAR* EndEventRecordedField = TEXT("end_event_recorded");

	/** Empty rather than the epoch, so "never happened" is distinguishable on the wire. */
	FString ToIsoOrEmpty(const FDateTime& Value)
	{
		return Value == FDateTime::MinValue() ? FString() : Value.ToIso8601();
	}
}

FFlockSession::FFlockSession(const FFlockAnalyticsConfig& InConfig, const FString& InStateFilePath, FClock InClock)
	: Config(InConfig)
	, StateFilePath(InStateFilePath)
	, Clock(InClock ? MoveTemp(InClock) : FClock([]() { return FDateTime::UtcNow(); }))
{
	LoadSessionNumber();
}

void FFlockSession::ContinueSessionNumbersFrom(int32 EarlierSessionNumber)
{
	if (EarlierSessionNumber <= SessionNumber)
	{
		return;
	}
	SessionNumber = EarlierSessionNumber;

	// Written at once: the launch this count came from is about to be deleted, and this launch may never start a session of
	// its own to write it later.
	if (!bActive)
	{
		WriteState(nullptr);
		return;
	}
	const FFlockSessionSnapshot Snapshot = TakeSnapshot();
	WriteState(&Snapshot);
}

void FFlockSession::LoadSessionNumber()
{
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *StateFilePath))
	{
		return;
	}

	TSharedPtr<FJsonObject> Root;
	if (!FFlockJsonUtils::TryParseObject(Json, Root) || !Root.IsValid())
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

void FFlockSession::EnsureStateDirectory() const
{
	// The directory can only ever need creating once, and this runs on every heartbeat write.
	if (bStateDirectoryEnsured)
	{
		return;
	}
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(StateFilePath));
	bStateDirectoryEnsured = true;
}

void FFlockSession::WriteState(const FFlockSessionSnapshot* ActiveSnapshot, bool bEndEventRecorded) const
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(SessionNumberField, SessionNumber);

	if (ActiveSnapshot != nullptr)
	{
		if (const TSharedPtr<FJsonObject> Active = FFlockAnalyticsJson::SnapshotToJson(*ActiveSnapshot))
		{
			if (bEndEventRecorded)
			{
				Active->SetBoolField(EndEventRecordedField, true);
			}
			Root->SetObjectField(ActiveSessionField, Active);
		}
	}

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);

	EnsureStateDirectory();
	// Through a temporary file, so a crash mid-write never leaves a torn record the next launch cannot recover.
	FFlockTemporaryFiles::SaveThenMove(Json, StateFilePath);
}

void FFlockSession::PersistState() const
{
	if (!Config.bPersistSessionOnDisk || !bActive)
	{
		return;
	}
	const FFlockSessionSnapshot Snapshot = TakeSnapshot();
	WriteState(&Snapshot);
}

void FFlockSession::ClearPersistedSession() const
{
	WriteState(nullptr);
}

void FFlockSession::KeepEndedRecord(const FFlockSessionSnapshot& Ended, bool bEndEventRecorded) const
{
	if (!Config.bPersistSessionOnDisk || Ended.SessionId.IsEmpty())
	{
		return;
	}
	FFlockSessionSnapshot Record = Ended;
	// Open, so a later launch finds it; its end time stays, so that launch ends it when it really ended.
	Record.IsActive = true;
	WriteState(&Record, bEndEventRecorded);
}

void FFlockSession::UpdateKeptRecord(const FString& InSessionId, const FString& InServerSessionId, bool bEndEventRecorded) const
{
	if (!Config.bPersistSessionOnDisk || InSessionId.IsEmpty() || bActive)
	{
		return;
	}
	FString Json;
	TSharedPtr<FJsonObject> Root;
	const TSharedPtr<FJsonObject>* Active = nullptr;
	if (!FFileHelper::LoadFileToString(Json, *StateFilePath) || !FFlockJsonUtils::TryParseObject(Json, Root) || !Root.IsValid()
		|| !Root->TryGetObjectField(ActiveSessionField, Active) || !Active->IsValid())
	{
		return;
	}
	FFlockSessionSnapshot Kept;
	if (!FFlockAnalyticsJson::SnapshotFromJson(Active->ToSharedRef(), Kept) || !Kept.SessionId.Equals(InSessionId, ESearchCase::CaseSensitive))
	{
		return;
	}
	bool bWasRecorded = false;
	(*Active)->TryGetBoolField(EndEventRecordedField, bWasRecorded);
	if (!InServerSessionId.IsEmpty())
	{
		Kept.ServerSessionId = InServerSessionId;
	}
	WriteState(&Kept, bWasRecorded || bEndEventRecorded);
}

bool FFlockSession::RecoverOrphanedSession(FFlockSessionSnapshot& OutSnapshot, bool* bOutEndEventRecorded) const
{
	if (!Config.bPersistSessionOnDisk)
	{
		return false;
	}

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *StateFilePath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	if (!FFlockJsonUtils::TryParseObject(Json, Root) || !Root.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Active = nullptr;
	if (!Root->TryGetObjectField(ActiveSessionField, Active) || !Active->IsValid())
	{
		// The clean-exit state, and what every pre-0.8.0 state file looks like.
		return false;
	}

	FFlockSessionSnapshot Recovered;
	if (!FFlockAnalyticsJson::SnapshotFromJson(Active->ToSharedRef(), Recovered))
	{
		return false;
	}
	// A record that was already closed has been dealt with; only a still-active one was orphaned.
	if (!Recovered.IsActive || Recovered.SessionId.IsEmpty())
	{
		return false;
	}

	Recovered.IsActive = false;
	// A record kept for a session that ended holds its real end; a live one ends at its last sign of life.
	if (Recovered.EndTimeUtc.IsEmpty())
	{
		Recovered.EndTimeUtc = Recovered.LastHeartbeatUtc.IsEmpty()
			? Recovered.StartTimeUtc
			: Recovered.LastHeartbeatUtc;
	}
	// Its length is what the last saved record held, so a bounce is judged on that.
	Recovered.IsBounce = Config.BounceThresholdSeconds > 0.f && Recovered.DurationSeconds < Config.BounceThresholdSeconds;
	if (bOutEndEventRecorded != nullptr)
	{
		bool bRecorded = false;
		(*Active)->TryGetBoolField(EndEventRecordedField, bRecorded);
		*bOutEndEventRecorded = bRecorded;
	}
	OutSnapshot = Recovered;
	return true;
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
	bSavedPastBounceThreshold = false;
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
	bActive = true;

	// One write covers both halves — the bumped counter, and the live record that makes this session
	// recoverable if the run never reaches End().
	const FFlockSessionSnapshot Snapshot = TakeSnapshot();
	WriteState(Config.bPersistSessionOnDisk ? &Snapshot : nullptr);
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

	// Saved once past the bounce threshold: a heartbeat may be a minute away, and a crash before it would leave a record that
	// reads as a bounce.
	if (!bSavedPastBounceThreshold && DurationSeconds >= Config.BounceThresholdSeconds)
	{
		bSavedPastBounceThreshold = true;
		PersistState();
	}
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
	if (!bActive || ScreenName.TrimStartAndEnd().IsEmpty())
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
	Snapshot.DeviceInfo = DeviceInfo;
	return Snapshot;
}
