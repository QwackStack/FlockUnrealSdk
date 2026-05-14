#include "QwackContextSubsystem.h"

#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Qwack_ue_Sdk/Auth/QwackAuthSubsystem.h"
#include "Qwack_ue_Sdk/Config/QwackConfigSubsystem.h"

namespace
{
	// Bumped manually with releases; surfaces in `sdk_version` on every event.
	constexpr const TCHAR* kSdkVersion = TEXT("1.0.0");
	FString InstallIdFilePath()
	{
		return FPaths::ProjectSavedDir() / TEXT("Flock") / TEXT("install_id.txt");
	}

	const TCHAR* DeviceTypeForPlatform()
	{
#if PLATFORM_IOS || PLATFORM_ANDROID
		return TEXT("mobile");
#elif PLATFORM_DESKTOP
		return TEXT("desktop");
#else
		return TEXT("console");
#endif
	}
}

void UQwackContextSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	SessionStartUtc = FDateTime::UtcNow();
	SnapshotPlatform();
	LoadOrCreateInstallId();

	// Desktop focus loss and mobile background both subtract from gameplay_time.
	// The bInBackground guard makes overlap between the pairs safe.
	DeactivateHandle = FCoreDelegates::ApplicationWillDeactivateDelegate.AddUObject(this, &UQwackContextSubsystem::OnEnterBackground);
	ReactivateHandle = FCoreDelegates::ApplicationHasReactivatedDelegate.AddUObject(this, &UQwackContextSubsystem::OnEnterForeground);
	BackgroundHandle = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(this, &UQwackContextSubsystem::OnEnterBackground);
	ForegroundHandle = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(this, &UQwackContextSubsystem::OnEnterForeground);
}

void UQwackContextSubsystem::Deinitialize()
{
	FCoreDelegates::ApplicationWillDeactivateDelegate.Remove(DeactivateHandle);
	FCoreDelegates::ApplicationHasReactivatedDelegate.Remove(ReactivateHandle);
	FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(BackgroundHandle);
	FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(ForegroundHandle);
	Super::Deinitialize();
}

void UQwackContextSubsystem::SnapshotPlatform()
{
	Platform = FString(FPlatformProperties::IniPlatformName());
	DeviceType = DeviceTypeForPlatform();
	DeviceModel = FPlatformMisc::GetDeviceMakeAndModel();
	OsVersion = FPlatformMisc::GetOSVersion();
	CpuBrand = FPlatformMisc::GetCPUBrand();
	GpuBrand = FPlatformMisc::GetPrimaryGPUBrand();
	Locale = FPlatformMisc::GetDefaultLocale();
	EngineVersionString = FEngineVersion::Current().ToString();
	SdkVersionString = kSdkVersion;
}

void UQwackContextSubsystem::LoadOrCreateInstallId()
{
	const FString Path = InstallIdFilePath();

	FString Existing;
	if (FFileHelper::LoadFileToString(Existing, *Path))
	{
		InstallId = Existing.TrimStartAndEnd();
		if (!InstallId.IsEmpty()) return;
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
	InstallId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	FFileHelper::SaveStringToFile(InstallId, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void UQwackContextSubsystem::OnEnterBackground()
{
	if (bInBackground) return;
	bInBackground = true;
	BackgroundEnterMonotonic = FPlatformTime::Seconds();
}

void UQwackContextSubsystem::OnEnterForeground()
{
	if (!bInBackground) return;
	BackgroundAccumulatedSec += FPlatformTime::Seconds() - BackgroundEnterMonotonic;
	bInBackground = false;
}

double UQwackContextSubsystem::GetGameplayTimeSeconds() const
{
	const double Elapsed = (FDateTime::UtcNow() - SessionStartUtc).GetTotalSeconds();
	double Bg = BackgroundAccumulatedSec;
	if (bInBackground)
	{
		Bg += FPlatformTime::Seconds() - BackgroundEnterMonotonic;
	}
	return FMath::Max(0.0, Elapsed - Bg);
}

void UQwackContextSubsystem::SetSessionId(const FString& InSessionId)
{
	SessionId = InSessionId;
}

void UQwackContextSubsystem::ClearSessionId()
{
	SessionId.Reset();
}

void UQwackContextSubsystem::MergeDefaults(const TSharedRef<FJsonObject>& Target) const
{
	auto SetStringIfAbsent = [&Target](const TCHAR* Key, const FString& Value)
	{
		if (Value.IsEmpty()) return;
		if (Target->HasField(Key)) return;
		Target->SetStringField(Key, Value);
	};
	auto SetNumberIfAbsent = [&Target](const TCHAR* Key, double Value)
	{
		if (Target->HasField(Key)) return;
		Target->SetNumberField(Key, Value);
	};

	FString PlayerId;
	FString GameVersion;
	if (const UGameInstance* GI = GetGameInstance())
	{
		if (const UQwackAuthSubsystem* Auth = GI->GetSubsystem<UQwackAuthSubsystem>())
		{
			PlayerId = Auth->GetPlayerId();
		}
		if (const UQwackConfigSubsystem* Config = GI->GetSubsystem<UQwackConfigSubsystem>())
		{
			GameVersion = Config->GetGameVersion();
		}
	}

	SetStringIfAbsent(TEXT("player_id"), PlayerId);
	SetStringIfAbsent(TEXT("session_id"), SessionId);
	SetStringIfAbsent(TEXT("install_id"), InstallId);

	SetStringIfAbsent(TEXT("game_version"), GameVersion);
	SetStringIfAbsent(TEXT("sdk_version"), SdkVersionString);
	SetStringIfAbsent(TEXT("engine_version"), EngineVersionString);

	SetStringIfAbsent(TEXT("platform"), Platform);
	SetStringIfAbsent(TEXT("device_type"), DeviceType);
	SetStringIfAbsent(TEXT("device_model"), DeviceModel);
	SetStringIfAbsent(TEXT("os_version"), OsVersion);
	SetStringIfAbsent(TEXT("cpu_brand"), CpuBrand);
	SetStringIfAbsent(TEXT("gpu_brand"), GpuBrand);
	SetStringIfAbsent(TEXT("locale"), Locale);

	SetStringIfAbsent(TEXT("session_start_utc"), SessionStartUtc.ToIso8601());
	SetNumberIfAbsent(TEXT("gameplay_time_sec"), GetGameplayTimeSeconds());
}
