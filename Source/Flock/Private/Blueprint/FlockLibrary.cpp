// Copyright 2022, Qwacks. All Rights Reserved.

#include "Blueprint/FlockLibrary.h"

#include "FlockEvents.h"
#include "FlockSubsystem.h"

// Every node resolves the subsystem from the calling graph's world context and forwards. A missing SDK
// (no context, or before init) is a safe no-op / default, matching the subsystem methods themselves.

void UFlockLibrary::LogEvent(const UObject* WorldContextObject, const FString& Message, const TMap<FString, FString>& ExtraData)
{
	if (UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject))
	{
		Sdk->LogAnalyticsEvent(Message, ExtraData);
	}
}

void UFlockLibrary::LogError(const UObject* WorldContextObject, const FString& Message, const FFlockLogDetails& Details)
{
	if (UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject))
	{
		Sdk->LogAnalyticsError(Message, Details);
	}
}

void UFlockLibrary::LogException(const UObject* WorldContextObject, const FString& Message, const FString& StackTrace,
	const FFlockLogDetails& Details)
{
	if (UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject))
	{
		Sdk->LogAnalyticsException(Message, StackTrace, Details);
	}
}

void UFlockLibrary::RecordScreenView(const UObject* WorldContextObject, const FString& ScreenName)
{
	if (UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject))
	{
		Sdk->RecordAnalyticsScreenView(ScreenName);
	}
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
