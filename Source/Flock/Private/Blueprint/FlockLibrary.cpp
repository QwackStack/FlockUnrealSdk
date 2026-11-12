// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockLibrary.h"

#include "Analytics/FlockGameplayEvents.h"
#include "FlockEvents.h"
#include "FlockSubsystem.h"
#include "Providers/FlockAuthProvider.h"

// Every node resolves the subsystem from the calling graph's world context and forwards. A missing SDK
// (no context, or before init) is a safe no-op / default, matching the subsystem methods themselves.

void UFlockLibrary::LogDiagnosticEvent(const UObject* WorldContextObject, const FString& Message,
	const TMap<FString, FString>& ExtraData)
{
	if (UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject))
	{
		Sdk->LogDiagnosticEvent(Message, ExtraData);
	}
}

void UFlockLibrary::LogDiagnosticError(const UObject* WorldContextObject, const FString& Message, const FFlockLogDetails& Details)
{
	if (UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject))
	{
		Sdk->LogDiagnosticError(Message, Details);
	}
}

void UFlockLibrary::LogDiagnosticException(const UObject* WorldContextObject, const FString& Message, const FString& StackTrace,
	const FFlockLogDetails& Details)
{
	if (UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject))
	{
		Sdk->LogDiagnosticException(Message, StackTrace, Details);
	}
}

// The former names forward to the new ones rather than to the subsystem, so a graph still calling one
// takes exactly the path the renamed node takes.
void UFlockLibrary::LogEvent(const UObject* WorldContextObject, const FString& Message, const TMap<FString, FString>& ExtraData)
{
	LogDiagnosticEvent(WorldContextObject, Message, ExtraData);
}

void UFlockLibrary::LogError(const UObject* WorldContextObject, const FString& Message, const FFlockLogDetails& Details)
{
	LogDiagnosticError(WorldContextObject, Message, Details);
}

void UFlockLibrary::LogException(const UObject* WorldContextObject, const FString& Message, const FString& StackTrace,
	const FFlockLogDetails& Details)
{
	LogDiagnosticException(WorldContextObject, Message, StackTrace, Details);
}

void UFlockLibrary::RecordScreenView(const UObject* WorldContextObject, const FString& ScreenName)
{
	if (UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject))
	{
		Sdk->RecordAnalyticsScreenView(ScreenName);
	}
}

bool UFlockLibrary::TrackEvent(const UObject* WorldContextObject, const FString& EventName,
	const FFlockCommandData& Properties, const FString& EventCategory)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk && Sdk->TrackAnalyticsEvent(EventName, Properties, EventCategory);
}

bool UFlockLibrary::TrackLevelStart(const UObject* WorldContextObject, int32 Level, const FString& Difficulty,
	const FFlockCommandData& ExtraProperties)
{
	return FlockGameplayEvents::TrackLevelStartFromBlueprint(UFlockSubsystem::Get(WorldContextObject), Level, Difficulty,
		ExtraProperties);
}

bool UFlockLibrary::TrackLevelComplete(const UObject* WorldContextObject, int32 Level, int32 Score, int32 DurationSeconds,
	int32 Stars, const FFlockCommandData& ExtraProperties)
{
	return FlockGameplayEvents::TrackLevelCompleteFromBlueprint(UFlockSubsystem::Get(WorldContextObject), Level, Score,
		DurationSeconds, Stars, ExtraProperties);
}

bool UFlockLibrary::TrackLevelFail(const UObject* WorldContextObject, int32 Level, const FString& Reason, int32 DurationSeconds,
	const FFlockCommandData& ExtraProperties)
{
	return FlockGameplayEvents::TrackLevelFailFromBlueprint(UFlockSubsystem::Get(WorldContextObject), Level, Reason,
		DurationSeconds, ExtraProperties);
}

bool UFlockLibrary::TrackCheckpoint(const UObject* WorldContextObject, int32 Level, const FString& Checkpoint,
	const FFlockCommandData& ExtraProperties)
{
	return FlockGameplayEvents::TrackCheckpointFromBlueprint(UFlockSubsystem::Get(WorldContextObject), Level, Checkpoint,
		ExtraProperties);
}

bool UFlockLibrary::TrackTutorialStep(const UObject* WorldContextObject, int32 Step, const FString& StepName,
	const FFlockCommandData& ExtraProperties)
{
	return FlockGameplayEvents::TrackTutorialStepFromBlueprint(UFlockSubsystem::Get(WorldContextObject), Step, StepName,
		ExtraProperties);
}

bool UFlockLibrary::TrackSettingsChange(const UObject* WorldContextObject, const FString& Setting, const FString& Value,
	const FFlockCommandData& ExtraProperties)
{
	return FlockGameplayEvents::TrackSettingsChangeFromBlueprint(UFlockSubsystem::Get(WorldContextObject), Setting, Value,
		ExtraProperties);
}

bool UFlockLibrary::TrackMatchStart(const UObject* WorldContextObject, const FString& Mode, const FString& Map, int32 TeamSize,
	const FFlockCommandData& ExtraProperties)
{
	return FlockGameplayEvents::TrackMatchStartFromBlueprint(UFlockSubsystem::Get(WorldContextObject), Mode, Map, TeamSize,
		ExtraProperties);
}

bool UFlockLibrary::TrackMatchComplete(const UObject* WorldContextObject, const FString& Mode, const FString& Result,
	int32 Score, int32 DurationSeconds, int32 Kills, const FFlockCommandData& ExtraProperties)
{
	return FlockGameplayEvents::TrackMatchCompleteFromBlueprint(UFlockSubsystem::Get(WorldContextObject), Mode, Result, Score,
		DurationSeconds, Kills, ExtraProperties);
}

FString UFlockLibrary::MatchResultWin()
{
	return FlockMatchResult::Win;
}

FString UFlockLibrary::MatchResultLoss()
{
	return FlockMatchResult::Loss;
}

FString UFlockLibrary::MatchResultDraw()
{
	return FlockMatchResult::Draw;
}

FFlockExceptionCaptureCoverage UFlockLibrary::GetExceptionCaptureCoverage(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk ? Sdk->GetExceptionCaptureCoverage() : FFlockExceptionCaptureCoverage();
}

void UFlockLibrary::SetAnalyticsConsent(const UObject* WorldContextObject, bool bGranted)
{
	if (UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject))
	{
		Sdk->SetAnalyticsConsent(bGranted);
	}
}

bool UFlockLibrary::HasAnalyticsConsent(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk && Sdk->HasAnalyticsConsent();
}

bool UFlockLibrary::HasActiveAnalyticsSession(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk && Sdk->HasActiveAnalyticsSession();
}

FString UFlockLibrary::GetAnalyticsSessionId(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk ? Sdk->GetAnalyticsSessionId() : FString();
}

FFlockSessionSnapshot UFlockLibrary::GetAnalyticsSnapshot(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk ? Sdk->GetAnalyticsSnapshot() : FFlockSessionSnapshot();
}

void UFlockLibrary::EraseLocalAnalyticsData(const UObject* WorldContextObject)
{
	if (UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject))
	{
		Sdk->EraseLocalAnalyticsData();
	}
}

int32 UFlockLibrary::GetPendingCommandCount(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk ? Sdk->GetPendingCommandCount() : 0;
}

bool UFlockLibrary::IsAuthenticated(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk && Sdk->IsAuthenticated();
}

FString UFlockLibrary::GetPlayerId(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk ? Sdk->GetPlayerId() : FString();
}

FString UFlockLibrary::GetThisDeviceId(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	const FFlockAuthProvider* Auth = Sdk && Sdk->IsInitialized() ? Sdk->GetAuthProvider() : nullptr;
	return Auth ? Auth->ThisDeviceId() : FString();
}

bool UFlockLibrary::IsRestoringSession(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk && Sdk->IsRestoringSession();
}

void UFlockLibrary::Logout(const UObject* WorldContextObject)
{
	if (UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject))
	{
		Sdk->Logout();
	}
}

bool UFlockLibrary::IsInitialized(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk && Sdk->IsInitialized();
}

FString UFlockLibrary::GetInitializationError(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk ? Sdk->GetInitializationError() : FString();
}

FString UFlockLibrary::GetGameId(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk ? Sdk->GetGameId() : FString();
}

FString UFlockLibrary::GetGameVersionId(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk ? Sdk->GetGameVersionId() : FString();
}

FString UFlockLibrary::GetApiUrl(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk ? Sdk->GetApiUrl() : FString();
}

FString UFlockLibrary::GetVersionedApiUrl(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk ? Sdk->GetVersionedApiUrl() : FString();
}

UFlockEvents* UFlockLibrary::GetEvents(const UObject* WorldContextObject)
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	return Sdk ? Sdk->GetEvents() : nullptr;
}
