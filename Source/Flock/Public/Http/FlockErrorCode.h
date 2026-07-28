// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockErrorCode.generated.h"

/**
 * Typed view of the backend's coded-error contract (the `detail.code` string). Member name = wire code
 * PascalCased, e.g. "player.email_already_registered" -> PlayerEmailAlreadyRegistered. Unknown = no
 * code, or one this SDK version predates (read FFlockError::Code for the raw string). Keep in sync with
 * the backend OpenAPI spec when it adds codes.
 */
UENUM(BlueprintType)
enum class EFlockErrorCode : uint8
{
	Unknown = 0,

	// analytics.*
	AnalyticsCurrencyNotFound,
	AnalyticsPlayerNotFound,
	AnalyticsSessionNotFound,

	// asset.*
	AssetAssetNotFound,

	// game.*
	GameGameNotFound,
	GameMissingStudioId,

	// game_command.*
	GameCommandAchievementNotFound,
	GameCommandCurrencyNotFound,
	GameCommandInvalidAmount,
	GameCommandNotAWallet,
	GameCommandNotAnAchievementRecord,
	GameCommandPlayerDataNotFound,
	GameCommandPlayerDataNotLinkedToTemplate,
	GameCommandPlayerTemplateNotFound,
	GameCommandTemplateValidationFailed,

	// game_config.*
	GameConfigConfigNotFound,
	GameConfigFeatureConfigNotFound,
	GameConfigInvalidTag,
	GameConfigPlayerNoGameVersion,
	GameConfigPlayerNotFound,

	// game_patch.*
	GamePatchGameConfigNotFound,
	GamePatchPatchNotFound,

	// game_version.*
	GameVersionGameVersionByNameNotFound,
	GameVersionGameVersionNotFound,

	// log_event.*
	LogEventGameNotFound,

	// player.*
	PlayerAppleAccountAlreadyRegistered,
	PlayerDeviceAlreadyRegistered,
	PlayerEmailAlreadyRegistered,
	PlayerGameJwkNotConfigured,
	PlayerGameVersionIdRequired,
	PlayerGoogleAccountAlreadyRegistered,
	PlayerInvalidDeviceRegistrationRequest,
	PlayerInvalidLoginCredentials,
	PlayerInvalidRefreshToken,
	PlayerInvalidRegistrationRequest,
	PlayerInvalidResetCode,
	PlayerInvalidVerificationCode,
	PlayerNameAlreadyRegistered,
	PlayerNoEmailAccount,
	PlayerOauthFailed,
	PlayerPlayerNotFound,
	PlayerSteamAccountAlreadyRegistered,

	// player_ban.*
	PlayerBanPlayerNotFound,

	// player_data.*
	PlayerDataNotFound,
	PlayerDataPlayerNotFound,

	// player_inventory.*
	PlayerInventoryPlayerNotFound,

	// player_template.*
	PlayerTemplateNotFound,
	PlayerTemplateNotFoundByName,

	// shop.*
	ShopCurrencyNotHeld,
	ShopCurrencyTemplateNotFound,
	ShopInsufficientFunds,
	ShopItemNotFound,
	ShopPlayerNotFound,
	ShopShopNotFound,
	ShopWalletNotFound,

	// shop_item.*
	ShopItemShopItemNotFound,
	ShopItemShopNotFound,
};

/** Parses the backend's coded-error string into EFlockErrorCode. */
class FLOCK_API FFlockErrorCodes
{
public:
	/**
	 * Maps a wire code ("namespace.reason_words") to EFlockErrorCode by PascalCasing it; returns Unknown
	 * for empty input or any code not in the enum.
	 */
	static EFlockErrorCode Parse(const FString& Code);
};
