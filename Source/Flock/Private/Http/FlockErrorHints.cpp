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

	/** Every request is the API URL plus /v1/<route>, so a path in the setting doubles or misses part of every route. */
	const TCHAR* const ApiUrlAlone =
		TEXT("API URL under Project Settings > Flock SDK must be the Flock server's address alone, such as https://api.example.com, with no /v1 or other path after it.");

	/** A wallet is built once, at registration; neither a purchase nor Add Game Funds adds a currency to it. */
	const TCHAR* const WalletHoldsTemplateCurrencies =
		TEXT("A wallet holds the currencies of its Game Version's currency template, each starting at zero when the player registers.");

	/** A purchase and the shop reads take the shop item's own id, which a newcomer cannot tell from the item's name. */
	const TCHAR* const ShopItemIdFromShop =
		TEXT("A purchase (Purchase, Flock Purchase) and GetItem (Flock Get Shop Item) take the shop item's Id, FFlockShopItem::Id, read from its shop with GetAll, GetByName or GetItemsByShop (Flock Get All Shops, Flock Get Shop By Name, Flock Get Shop Items); not the item's name or the shop's id. If the id is right, check API Key under Project Settings > Flock SDK is the key of the game that owns the shop.");

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
			{ EFlockErrorCode::GameInvalidApiKey, TEXT("The server does not know this API key. Copy the game's key from the Flock dashboard into API Key under Project Settings > Flock SDK; a key from another Flock server (local, staging or live) is refused too, so check API URL points at the server the key came from.") },
			{ EFlockErrorCode::GameMissingApiKey, TEXT("The request reached the server with a blank API key. Set API Key under Project Settings > Flock SDK (or in the config handed to InitializeWithConfig) to the game's key from the Flock dashboard.") },
			{ EFlockErrorCode::GameMissingStudioId, TEXT("The game record has no studio attached. Fix it in the Flock dashboard — nothing in the project can supply it.") },
			{ EFlockErrorCode::LogEventGameNotFound, TEXT("The API key does not resolve to a game, so log events have nowhere to land. Check API Key under Project Settings > Flock SDK.") },

			// Game commands — dashboard content, plus the write-key rule that trips everyone once.
			{ EFlockErrorCode::GameCommandAchievementNotFound, FString(TEXT("No achievement by that id. ")) + AuthorAndSync },
			{ EFlockErrorCode::GameCommandCurrencyNotFound, FString(TEXT("The player's wallet has no currency by that code. ")) + WalletHoldsTemplateCurrencies + TEXT(" Use one of those codes, letter case included, or add the currency to the currency template in the Flock dashboard.") },
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

			// Matchmaking, parties and sessions. A 404 here usually means "not yours, or no longer live", not "never existed".
			{ EFlockErrorCode::MatchmakingAlreadyQueued, TEXT("A player in this search is already searching, and a player searches once at a time per game, in any queue. FindMatch replaces the player's own earlier search itself, so this is the party's search (wait for it with the party's FindMatch, or OnSearchEnded), or a party member searching alone, whose game cancels that search first.") },
			{ EFlockErrorCode::MatchmakingMatchNotFound, TEXT("No match by that id that the signed-in player is in. Only the players in a match can read it.") },
			{ EFlockErrorCode::MatchmakingNotPartyLeader, TEXT("Only the party leader can put the party in a queue: the leader's game starts the search with the party's FindMatch, and another member's FindMatch waits for it. Leadership changed before this search went out; read the party again (GetMyParty) and let its leader search.") },
			{ EFlockErrorCode::MatchmakingPartyTooLarge, TEXT("The party has more players than the queue allows. Raise the queue's \"Max Players\" in the Flock dashboard, or call FindMatch alone or with a smaller party.") },
			{ EFlockErrorCode::MatchmakingPlayerNotEligible, TEXT("A player in this search is outside the player segments the queue allows. Check the queue's \"Allowed Segments\" in the Flock dashboard.") },
			{ EFlockErrorCode::MatchmakingQueueNotFound, TEXT("No matchmaking queue by that name in this game. FindMatch takes the queue's name as the Flock dashboard shows it, letter case included; create the queue there first.") },
			{ EFlockErrorCode::MatchmakingTicketNotCancelable, TEXT("That search has already ended: it was matched, cancelled or ran out of time. A FindMatch cancelled through its handle deals with this itself, giving up the seat a match gave meanwhile.") },
			{ EFlockErrorCode::MatchmakingTicketNotFound, TEXT("No search by that id for the signed-in player. A player can only read or cancel their own search.") },
			{ EFlockErrorCode::MatchmakingVersionMismatch, TEXT("A party member's game is on another Game Version than the leader's, or has not reported one yet, and this queue matches one build at a time. A member's game reports its version when it creates, joins or reads the party (GetMyParty), so have every member read the party on the same build, then search again with the party's FindMatch; or turn off \"Same game version only\" on the queue in the Flock dashboard.") },
			{ EFlockErrorCode::MultiplayerAlreadyInSession, TEXT("Two requests tried to seat this player at the same moment. A player holds one seat per game; try again once the first has finished.") },
			{ EFlockErrorCode::MultiplayerInvalidJoinCode, TEXT("No open session has that join code. Codes are 6 characters, are not case sensitive, and stop working once the session ends.") },
			{ EFlockErrorCode::MultiplayerInvalidJoinToken, TEXT("The join token is wrong, expired (they last 60 seconds), meant for another session, or its player no longer holds a seat. The joining player asks for a new token with RequestJoinToken and connects again; one that lost its seat joins again with JoinSession first.") },
			{ EFlockErrorCode::MultiplayerJoinCodeUnavailable, TEXT("The server could not find a free join code for a new session. It is temporary; host the session again with HostSession.") },
			{ EFlockErrorCode::MultiplayerMintRateLimited, TEXT("Relay credentials were requested too often (12 a minute per player by default). Request them once per session and reuse them until they expire.") },
			{ EFlockErrorCode::MultiplayerNotAParticipant, TEXT("The signed-in player holds no seat in this session, so there is nothing to leave.") },
			{ EFlockErrorCode::MultiplayerNotHost, TEXT("Only the session's current host can do this. Hosting may have moved to another player; GetSession reads the session again to show who hosts now.") },
			{ EFlockErrorCode::MultiplayerSeatMintLimit, TEXT("This player's seat has drawn the most relay credentials one seat may (10 by default), usually from starting the game's networking again and again in one session. Nothing more is given for this seat, even after leaving and joining the same session again; another session gives a seat of its own.") },
			{ EFlockErrorCode::MultiplayerSessionFull, TEXT("The session has no free seats, or a session created for a party has fewer seats than the party has players. A session's seat count is set when it is created; a matched session takes it from the game's \"Multiplayer settings\" in the Flock dashboard.") },
			{ EFlockErrorCode::MultiplayerSessionNotFound, TEXT("The session does not exist, has ended, or the signed-in player no longer holds a seat in it. The SDK keeps the seat with heartbeats while the game runs, so a game paused past the game's timeouts loses it; join again with the session's join code through JoinSession.") },
			{ EFlockErrorCode::MultiplayerTargetNotAParticipant, TEXT("That player holds no seat in this session, so hosting cannot be handed to them.") },
			{ EFlockErrorCode::MultiplayerVersionMismatch, TEXT("The session keeps its players to one Game Version: it was opened on another build, or, for a session made for a party, a party member is on another build. Players join sessions of their own build; a host lets any build join by calling HostSession with bSameVersionOnly false.") },
			{ EFlockErrorCode::PartyAlreadyInParty, TEXT("The signed-in player is already in a party in this game, and a player can be in one at a time. GetMyParty returns it; leave it with the party's Leave before creating or joining another.") },
			{ EFlockErrorCode::PartyCannotKickLeader, TEXT("The leader cannot remove themselves. Hand leadership to another member with MakeLeader and then leave, or disband the party with Disband.") },
			{ EFlockErrorCode::PartyFull, TEXT("The party has no free places, or its size was set below the number of players already in it.") },
			{ EFlockErrorCode::PartyInvalidInviteCode, TEXT("No open party has that invite code. Ask the party leader for the current code.") },
			{ EFlockErrorCode::PartyNotAMember, TEXT("The signed-in player is not in that party, so there is nothing to leave.") },
			{ EFlockErrorCode::PartyNotFound, TEXT("No party by that id that the signed-in player belongs to. It may have been disbanded, or the player removed from it; a party the SDK held for the player ends as Removed, and GetMyParty reads the player's current party.") },
			{ EFlockErrorCode::PartyNotPartyLeader, TEXT("Only the party leader can do this. Leadership may have moved to another member; GetMyParty reads the party again to show who leads now.") },
			{ EFlockErrorCode::PartyTargetNotAMember, TEXT("That player is not in the party, so they cannot be removed or made leader.") },

			// Auth — signing in never creates an account, which is the most common first-run mistake.
			{ EFlockErrorCode::PlayerAccountAlreadyLinked, TEXT("That credential already belongs to an account. Call Flock Get Linked Accounts to see what this player is already linked to.") },
			{ EFlockErrorCode::PlayerAccountNotLinked, TEXT("That credential is not linked to this player. Attach it with the matching Flock Link node first.") },
			{ EFlockErrorCode::PlayerAppleAccountAlreadyRegistered, TEXT("That Apple account is already registered. Call Flock Login With Apple instead.") },
			{ EFlockErrorCode::PlayerCannotUnlinkLastCredential, TEXT("A player must keep at least one way to sign in. Link another credential before unlinking this one.") },
			{ EFlockErrorCode::PlayerDeviceAlreadyRegistered, TEXT("This device already has an account. Call Flock Login With Device instead of registering it again.") },
			{ EFlockErrorCode::PlayerEmailAlreadyRegistered, TEXT("That email already has an account. Call Flock Login With Email, or Flock Forgot Password if the password is lost.") },
			{ EFlockErrorCode::PlayerExpiredToken, TEXT("The player's sign-in expired and could not be renewed. Sign the player in again; the SDK renews a sign-in by itself while its saved session is still valid.") },
			{ EFlockErrorCode::PlayerGameJwkNotConfigured, TEXT("The game has no signing key configured, so nobody can authenticate. Set one up in the Flock dashboard.") },
			{ EFlockErrorCode::PlayerGameVersionIdRequired, FString(TEXT("This route needs a game version. ")) + ResolveGameVersion },
			{ EFlockErrorCode::PlayerGoogleAccountAlreadyRegistered, TEXT("That Google account is already registered. Call Flock Login With Google instead.") },
			{ EFlockErrorCode::PlayerInvalidDeviceRegistrationRequest, TEXT("The device registration was rejected. Pass a stable, non-empty device id — the same string on every launch, or the account cannot be found again.") },
			{ EFlockErrorCode::PlayerInvalidLinkRequest, TEXT("The link request was rejected. Every provider links with the raw token the platform handed you, not a wrapped payload of your own.") },
			{ EFlockErrorCode::PlayerInvalidLoginCredentials, TEXT("No account matches these credentials. Signing in never creates an account — call the matching Flock Register With node once first.") },
			{ EFlockErrorCode::PlayerInvalidRefreshToken, TEXT("The saved session is no longer valid. Call Flock Logout and sign in again.") },
			{ EFlockErrorCode::PlayerInvalidRegistrationRequest, TEXT("The registration was rejected. Check the fields this provider requires — an email registration needs both an email and a password.") },
			{ EFlockErrorCode::PlayerInvalidResetCode, TEXT("The password reset code is wrong or expired. Call Flock Forgot Password to issue a new one.") },
			{ EFlockErrorCode::PlayerInvalidToken, TEXT("The server does not accept the player's sign-in: it was made by another game or another Flock server, or the game's signing key changed since. Call Flock Logout and sign the player in again.") },
			{ EFlockErrorCode::PlayerInvalidVerificationCode, TEXT("The email verification code is wrong or expired. Call Flock Send Email Verification to issue a new one.") },
			{ EFlockErrorCode::PlayerMissingToken, TEXT("This call acts as the signed-in player and went out with no sign-in. Sign the player in first, and make the call once the sign-in has succeeded.") },
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

			// Request — the server's own codes for a failure no route raised itself. A wrong API URL lands here.
			{ EFlockErrorCode::RequestConflict, TEXT("The change clashed with another made at the same moment, or with what the record holds now. Read it again, then make the change again if it still applies.") },
			{ EFlockErrorCode::RequestFailed, TEXT("The server could not complete the request; the reason above is its own. Try again later, and check the server is running if it lasts.") },
			{ EFlockErrorCode::RequestForbidden, TEXT("The server does not allow this request; the reason above is its own. Check the call is one a game may make with its API key, for the player signed in now.") },
			{ EFlockErrorCode::RequestMethodNotAllowed, FString(TEXT("The server has a route at this address, but not for this kind of request, so the address is not the one this SDK expects. ")) + ApiUrlAlone + TEXT(" If it is, the server runs an older Flock than this SDK.") },
			{ EFlockErrorCode::RequestNotFound, FString(TEXT("The server found nothing for this request; the reason above says what. A bare \"Not Found\" means no route answers at this address: ")) + ApiUrlAlone + TEXT(" If it is, the server runs an older Flock that does not have this feature yet. Any other reason names a record this game or player does not have: check the id passed to the call.") },
			{ EFlockErrorCode::RequestPayloadTooLarge, TEXT("The request is larger than the server accepts. Send less in one call: split a large write into several.") },
			{ EFlockErrorCode::RequestRateLimited, TEXT("The server turned the request away for volume; the reason above says which kind. Too many calls in a short time: the SDK retries reads after the wait the server asks for, so send writes less often, several values together in one call. A limit on how many of a thing a player may hold, such as pending scheduled notifications: cancel some, as waiting will not help.") },
			{ EFlockErrorCode::RequestUnauthorized, TEXT("The server refused the request's credentials. Check API Key under Project Settings > Flock SDK against the Flock dashboard, and sign the player in again.") },
			{ EFlockErrorCode::RequestValidationFailed, TEXT("The server refused a value in the request; the reason above names it (body.<name> is a member of what the SDK sent, header.<name> a header). It is usually a value passed to this call: check it against the call's documentation. header.X-Flock-API-Key means API Key under Project Settings > Flock SDK is empty. A member you never pass means this SDK and the server are on different Flock versions.") },

			// Shop — runtime state the caller can act on, not misconfiguration.
			{ EFlockErrorCode::ShopCurrencyNotHeld, FString(TEXT("The player's wallet has no balance in the item's currency. ")) + WalletHoldsTemplateCurrencies + TEXT(" Price the item in one of those, or add the currency to the currency template in the Flock dashboard.") },
			{ EFlockErrorCode::ShopCurrencyTemplateNotFound, TEXT("This Game Version has no currency template (a player-data template tagged currency), so no player has a wallet to pay from. Create one in the Flock dashboard under the version this build runs (Game Version under Project Settings > Flock SDK).") },
			{ EFlockErrorCode::ShopRealCurrencyNeedsCheckout, TEXT("This item is priced in a currency the game's currency config lists as real money, and Purchase only spends game currencies from the player's wallet. Price the item in a game currency in the Flock dashboard, or leave it out of what the game sells through Purchase.") },
			{ EFlockErrorCode::ShopInsufficientFunds, TEXT("The player cannot afford this item. Read the wallet balance before offering the purchase.") },
			{ EFlockErrorCode::ShopItemNotFound, FString(TEXT("No shop item has that id in this game. ")) + ShopItemIdFromShop },
			{ EFlockErrorCode::ShopMalformedReward, TEXT("The item's stored reward cannot be granted, so the purchase was refused rather than half-applied. Fix that item's rewards in the Flock dashboard — no change to the call will work around it.") },
			{ EFlockErrorCode::ShopPackGrantsNothing, TEXT("This currency pack has no rewards configured, so buying it would grant nothing. Add its rewards in the Flock dashboard, or take it off sale.") },
			{ EFlockErrorCode::ShopPlayerNotFound, TEXT("No player has that id in this game. Leave Player Id empty so the purchase acts on the signed-in player, and sign in before offering one; a player id from another game or another Flock server is not found.") },
			{ EFlockErrorCode::ShopRewardCurrencyNotHeld, FString(TEXT("The reward pays out a currency the player's wallet does not have. ")) + WalletHoldsTemplateCurrencies + TEXT(" Reward one of those, or add the currency to the currency template in the Flock dashboard.") },
			{ EFlockErrorCode::ShopShopNotFound, TEXT("No shop by that name in this build's Game Version, or by that id in this game. A shop belongs to one Game Version: create it in the Flock dashboard under the version this build runs (Game Version under Project Settings > Flock SDK), and give its name exactly as the dashboard shows it, letter case included.") },
			{ EFlockErrorCode::ShopWalletNotFound, TEXT("This player has no wallet in this Game Version. The server makes a player's wallet when the player registers, from the version's currency template, so a player who registered before that template existed, or under another Game Version, has none. Create the currency template first, then register the player under this build's Game Version.") },
			{ EFlockErrorCode::ShopItemShopItemNotFound, FString(TEXT("No shop item has that id in this game. ")) + ShopItemIdFromShop },
			{ EFlockErrorCode::ShopItemShopNotFound, TEXT("No shop has that id in this game. GetItemsByShop (Flock Get Shop Items) takes the shop's Id, FFlockShop::Id, read with GetAll or GetByName (Flock Get All Shops, Flock Get Shop By Name); not the shop's name.") },
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

FString FFlockErrorHints::ForFailure(const FFlockError& Error)
{
	const FString ForCode = For(Error.ErrorCode);
	if (!ForCode.IsEmpty())
	{
		return ForCode;
	}

	switch (Error.Type)
	{
	case EFlockErrorType::Cancelled:
		return TEXT("Nothing needs fixing if the game cancelled this call, the player signed out or Flock shut down; otherwise make the call again.");
	case EFlockErrorType::Timeout:
		return TEXT("Check the device's network connection, then try again; the SDK retries a read by itself. A purchase or other change that timed out may still have been made, so read the result before sending it again.");
	case EFlockErrorType::Connection:
		return FString(TEXT("Check the device is online and can reach the Flock server: a wrong address, a firewall or a VPN stops every call. ")) + ApiUrlAlone;
	case EFlockErrorType::Serialization:
		// With a status the server answered something unreadable; with none the SDK could not build its own request.
		return Error.StatusCode > 0
			? NotFlocksAnswer()
			: FString(TEXT("This is a fault in the Flock SDK, not in the call. Report it with this log line."));
	default:
		break;
	}

	// Made by the SDK itself, which states its own fix wherever it has one; these are what is left.
	if (Error.StatusCode == 0)
	{
		switch (Error.Type)
		{
		case EFlockErrorType::Auth:
			return SignInFirst();
		case EFlockErrorType::Validation:
			return TEXT("Change what the reason above names, then make the call again.");
		default:
			return TEXT("Make the call again. If it keeps failing, check the device's network connection.");
		}
	}

	if (Error.StatusCode >= 500)
	{
		return TEXT("The server, or a proxy in front of it, failed while handling the call; the call did not cause it. Try again later (the SDK retries a read by itself), and check the server is running if it lasts.");
	}
	if (!Error.Code.IsEmpty())
	{
		// Flock answered with a code newer than this SDK: its reason is all there is.
		return FString::Printf(TEXT("The reason above is the server's own. This version of the Flock SDK has no advice for the code %s; a newer version may."), *Error.Code);
	}
	if (Error.StatusCode == 408 || Error.StatusCode == 429)
	{
		return TEXT("Something between the game and Flock is asking it to slow down. Wait a moment and try again; the SDK retries a read by itself.");
	}
	// Every failure Flock's game API sends carries a code, so an answer without one came from something else.
	return FString(TEXT("The answer carries no Flock error code, so it did not come from Flock's game API: a proxy, a firewall or a captive portal answered, or the request went to another server. ")) + ApiUrlAlone;
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
	// Unknown is "no code, or one this SDK version predates": it has no advice of its own, and ForFailure gives the
	// advice for the kind of failure instead.
	return Code == EFlockErrorCode::Unknown;
}

FString FFlockErrorHints::SignInFirst()
{
	return TEXT("Sign the player in first, and make this call once the sign-in has succeeded.");
}

FString FFlockErrorHints::InitializeFlockFirst()
{
	return TEXT("Initialize Flock before this call. It initializes itself at startup when Auto Initialize On Load is on under Project Settings > Flock SDK, and the Output Log says what stopped it (an unresolved Game Version, an empty API URL); otherwise call Initialize From Settings or Initialize With Config on the Flock subsystem first. A Blueprint node also needs an object in the game's world as its World Context.");
}

FString FFlockErrorHints::NotFlocksAnswer()
{
	return FString(TEXT("The answer did not come from Flock's game API: a captive portal (a Wi-Fi sign-in page), a proxy, or an API URL that points at another server answers like this. Sign in to the network if it asks, and check the address. ")) + ApiUrlAlone;
}

FString FFlockErrorHints::PassAHeldSession()
{
	return TEXT("Pass the session the game holds: the one HostSession, JoinSession or GetMySession handed back.");
}

FString FFlockErrorHints::PassAHeldParty()
{
	return TEXT("Pass the party the game holds: the one CreateParty, JoinParty or GetMyParty handed back.");
}

FString FFlockErrorHints::FindAnotherSession()
{
	return TEXT("Host or join another session (HostSession, JoinSession), or get the player's current one with GetMySession.");
}

FString FFlockErrorHints::ForStorageDownload(int32 StatusCode)
{
	if (StatusCode <= 0)
	{
		return TEXT("Check the device is online and that a firewall or VPN lets it reach the asset's download address; asset files come from storage, not from the Flock server.");
	}
	if (StatusCode == 401 || StatusCode == 403)
	{
		return TEXT("The download link expired or storage refused it. Read the asset again (GetById or GetAll) for a fresh link, then download it again.");
	}
	if (StatusCode == 404)
	{
		return TEXT("The asset's file is missing from storage. Upload it again in the Flock dashboard and publish it to this game version.");
	}
	if (StatusCode >= 500)
	{
		return TEXT("Storage failed while serving the file; the call did not cause it. Try again later.");
	}
	return TEXT("Storage refused the download. Read the asset again for a fresh link, then download it again.");
}

FString FFlockErrorHints::FindAnotherParty()
{
	return TEXT("Create or join another party (CreateParty, JoinParty), or get the player's current one with GetMyParty.");
}
