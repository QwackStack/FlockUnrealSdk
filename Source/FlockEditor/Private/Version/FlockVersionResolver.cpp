// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Version/FlockVersionResolver.h"
#include "Version/FlockVersionLookup.h"
#include "Config/FlockConfig.h"
#include "FlockSubsystem.h"
#include "FlockEditor.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "FlockEditor"

namespace
{
	/** Percent-encodes a URL path segment (RFC 3986 unreserved kept). */
	FString EncodePathSegment(const FString& In)
	{
		FString Result;
		const FTCHARToUTF8 Utf8(*In);
		const uint8* Bytes = reinterpret_cast<const uint8*>(Utf8.Get());
		const int32 Len = Utf8.Length();
		for (int32 Index = 0; Index < Len; ++Index)
		{
			const uint8 Byte = Bytes[Index];
			const bool bUnreserved =
				(Byte >= 'A' && Byte <= 'Z') || (Byte >= 'a' && Byte <= 'z') ||
				(Byte >= '0' && Byte <= '9') || Byte == '-' || Byte == '_' || Byte == '.' || Byte == '~';
			if (bUnreserved)
			{
				Result.AppendChar(static_cast<TCHAR>(Byte));
			}
			else
			{
				Result += FString::Printf(TEXT("%%%02X"), Byte);
			}
		}
		return Result;
	}

	void ShowResultToast(const FText& Message, bool bSuccess)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = bSuccess ? 5.0f : 7.0f;
		const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}
}

FString FFlockVersionResolver::VersionedUrl(const FString& ApiUrl, const FString& Path)
{
	FString Base = ApiUrl.TrimStartAndEnd();
	while (Base.EndsWith(TEXT("/")))
	{
		Base.LeftChopInline(1);
	}
	return FString::Printf(TEXT("%s/%s/%s"), *Base, *UFlockSubsystem::ApiVersion, *Path);
}

FString FFlockVersionResolver::ByNameUrl(const FString& ApiUrl, const FString& GameVersion)
{
	return VersionedUrl(ApiUrl, FString::Printf(TEXT("game_version/by-name/%s"), *EncodePathSegment(GameVersion)));
}

bool FFlockVersionResolver::ApplyResult(UFlockConfig& Config, const FFlockResolveResult& Result)
{
	if (!Result.bSuccess || Result.GameVersionId.IsEmpty())
	{
		return false;
	}
	if (Config.GameVersionId.Equals(Result.GameVersionId, ESearchCase::CaseSensitive))
	{
		return false;
	}

	Config.GameVersionId = Result.GameVersionId;
	return true;
}

void FFlockVersionResolver::ResolveAndBake(UFlockConfig& Config)
{
	// The config is the UFlockConfig CDO (process-lifetime), so a raw pointer capture is safe.
	UFlockConfig* ConfigPtr = &Config;

	FFlockVersionLookupRegistry::Get().Resolve(Config.ApiUrl, Config.ApiKey, Config.GameVersion,
		FFlockResolveComplete::CreateLambda([ConfigPtr](const FFlockResolveResult& Result)
		{
			if (!Result.bSuccess)
			{
				UE_LOG(LogFlockEditor, Warning, TEXT("Flock resolve failed: %s"), *Result.Error);
				ShowResultToast(FText::Format(
					LOCTEXT("ResolveFailed", "Flock: could not resolve Game Version.\n{0}"),
					FText::FromString(Result.Error)), /*bSuccess*/ false);
				return;
			}

			if (FFlockVersionResolver::ApplyResult(*ConfigPtr, Result))
			{
				ConfigPtr->TryUpdateDefaultConfigFile();
			}

			UE_LOG(LogFlockEditor, Log, TEXT("Flock Game Version resolved and baked: %s"), *Result.GameVersionId);
			ShowResultToast(FText::Format(
				LOCTEXT("ResolveOk", "Flock: Game Version ID baked.\n{0}"),
				FText::FromString(Result.GameVersionId)), /*bSuccess*/ true);
		}));
}

#undef LOCTEXT_NAMESPACE
