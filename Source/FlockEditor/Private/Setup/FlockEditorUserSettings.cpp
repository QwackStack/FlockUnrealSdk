// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Setup/FlockEditorUserSettings.h"

UFlockEditorUserSettings* UFlockEditorUserSettings::Get()
{
	return GetMutableDefault<UFlockEditorUserSettings>();
}

void UFlockEditorUserSettings::MarkSeen(const FString& SdkVersion)
{
	if (LastSeenSdkVersion.Equals(SdkVersion, ESearchCase::CaseSensitive))
	{
		return;
	}
	LastSeenSdkVersion = SdkVersion;
	SaveConfig();
}

void UFlockEditorUserSettings::Suppress(FName FindingId)
{
	if (FindingId.IsNone() || SuppressedFindingIds.Contains(FindingId))
	{
		return;
	}
	SuppressedFindingIds.Add(FindingId);
	SaveConfig();
}

void UFlockEditorUserSettings::ClearSuppressions()
{
	if (SuppressedFindingIds.Num() == 0)
	{
		return;
	}
	SuppressedFindingIds.Reset();
	SaveConfig();
}
