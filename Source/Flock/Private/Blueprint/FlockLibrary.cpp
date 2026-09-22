// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockLibrary.h"

#include "FlockEvents.h"
#include "FlockSubsystem.h"

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
