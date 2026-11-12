// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Analytics/FlockDefaultEvents.h"

#include "Dom/JsonObject.h"
#include "GeneralProjectSettings.h"
#include "Http/FlockJsonUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockTemporaryFiles.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const TCHAR* const FirstLaunchRecordedField = TEXT("first_launch_recorded");
	const TCHAR* const LastAppVersionField = TEXT("last_app_version");

	FString Condensed(const TSharedRef<FJsonObject>& Object)
	{
		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Object, Writer);
		return Json;
	}

	/** Set unless the event has the key already, in any letter case, or the value is empty. */
	void AddUnlessSet(const TSharedRef<FJsonObject>& Object, const TArray<FString>& GameKeys, const TCHAR* Key,
		const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || GameKeys.ContainsByPredicate([Key](const FString& Existing) { return Existing.Equals(Key, ESearchCase::IgnoreCase); }))
		{
			return;
		}
		Object->SetField(Key, Value);
	}

	TSharedPtr<FJsonValue> StringOrNull(const FString& Value)
	{
		return Value.IsEmpty() ? TSharedPtr<FJsonValue>() : FFlockCommandValue(Value).ToJsonValue();
	}
}

bool FlockDefaultEvents::IsOneOfTheSdksOwnEvents(const FString& EventName)
{
	for (const FString& Name : Names())
	{
		if (Name.Equals(EventName, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

TArray<FString> FlockDefaultEvents::Names()
{
	return { SessionEnd, AppOpen, FirstLaunch, AppUpdate, StorePurchase, ScreenView };
}

FString FlockDefaultEvents::AppVersionOfThisGame()
{
	const UGeneralProjectSettings* Project = GetDefault<UGeneralProjectSettings>();
	return Project != nullptr ? Project->ProjectVersion : FString();
}

void FlockDefaultEvents::AddParameters(FFlockCommandData& Properties, const FFlockDeviceInfo& Device, const TOptional<int32>& InSessionNumber)
{
	const TSharedRef<FJsonObject> Object = Properties.ToJsonObject();
	const TArray<FString> GameKeys = FFlockJsonUtils::GetFieldNames(Object);
	AddUnlessSet(Object, GameKeys, Platform, StringOrNull(Device.Platform));
	AddUnlessSet(Object, GameKeys, OperatingSystem, StringOrNull(Device.OperatingSystem));
	AddUnlessSet(Object, GameKeys, DeviceModel, StringOrNull(Device.DeviceModel));
	AddUnlessSet(Object, GameKeys, AppVersion, StringOrNull(Device.AppVersion));
	AddUnlessSet(Object, GameKeys, Language, StringOrNull(Device.SystemLanguage));
	AddUnlessSet(Object, GameKeys, SdkVersion, StringOrNull(Device.SdkVersion));
	if (InSessionNumber.IsSet())
	{
		AddUnlessSet(Object, GameKeys, SessionNumber, FFlockCommandValue(InSessionNumber.GetValue()).ToJsonValue());
	}
	Properties = FFlockCommandData::FromJsonString(Condensed(Object));
}

FString FlockDefaultEvents::ReasonName(EFlockSessionEndReason Reason)
{
	switch (Reason)
	{
	case EFlockSessionEndReason::Logout: return SignOut;
	case EFlockSessionEndReason::Timeout: return Timeout;
	case EFlockSessionEndReason::Quit: return Quit;
	case EFlockSessionEndReason::Restarted: return Restarted;
	case EFlockSessionEndReason::Manual: return EndedByTheGame;
	}
	return EndedByTheGame;
}

int32 FlockDefaultEvents::WholeSeconds(float Seconds)
{
	return FMath::RoundToInt(FMath::Max(0.f, Seconds));
}

FFlockCommandData FlockDefaultEvents::SessionEndProperties(const FFlockSessionSnapshot& Snapshot, const FString& Reason)
{
	FFlockCommandData Properties;
	Properties.Set(TEXT("reason"), Reason);
	Properties.Set(TEXT("duration_seconds"), WholeSeconds(Snapshot.DurationSeconds));
	Properties.Set(TEXT("screens_viewed"), Snapshot.ScreensViewed);
	Properties.Set(TEXT("is_bounce"), Snapshot.IsBounce);
	Properties.Set(TEXT("session_number"), Snapshot.SessionNumber);
	Properties.Set(TEXT("is_first_session"), Snapshot.IsFirstSession);
	Properties.Set(TEXT("pause_count"), Snapshot.PauseCount);
	// Only when frames were sampled: Track FPS off, or a session too short for a sample, has no frame rate.
	if (Snapshot.AverageFps > 0.f)
	{
		Properties.Set(TEXT("average_fps"), FMath::RoundHalfToEven(static_cast<double>(Snapshot.AverageFps) * 10.0) / 10.0);
		Properties.Set(TEXT("min_fps"), FMath::RoundHalfToEven(static_cast<double>(Snapshot.MinFps) * 10.0) / 10.0);
	}
	return Properties;
}

FlockDefaultEvents::FLaunchRecord FlockDefaultEvents::FLaunchRecord::Load(const FString& Path)
{
	FLaunchRecord Record;
	FString Json;
	TSharedPtr<FJsonObject> Root;
	if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Json, *Path) || !FFlockJsonUtils::TryParseObject(Json, Root) || !Root.IsValid())
	{
		return Record;
	}
	Root->TryGetBoolField(FirstLaunchRecordedField, Record.bFirstLaunchRecorded);
	Root->TryGetStringField(LastAppVersionField, Record.LastAppVersion);
	return Record;
}

void FlockDefaultEvents::FLaunchRecord::Save(const FString& Path) const
{
	if (Path.IsEmpty())
	{
		return;
	}
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(FirstLaunchRecordedField, bFirstLaunchRecorded);
	Root->SetStringField(LastAppVersionField, LastAppVersion);
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(Path));
	FFlockTemporaryFiles::SaveThenMove(Condensed(Root), Path);
}
