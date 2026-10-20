// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockErrorCode.generated.h"

/**
 * Typed view of the backend's coded-error contract (the `detail.code` string). Member name = wire code
 * PascalCased, e.g. "player.email_already_registered" -> PlayerEmailAlreadyRegistered. Unknown = no
 * code, or one this SDK version predates (read FFlockError::Code for the raw string). Keep in sync with
 * Tooling/WireErrorCodes.txt, whose header says how to refresh it from the backend.
 */
UENUM(BlueprintType)
enum class EFlockErrorCode : uint8
{
	Unknown = 0,

	// analytics.*
	AnalyticsCurrencyNotFound,
	AnalyticsInvalidCurrencyId,
	AnalyticsPlayerNotFound,
	AnalyticsSessionNotFound,

	// asset.*
	AssetAssetNotFound,

	// game.* (InvalidApiKey and MissingApiKey come from the API key check every /v1 route makes first)
	GameGameNotFound,
	GameInvalidApiKey,
	GameMissingApiKey,
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
	GameCommandRateLimited,
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

	// leaderboard.*
	//
	// Only the by-name lookup answers with a code — the standings/rank routes raise nothing, so a bad
	// board always surfaces here rather than as empty standings. The backend defines many more
	// leaderboard codes, but they belong to the dashboard's create/season/prize routes, which this SDK
	// does not call.
	LeaderboardNotFound,

	// log_event.*
	LogEventGameNotFound,

	// matchmaking.* (NotPartyLeader and PlayerNotEligible arrive as 403s that mean "not allowed", never "signed out")
	MatchmakingAlreadyQueued,
	MatchmakingMatchNotFound,
	MatchmakingNotPartyLeader,
	MatchmakingPartyTooLarge,
	MatchmakingPlayerNotEligible,
	MatchmakingQueueNotFound,
	MatchmakingTicketNotCancelable,
	MatchmakingTicketNotFound,
	MatchmakingVersionMismatch,

	// multiplayer.* (sessions and the relay; NotHost is a 403 that means "not allowed")
	MultiplayerAlreadyInSession,
	MultiplayerInvalidJoinCode,
	MultiplayerInvalidJoinToken,
	MultiplayerJoinCodeUnavailable,
	MultiplayerMintRateLimited,
	MultiplayerNotAParticipant,
	MultiplayerNotHost,
	MultiplayerSeatMintLimit,
	MultiplayerSessionFull,
	MultiplayerSessionNotFound,
	MultiplayerTargetNotAParticipant,
	MultiplayerVersionMismatch,

	// notification_template.*
	//
	// The wire code is `notification_template.not_found`, so the PascalCased name collapses to this —
	// it is not `notification.template_not_found`. The inbox and schedule routes raise no code of their
	// own; an unknown template is the only coded failure in the notification family.
	NotificationTemplateNotFound,

	// party.* (NotPartyLeader is a 403 that means "not allowed")
	PartyAlreadyInParty,
	PartyCannotKickLeader,
	PartyFull,
	PartyInvalidInviteCode,
	PartyNotAMember,
	PartyNotFound,
	PartyNotPartyLeader,
	PartyTargetNotAMember,

	// player.* (MissingToken, ExpiredToken and InvalidToken come from the sign-in check of a route that acts as the player)
	PlayerAccountAlreadyLinked,
	PlayerAccountNotLinked,
	PlayerAppleAccountAlreadyRegistered,
	PlayerCannotUnlinkLastCredential,
	PlayerDeviceAlreadyRegistered,
	PlayerEmailAlreadyRegistered,
	PlayerExpiredToken,
	PlayerGameJwkNotConfigured,
	PlayerGameVersionIdRequired,
	PlayerGoogleAccountAlreadyRegistered,
	PlayerInvalidDeviceRegistrationRequest,
	PlayerInvalidLinkRequest,
	PlayerInvalidLoginCredentials,
	PlayerInvalidRefreshToken,
	PlayerInvalidRegistrationRequest,
	PlayerInvalidResetCode,
	PlayerInvalidToken,
	PlayerInvalidVerificationCode,
	PlayerMissingToken,
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
	PlayerInventoryAlreadyUsed,
	PlayerInventoryInventoryEntryNotFound,
	PlayerInventoryPlayerNotFound,

	// player_template.*
	PlayerTemplateNotFound,
	PlayerTemplateNotFoundByName,

	// request.*
	//
	// The server's own codes for a failure no route raised itself: a route that does not exist, a request the framework
	// refused before any route code ran (a member missing from the body), and the stock code for each status. Every /v1
	// failure carries a code, so an error with none did not come from Flock's game API.
	RequestConflict,
	RequestFailed,
	RequestForbidden,
	RequestMethodNotAllowed,
	RequestNotFound,
	RequestPayloadTooLarge,
	RequestRateLimited,
	RequestUnauthorized,
	RequestValidationFailed,

	// shop.*
	ShopCurrencyNotHeld,
	ShopCurrencyTemplateNotFound,
	ShopInsufficientFunds,
	ShopItemNotFound,
	ShopMalformedReward,
	ShopPackGrantsNothing,
	ShopPlayerNotFound,
	ShopRealCurrencyNeedsCheckout,
	ShopRewardCurrencyNotHeld,
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
