// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Http/FlockErrorHints.h"

namespace
{
	/** Dashboard content only reaches the game after a schema sync, which is the step newcomers miss. */
	const TCHAR* const AuthorAndSync =
		TEXT("Author it in the Flock dashboard, then run Tools > Flock > Sync Schemas so the generated types match.");

	/** No sync involved — these are read at runtime, so publishing is the whole fix. */
	const TCHAR* const AuthorInDashboard =
		TEXT("Author it in the Flock dashboard and publish it to this game version.");

	/** Every id-taking node treats an empty Player Id pin as "the signed-in player". */
	const TCHAR* const LeavePlayerIdEmpty =
		TEXT("No player matches that id. Leave the Player Id empty to act on the signed-in player.");

	/** The version id is baked at edit time, so a bad one is fixed in the editor and not in the call. */
	const TCHAR* const ResolveGameVersion =
		TEXT("Run Tools > Flock > Resolve Game Version so a valid id is baked into the build.");

	const TMap<EFlockErrorCode, FString>& GetHints()
	{
		static const TMap<EFlockErrorCode, FString> Hints = {
			// Analytics — the session follows sign-in, so these are almost always ordering mistakes.
			{ EFlockErrorCode::AnalyticsCurrencyNotFound, TEXT("Transaction analytics need a currency entity for this game. Create one in the Flock dashboard.") },
			{ EFlockErrorCode::AnalyticsInvalidCurrencyId, TEXT("The transaction's Currency Id is not a 26-character ULID. Leave it empty and set Currency Code instead — the backend resolves the code to an id.") },
			{ EFlockErrorCode::AnalyticsPlayerNotFound, TEXT("The analytics call named a player the backend does not have. Sessions follow sign-in — start one only after a login node reports success.") },
			{ EFlockErrorCode::AnalyticsSessionNotFound, TEXT("That analytics session is no longer open on the server. The SDK opens one on sign-in and closes it on logout; a new session starts on the next sign-in.") },

			// Assets.
			{ EFlockErrorCode::AssetAssetNotFound, TEXT("No asset by that id. Upload it in the Flock dashboard and publish it to this game version, then re-read the index with Flock Get Assets.") },

			// Game / key configuration — the settings page, not the call.
			{ EFlockErrorCode::GameGameNotFound, TEXT("The API key does not resolve to a game. Check API Key under Project Settings > Flock SDK against the dashboard.") },
			{ EFlockErrorCode::GameMissingStudioId, TEXT("The game record has no studio attached. Fix it in the Flock dashboard — nothing in the project can supply it.") },
			{ EFlockErrorCode::LogEventGameNotFound, TEXT("The API key does not resolve to a game, so log events have nowhere to land. Check API Key under Project Settings > Flock SDK.") },

			// Game commands — dashboard content, plus the write-key rule that trips everyone once.
			{ EFlockErrorCode::GameCommandAchievementNotFound, FString(TEXT("No achievement by that id. ")) + AuthorAndSync },
			{ EFlockErrorCode::GameCommandCurrencyNotFound, FString(TEXT("No currency by that name. ")) + AuthorAndSync },
			{ EFlockErrorCode::GameCommandInvalidAmount, TEXT("The amount must be greater than zero.") },
			{ EFlockErrorCode::GameCommandNotAWallet, TEXT("That record is not a wallet. Flock Add Game Funds only works on wallet-typed player data.") },
			{ EFlockErrorCode::GameCommandNotAnAchievementRecord, TEXT("That record is not an achievement. Flock Unlock Achievement only works on achievement-typed player data.") },
			{ EFlockErrorCode::GameCommandPlayerDataNotFound, TEXT("The player has no record for that template yet. A read never creates one — write it first with Flock Update Player Data.") },
			{ EFlockErrorCode::GameCommandPlayerDataNotLinkedToTemplate, TEXT("That record is not linked to a template, so template-aware commands cannot act on it. Re-create it from the template in the Flock dashboard.") },
			{ EFlockErrorCode::GameCommandPlayerTemplateNotFound, FString(TEXT("No player-data template by that name. ")) + AuthorAndSync },
			{ EFlockErrorCode::GameCommandRateLimited, TEXT("Too many writes to that player-data row. The SDK already retries this with backoff — send the fields together in one Flock Update Player Data rather than one call per field.") },
			{ EFlockErrorCode::GameCommandTemplateValidationFailed, TEXT("The value does not match the template's schema. A write is matched against the template's declared field names, not the names a read hands back — compare them in the Flock dashboard, then re-run Tools > Flock > Sync Schemas.") },

			// Game config and patches.
			{ EFlockErrorCode::GameConfigConfigNotFound, FString(TEXT("No game config by that name. ")) + AuthorInDashboard },
			{ EFlockErrorCode::GameConfigFeatureConfigNotFound, FString(TEXT("No feature config by that name. ")) + AuthorInDashboard },
			{ EFlockErrorCode::GameConfigInvalidTag, TEXT("The backend does not accept that config tag. Pass a value from EFlockConfigTag — Any omits the filter — rather than a hand-typed string.") },
			{ EFlockErrorCode::GameConfigPlayerNoGameVersion, FString(TEXT("The player has no game version attached. ")) + ResolveGameVersion },
			{ EFlockErrorCode::GameConfigPlayerNotFound, LeavePlayerIdEmpty },
			{ EFlockErrorCode::GamePatchGameConfigNotFound, TEXT("The patch points at a config that no longer exists. Re-check the config it patches in the Flock dashboard.") },
			{ EFlockErrorCode::GamePatchPatchNotFound, FString(TEXT("No patch by that id. ")) + AuthorInDashboard },

			// Game version — always an edit-time bake, never a runtime argument.
			{ EFlockErrorCode::GameVersionGameVersionByNameNotFound, FString(TEXT("No game version by that name. ")) + ResolveGameVersion },
			{ EFlockErrorCode::GameVersionGameVersionNotFound, FString(TEXT("That game version id does not exist. ")) + ResolveGameVersion },

			// Notifications — the template catalog is the family's only coded failure.
			{ EFlockErrorCode::NotificationTemplateNotFound, FString(TEXT("No notification template by that name, or it is not active. ")) + AuthorInDashboard },

			// Leaderboards.
			{ EFlockErrorCode::LeaderboardNotFound, FString(TEXT("No leaderboard by that name — boards are addressed by name, never by id. ")) + AuthorInDashboard },

			// Auth — signing in never creates an account, which is the most common first-run mistake.
			{ EFlockErrorCode::PlayerAccountAlreadyLinked, TEXT("That credential already belongs to an account. Call Flock Get Linked Accounts to see what this player is already linked to.") },
			{ EFlockErrorCode::PlayerAccountNotLinked, TEXT("That credential is not linked to this player. Attach it with the matching Flock Link node first.") },
			{ EFlockErrorCode::PlayerAppleAccountAlreadyRegistered, TEXT("That Apple account is already registered. Call Flock Login With Apple instead.") },
			{ EFlockErrorCode::PlayerCannotUnlinkLastCredential, TEXT("A player must keep at least one way to sign in. Link another credential before unlinking this one.") },
			{ EFlockErrorCode::PlayerDeviceAlreadyRegistered, TEXT("This device already has an account. Call Flock Login With Device instead of registering it again.") },
			{ EFlockErrorCode::PlayerEmailAlreadyRegistered, TEXT("That email already has an account. Call Flock Login With Email, or Flock Forgot Password if the password is lost.") },
			{ EFlockErrorCode::PlayerGameJwkNotConfigured, TEXT("The game has no signing key configured, so nobody can authenticate. Set one up in the Flock dashboard.") },
			{ EFlockErrorCode::PlayerGameVersionIdRequired, FString(TEXT("This route needs a game version. ")) + ResolveGameVersion },
			{ EFlockErrorCode::PlayerGoogleAccountAlreadyRegistered, TEXT("That Google account is already registered. Call Flock Login With Google instead.") },
			{ EFlockErrorCode::PlayerInvalidDeviceRegistrationRequest, TEXT("The device registration was rejected. Pass a stable, non-empty device id — the same string on every launch, or the account cannot be found again.") },
			{ EFlockErrorCode::PlayerInvalidLinkRequest, TEXT("The link request was rejected. Every provider links with the raw token the platform handed you, not a wrapped payload of your own.") },
			{ EFlockErrorCode::PlayerInvalidLoginCredentials, TEXT("No account matches these credentials. Signing in never creates an account — call the matching Flock Register With node once first.") },
			{ EFlockErrorCode::PlayerInvalidRefreshToken, TEXT("The saved session is no longer valid. Call Flock Logout and sign in again.") },
			{ EFlockErrorCode::PlayerInvalidRegistrationRequest, TEXT("The registration was rejected. Check the fields this provider requires — an email registration needs both an email and a password.") },
			{ EFlockErrorCode::PlayerInvalidResetCode, TEXT("The password reset code is wrong or expired. Call Flock Forgot Password to issue a new one.") },
			{ EFlockErrorCode::PlayerInvalidVerificationCode, TEXT("The email verification code is wrong or expired. Call Flock Send Email Verification to issue a new one.") },
			{ EFlockErrorCode::PlayerNameAlreadyRegistered, TEXT("Display names are unique per game. Check Flock Is Name Available first, or register with an empty name and set it later.") },
			{ EFlockErrorCode::PlayerNoEmailAccount, TEXT("This player has no email credential. Attach one with Flock Link Email before using email-only flows.") },
			{ EFlockErrorCode::PlayerOauthFailed, TEXT("The identity provider rejected the token. It has usually expired — fetch a fresh one from the platform immediately before the call.") },
			{ EFlockErrorCode::PlayerPlayerNotFound, TEXT("No player matches that id. Sign in first — a node with an empty Player Id pin acts on the signed-in player.") },
			{ EFlockErrorCode::PlayerSteamAccountAlreadyRegistered, TEXT("That Steam account is already registered. Call Flock Login With Steam instead.") },

			// Player-scoped reads.
			{ EFlockErrorCode::PlayerBanPlayerNotFound, LeavePlayerIdEmpty },
			{ EFlockErrorCode::PlayerDataNotFound, TEXT("The player has no record for that template yet. A read never creates one — write it first with Flock Update Player Data.") },
			{ EFlockErrorCode::PlayerDataPlayerNotFound, LeavePlayerIdEmpty },
			{ EFlockErrorCode::PlayerInventoryAlreadyUsed, TEXT("That inventory entry has already been consumed — each one is spendable once. Re-read the player's inventory before offering it again.") },
			{ EFlockErrorCode::PlayerInventoryInventoryEntryNotFound, TEXT("No inventory entry by that id in this game. Consume takes the entry's own id — a purchase result's Inventory.Id — not the shop item id.") },
			{ EFlockErrorCode::PlayerInventoryPlayerNotFound, LeavePlayerIdEmpty },
			{ EFlockErrorCode::PlayerTemplateNotFound, FString(TEXT("No player-data template by that id. ")) + AuthorAndSync },
			{ EFlockErrorCode::PlayerTemplateNotFoundByName, FString(TEXT("No player-data template by that name. ")) + AuthorAndSync },

			// Shop — runtime state the caller can act on, not misconfiguration.
			{ EFlockErrorCode::ShopCurrencyNotHeld, TEXT("The player holds no wallet for that currency yet. Grant funds once with Flock Add Game Funds to create it.") },
			{ EFlockErrorCode::ShopCurrencyTemplateNotFound, FString(TEXT("No currency template by that name. ")) + AuthorAndSync },
			{ EFlockErrorCode::ShopInsufficientFunds, TEXT("The player cannot afford this item. Read the wallet balance before offering the purchase.") },
			{ EFlockErrorCode::ShopItemNotFound, FString(TEXT("No shop item by that id. ")) + AuthorInDashboard },
			{ EFlockErrorCode::ShopMalformedReward, TEXT("The item's stored reward cannot be granted, so the purchase was refused rather than half-applied. Fix that item's rewards in the Flock dashboard — no change to the call will work around it.") },
			{ EFlockErrorCode::ShopPackGrantsNothing, TEXT("This currency pack has no rewards configured, so buying it would grant nothing. Add its rewards in the Flock dashboard, or take it off sale.") },
			{ EFlockErrorCode::ShopPlayerNotFound, TEXT("No player matches that id. A purchase always acts on the signed-in player, so sign in before offering one.") },
			{ EFlockErrorCode::ShopRewardCurrencyNotHeld, TEXT("The reward pays out a currency this player holds no wallet for. Grant that currency once with Flock Add Game Funds to create the wallet before the reward can land.") },
			{ EFlockErrorCode::ShopShopNotFound, FString(TEXT("No shop by that name. ")) + AuthorInDashboard },
			{ EFlockErrorCode::ShopWalletNotFound, TEXT("The player has no wallet for that currency yet. Grant funds once with Flock Add Game Funds to create it.") },
			{ EFlockErrorCode::ShopItemShopItemNotFound, FString(TEXT("No shop item by that id. ")) + AuthorInDashboard },
			{ EFlockErrorCode::ShopItemShopNotFound, FString(TEXT("No shop by that id. ")) + AuthorInDashboard },
		};
		return Hints;
	}

	/** The node-name half of "Flock Login With X" / "Flock Link X"; empty for a method with no provider name. */
	FString ProviderWord(EFlockAuthMethod Method)
	{
		switch (Method)
		{
		case EFlockAuthMethod::Google:   return TEXT("Google");
		case EFlockAuthMethod::Apple:    return TEXT("Apple");
		case EFlockAuthMethod::Steam:    return TEXT("Steam");
		case EFlockAuthMethod::Facebook: return TEXT("Facebook");
		case EFlockAuthMethod::Discord:  return TEXT("Discord");
		default:                         return FString();
		}
	}
}

FString FFlockErrorHints::For(EFlockErrorCode Code)
{
	if (Code == EFlockErrorCode::Unknown)
	{
		return FString();
	}
	const FString* Hint = GetHints().Find(Code);
	return Hint ? *Hint : FString();
}

FString FFlockErrorHints::ForAuth(EFlockErrorCode Code, EFlockAuthMethod Method)
{
	if (Code != EFlockErrorCode::PlayerInvalidLoginCredentials)
	{
		return For(Code);
	}

	switch (Method)
	{
	case EFlockAuthMethod::Device:
		return TEXT("This device is not registered yet. Call Flock Register With Device once to create the account, then Flock Login With Device on later launches.");
	case EFlockAuthMethod::Email:
		return TEXT("Wrong email or password. If the account does not exist yet, call Flock Register With Email first — signing in never creates one.");
	case EFlockAuthMethod::Google:
	case EFlockAuthMethod::Apple:
	case EFlockAuthMethod::Steam:
		return FString::Printf(
			TEXT("No Flock account is linked to that %s identity yet. Call Flock Register With %s once, or attach it to the signed-in player with Flock Link %s."),
			*ProviderWord(Method), *ProviderWord(Method), *ProviderWord(Method));
	// No register route exists for these two — linking to an existing player is the only way in.
	case EFlockAuthMethod::Facebook:
	case EFlockAuthMethod::Discord:
		return FString::Printf(
			TEXT("No Flock account is linked to that %s identity yet. Sign in another way, then call Flock Link %s — there is no %s registration route."),
			*ProviderWord(Method), *ProviderWord(Method), *ProviderWord(Method));
	default:
		return For(Code);
	}
}

bool FFlockErrorHints::IsWaived(EFlockErrorCode Code)
{
	// Unknown is "no code, or one this SDK version predates" — there is nothing to advise about a
	// failure the SDK cannot identify, and the server's own reason is all the caller has.
	return Code == EFlockErrorCode::Unknown;
}
