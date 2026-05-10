// Copyright 2022, Qwack. All Rights Reserved.


#include "QwackGameEndpoints.h"



//would be good for dev mode or accessing some config file
/*const UQwackConfig* config = GetDefault<UQwackConfig>();
FString BaseURL = config->GameBaseURI;*/

FString UQwackFlockGameEndpoints::GameBaseURI = "https://Qwacks.sa";//"http://localhost:3000/";
FString UQwackFlockGameEndpoints::AuthBaseURI = "https://qwacks.sa/auth/";//"http://localhost:8080/";




// Weapon Endpoints
FSQwackFlockEndpoints UQwackFlockGameEndpoints::CreateAchievement = SetupEndpoints("/achievement", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetAchievements = SetupEndpoints("/achievement", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetAchievementByID = SetupEndpoints("/achievement/{achievement_id}", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::DeleteAchievement = SetupEndpoints("/achievement/{achievement_id}", EQwackSDKHTTPType::DELETE); // Bad tbh 
FSQwackFlockEndpoints UQwackFlockGameEndpoints::UpdateAchievement = SetupEndpoints("/achievement/{achievement_id}", EQwackSDKHTTPType::PUT);


// assets
FSQwackFlockEndpoints UQwackFlockGameEndpoints::CreateAssets = SetupEndpoints("/asset", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetAssets = SetupEndpoints("/asset", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetAssetByID = SetupEndpoints("/asset/{asset_id}", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::DeleteAssetByID = SetupEndpoints("/asset/{asset_id}", EQwackSDKHTTPType::DELETE);

// Bans
FSQwackFlockEndpoints UQwackFlockGameEndpoints::CreateBan = SetupEndpoints("/ban", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetBans = SetupEndpoints("/ban", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::UpdateBansByID = SetupEndpoints("/ban/{ban_id}", EQwackSDKHTTPType::PUT);

// Game Confi
FSQwackFlockEndpoints UQwackFlockGameEndpoints::CreateGameConfig = SetupEndpoints("/game-config", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetGameConfigs = SetupEndpoints("/game-config", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::UpdateGameConfigByID = SetupEndpoints("/game-config/{config_id}", EQwackSDKHTTPType::PUT);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetGameConfigByID = SetupEndpoints("/game-config/{config_id}", EQwackSDKHTTPType::GET);

// currency
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetCurrencies = SetupEndpoints("/currency", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetCurrencyByID = SetupEndpoints("/currency/{currency_id}", EQwackSDKHTTPType::GET);

// documents
FSQwackFlockEndpoints UQwackFlockGameEndpoints::CreateDocument = SetupEndpoints("/document", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetDockuments = SetupEndpoints("/document", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::UpdateDocumentsByID = SetupEndpoints("/document/{document_id}", EQwackSDKHTTPType::PUT);


// create report error
FSQwackFlockEndpoints UQwackFlockGameEndpoints::CreateReportError = SetupEndpoints("/report_error", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::UpdateReportErrorByID = SetupEndpoints("/report_error/{error_id}", EQwackSDKHTTPType::PUT);

// get game by id
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetGameByID = SetupEndpoints("/game/{game_id}", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetStudioByID = SetupEndpoints("/studio/{studio_id}", EQwackSDKHTTPType::GET);

// game commands
FSQwackFlockEndpoints UQwackFlockGameEndpoints::CreateGameCommand = SetupEndpoints("/game_command", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetGameCommands = SetupEndpoints("/game_command", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::UpdateGameCommandByID = SetupEndpoints("/game_command/{command_id}", EQwackSDKHTTPType::PUT);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::DeleteGameCommandByID = SetupEndpoints("/game_command/{command_id}", EQwackSDKHTTPType::DELETE);

// execute game command
FSQwackFlockEndpoints UQwackFlockGameEndpoints::ExecuteGameCommandByID = SetupEndpoints("/game_command_execution", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetGameCommandExecutions = SetupEndpoints("/game_command_execution", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetGameCommandExecutionByID = SetupEndpoints("/game_command_execution/{execution_id}", EQwackSDKHTTPType::GET);

// game events
FSQwackFlockEndpoints UQwackFlockGameEndpoints::CreateGameEvent = SetupEndpoints("/game-event", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetGameEvents = SetupEndpoints("/game-event", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::UpdateGameEventByID = SetupEndpoints("/game-event/{event_id}", EQwackSDKHTTPType::PUT);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::DeleteGameEventByID = SetupEndpoints("/game-event/{event_id}", EQwackSDKHTTPType::DELETE);



// game versions
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetGameVersionByID = SetupEndpoints("/game_version/{version_id}", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetGameVersionsByGameID = SetupEndpoints("/game/{game_id}/versions", EQwackSDKHTTPType::GET);


FSQwackFlockEndpoints UQwackFlockGameEndpoints::CreateLeaderboard = SetupEndpoints("/leaderboard", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetLeaderboards = SetupEndpoints("/leaderboard", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetLeaderboardByID = SetupEndpoints("/leaderboard/{leaderboard_id}", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::UpdateLeaderboardByID = SetupEndpoints("/leaderboard/{leaderboard_id}", EQwackSDKHTTPType::PUT);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::DeleteLeaderboardByID = SetupEndpoints("/leaderboard/{leaderboard_id}", EQwackSDKHTTPType::DELETE);

// Patches
FSQwackFlockEndpoints UQwackFlockGameEndpoints::CreatePatch = SetupEndpoints("/patch", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetPatches = SetupEndpoints("/patch", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetPatchByID = SetupEndpoints("/patch/{patch_id}", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::UpdatePatchByID = SetupEndpoints("/patch/{patch_id}", EQwackSDKHTTPType::PUT);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::DeletePatchByID = SetupEndpoints("/patch/{patch_id}", EQwackSDKHTTPType::DELETE);


// Player login endpoints

FSQwackFlockEndpoints UQwackFlockGameEndpoints::PlayerLogin = SetupEndpoints("/Player/login", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::LinkAuthUsingLoginType = SetupEndpoints("/player/{player_id}/link-auth/{login_type}", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetPlayers = SetupEndpoints("/player", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetPlayerByID = SetupEndpoints("/player/{player_id}", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::UpdatePlayerByID = SetupEndpoints("/player/{player_id}", EQwackSDKHTTPType::PUT);
// Player data

FSQwackFlockEndpoints UQwackFlockGameEndpoints::CreatePlayerData = SetupEndpoints("/player_data", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetPlayerDatas = SetupEndpoints("/player_data", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetPlayerDataByPlayerID = SetupEndpoints("/player/{player_id}", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::UpdatePlayerDataByDataID = SetupEndpoints("/player_data/{player_id}", EQwackSDKHTTPType::PUT);


// player templates
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetPlayerTemplates = SetupEndpoints("/player_data_template", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetPlayerTemplateByID = SetupEndpoints("/player_data_template/{template_id}", EQwackSDKHTTPType::GET);

// segments
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetSegments = SetupEndpoints("/segment", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetSegmentByID = SetupEndpoints("/segment/{segment_id}", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::CreateSegment = SetupEndpoints("/segment", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::UpdateSegmentByID = SetupEndpoints("/segment/{segment_id}", EQwackSDKHTTPType::PUT);

// shop
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetShops = SetupEndpoints("/shop", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetShopByID = SetupEndpoints("/shop/{shop_id}", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::CreateShop = SetupEndpoints("/shop", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::UpdateShopByID = SetupEndpoints("/shop/{shop_id}", EQwackSDKHTTPType::PUT);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::DeleteShopByID = SetupEndpoints("/shop/{shop_id}", EQwackSDKHTTPType::DELETE);
// SHOP ITEMS
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetShopItems = SetupEndpoints("{shop_id}/shop_item", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetShopItemByID = SetupEndpoints("{shop_id}/shop_item/{item_id}", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::CreateShopItem = SetupEndpoints("{shop_id}/shop_item/", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::UpdateShopItemByID = SetupEndpoints("{shop_id}/shop_item/{shop_item_id}", EQwackSDKHTTPType::PUT);


// studio memembers
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetStudioMemberships = SetupEndpoints("/studio_membership", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetStudioMembershipByID = SetupEndpoints("/studio_member/{studio_membershop_id}", EQwackSDKHTTPType::GET);

// studio team
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetStudioTeam = SetupEndpoints("/studio_team", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetStudioTeamByID = SetupEndpoints("/studio_team/{studio_team_id}", EQwackSDKHTTPType::GET);

FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetThirdPartyIntegrations = SetupEndpoints("/third_party_integration", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetThirdPartyIntegrationByID = SetupEndpoints("/third_party_integration/{third_party_integratio_id}", EQwackSDKHTTPType::GET);

//user assets
FSQwackFlockEndpoints UQwackFlockGameEndpoints::CreateUserAsset = SetupEndpoints("/user_asset", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetUserAssets = SetupEndpoints("/user_asset", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetUserAssetByID = SetupEndpoints("/user_asset/{user_asset_id}", EQwackSDKHTTPType::GET);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::UpdateUserAssetByID = SetupEndpoints("/user_asset/{user_asset_id}", EQwackSDKHTTPType::PUT);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::DeleteUserAssetByID = SetupEndpoints("/user_asset/{user_asset_id}", EQwackSDKHTTPType::DELETE);



FSQwackFlockEndpoints UQwackFlockGameEndpoints::SetupEndpoints(const FString& Endpoint, EQwackSDKHTTPType type)
{
	
	
	FSQwackFlockEndpoints RequestUrl;
	RequestUrl.EndPoint = GameBaseURI + Endpoint;
	RequestUrl.RequestType = type;
	return RequestUrl;
	
}

FSQwackFlockEndpoints UQwackFlockGameEndpoints::AuthEndpoint(const FString& Endpoint, EQwackSDKHTTPType type)
{
	
	FSQwackFlockEndpoints RequestUrl;
	RequestUrl.EndPoint = AuthBaseURI + Endpoint;
	RequestUrl.RequestType = type;
	return RequestUrl;
	
}
