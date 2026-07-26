// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Every relative API path the SDK calls, rooted at UFlockSubsystem::GetVersionedApiUrl() — one place to
 * view/diff the wire surface against the backend spec; providers consume these as they land. By-name
 * builders percent-encode the dynamic segment.
 *
 * The encoder is local (Core-only) on purpose: this is a public header, and pulling in the HTTP module's
 * FGenericPlatformHttp::UrlEncode would force HTTP to be a public dependency and leak it to every consumer.
 */
namespace FlockEndpoints
{
	/** Percent-encodes a path segment (RFC 3986 unreserved kept). */
	inline FString Encode(const FString& In)
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

	// Auth — login/register
	inline constexpr const TCHAR* PlayerLogin = TEXT("player/login");
	inline constexpr const TCHAR* PlayerLoginDevice = TEXT("player/login/device");
	inline constexpr const TCHAR* PlayerLoginGoogle = TEXT("player/login/google");
	inline constexpr const TCHAR* PlayerLoginApple = TEXT("player/login/apple");
	inline constexpr const TCHAR* PlayerLoginSteam = TEXT("player/login/steam");
	inline constexpr const TCHAR* PlayerRegister = TEXT("player/register");
	inline constexpr const TCHAR* PlayerRegisterDevice = TEXT("player/register/device");
	inline constexpr const TCHAR* PlayerRegisterGoogle = TEXT("player/register/google");
	inline constexpr const TCHAR* PlayerRegisterApple = TEXT("player/register/apple");
	inline constexpr const TCHAR* PlayerRegisterSteam = TEXT("player/register/steam");

	// Auth — session & account
	inline constexpr const TCHAR* PlayerTokenRefresh = TEXT("player/token/refresh");
	inline constexpr const TCHAR* PlayerTokenRevoke = TEXT("player/token/revoke");
	inline constexpr const TCHAR* PlayerPasswordForgot = TEXT("player/password/forgot");
	inline constexpr const TCHAR* PlayerPasswordReset = TEXT("player/password/reset");
	inline constexpr const TCHAR* PlayerEmailSendVerification = TEXT("player/email/send-verification");
	inline constexpr const TCHAR* PlayerEmailVerify = TEXT("player/email/verify");
	inline FString PlayerNameAvailable(const FString& Name)
	{
		return FString::Printf(TEXT("player/name-available?name=%s"), *Encode(Name));
	}

	// Player data / templates / bans
	inline constexpr const TCHAR* PlayerData = TEXT("player_data");
	inline FString PlayerDataById(const FString& PlayerDataId) { return FString::Printf(TEXT("player_data/%s"), *PlayerDataId); }
	inline constexpr const TCHAR* PlayerTemplate = TEXT("player_template");
	inline FString PlayerTemplateById(const FString& PlayerTemplateId) { return FString::Printf(TEXT("player_template/%s"), *PlayerTemplateId); }
	inline FString PlayerTemplateByName(const FString& Name) { return FString::Printf(TEXT("player_template/by-name/%s"), *Encode(Name)); }
	inline FString PlayerTemplateData(const FString& PlayerTemplateId) { return FString::Printf(TEXT("player_template/%s/player-data"), *PlayerTemplateId); }
	inline constexpr const TCHAR* PlayerBan = TEXT("player-ban");

	// Game / versions
	inline constexpr const TCHAR* Game = TEXT("game");
	inline constexpr const TCHAR* GameVersion = TEXT("game_version");
	inline FString GameVersionByName(const FString& Name) { return FString::Printf(TEXT("game_version/by-name/%s"), *Encode(Name)); }

	// Config / patches
	inline constexpr const TCHAR* GameConfig = TEXT("game_config");
	inline constexpr const TCHAR* GameConfigVersion = TEXT("game_config/version");
	inline FString GameConfigById(const FString& ConfigId) { return FString::Printf(TEXT("game_config/%s"), *ConfigId); }
	inline FString GameConfigByName(const FString& Name) { return FString::Printf(TEXT("game_config/by-name/%s"), *Encode(Name)); }
	inline FString GameConfigPlayerFeatures(const FString& PlayerId) { return FString::Printf(TEXT("game_config/player/%s/features"), *PlayerId); }
	inline constexpr const TCHAR* GamePatch = TEXT("game_patch");
	// The path segment is a patch id, not a config id (a prior name here was misleading).
	inline FString GamePatchById(const FString& PatchId) { return FString::Printf(TEXT("game_patch/%s"), *PatchId); }
	inline FString GamePatchByConfig(const FString& ConfigId) { return FString::Printf(TEXT("game_patch/config/%s"), *ConfigId); }
	// game_config/{id}/patches is a deliberate duplicate of game_patch/config/{id} — the config provider
	// uses the latter, so this builder is intentionally absent. Don't "fill the gap" without a reason.

	// Shop / inventory
	inline constexpr const TCHAR* Shop = TEXT("shop");
	inline FString ShopById(const FString& ShopId) { return FString::Printf(TEXT("shop/%s"), *ShopId); }
	inline FString ShopByName(const FString& Name) { return FString::Printf(TEXT("shop/by-name/%s"), *Encode(Name)); }
	inline constexpr const TCHAR* ShopTransaction = TEXT("shop/transaction");
	inline FString ShopItemById(const FString& ShopItemId) { return FString::Printf(TEXT("shop_item/%s"), *ShopItemId); }
	inline FString ShopItemsByShop(const FString& ShopId) { return FString::Printf(TEXT("shop_item/shop/%s"), *ShopId); }
	inline FString PlayerInventoryByPlayer(const FString& PlayerId) { return FString::Printf(TEXT("player_inventory/player/%s"), *PlayerId); }

	// Assets
	inline constexpr const TCHAR* Asset = TEXT("asset");
	inline FString AssetById(const FString& AssetId) { return FString::Printf(TEXT("asset/%s"), *AssetId); }

	// Analytics
	inline constexpr const TCHAR* AnalyticsSessions = TEXT("analytics/sessions");
	inline FString AnalyticsSessionById(const FString& SessionId) { return FString::Printf(TEXT("analytics/sessions/%s"), *SessionId); }
	inline constexpr const TCHAR* AnalyticsEvents = TEXT("analytics/events");
	inline constexpr const TCHAR* AnalyticsEventsSingle = TEXT("analytics/events/single");
	inline constexpr const TCHAR* AnalyticsTransactions = TEXT("analytics/transactions");
	inline constexpr const TCHAR* LogEvent = TEXT("log_event");
	inline constexpr const TCHAR* LogEventSingle = TEXT("log_event/single");

	// Commands — shared by the live call and the offline replay so the two can't drift.
	inline constexpr const TCHAR* CommandUpdatePlayerData = TEXT("game_command/update_player_data");
	inline constexpr const TCHAR* CommandUpdatePlayerDataKey = TEXT("game_command/update_player_data_key");
	inline constexpr const TCHAR* CommandUnlockAchievement = TEXT("game_command/unlock_achievement");
	inline constexpr const TCHAR* CommandAddGameFunds = TEXT("game_command/add_game_funds");
}
