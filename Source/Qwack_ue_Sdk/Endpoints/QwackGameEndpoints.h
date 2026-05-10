// Copyright 2022, Qwack. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "QwackGameEndpoints.generated.h"


UENUM(BlueprintType)
enum class EQwackSDKHTTPType : uint8
{
	GET = 0         UMETA(DisplayName = "GET"),
	POST = 1        UMETA(DisplayName = "POST"),
	DELETE = 2      UMETA(DisplayName = "DELETE"),
	PUT = 3         UMETA(DisplayName = "PUT"),
	HEAD = 4        UMETA(DisplayName = "HEAD"),
	CREATE = 5      UMETA(DisplayName = "CREATE"),
	OPTIONS = 6     UMETA(DisplayName = "OPTIONS"),
	PATCH = 7       UMETA(DisplayName = "PATCH"),
	UPLOAD = 8      UMETA(DisplayName = "UPLOAD")
};


USTRUCT(BlueprintType)
struct FSQwackFlockEndpoints
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintReadWrite, EditAnywhere, BlueprintReadWrite, Category = "QwackCoreSDK | Endpoints")
	FString EndPoint;
	UPROPERTY(BlueprintReadWrite, EditAnywhere, BlueprintReadWrite, Category = "QwackCoreSDK | HTTP Type")
	EQwackSDKHTTPType RequestType = EQwackSDKHTTPType::GET;
};
/**
 * 
 */
UCLASS()
class QWACK_UE_SDK_API UQwackFlockGameEndpoints : public UObject
{
	GENERATED_BODY()
public:

	// Achievements Endpoints
	static FSQwackFlockEndpoints CreateAchievement;
	static FSQwackFlockEndpoints GetAchievements; // list achievements (all)
	static FSQwackFlockEndpoints GetAchievementByID; // get achievement by id
	
	static FSQwackFlockEndpoints DeleteAchievement;
	static FSQwackFlockEndpoints UpdateAchievement;


	// assets Endpoints
	static FSQwackFlockEndpoints CreateAssets;// create asset
	static FSQwackFlockEndpoints GetAssets;// list assets (all)
	static FSQwackFlockEndpoints GetAssetByID; // get asset by id
	static FSQwackFlockEndpoints DeleteAssetByID; // delete asset by id


	// bans Endpoints
	static FSQwackFlockEndpoints CreateBan;// create ban for a player 
	static FSQwackFlockEndpoints GetBans;// list bans (all)
	static FSQwackFlockEndpoints UpdateBansByID; // update ban by id
	// I think we need unban or get ban or whatever

	

	// GameConfig Endpoints
	static FSQwackFlockEndpoints CreateGameConfig;// create game config
	static FSQwackFlockEndpoints GetGameConfigs;// list game configs (all)
	static FSQwackFlockEndpoints UpdateGameConfigByID; // update game config by id
	static FSQwackFlockEndpoints GetGameConfigByID; // get game config by id
	

	// currencies CREATE FROM DASHBOARD NOT UE
	static FSQwackFlockEndpoints GetCurrencies; // list currencies (all)
	static FSQwackFlockEndpoints GetCurrencyByID; // get currency by id
	
	//Documents

	static FSQwackFlockEndpoints CreateDocument;// create document
	static FSQwackFlockEndpoints GetDockuments;// list documents (all)
	static FSQwackFlockEndpoints UpdateDocumentsByID;// Update document by id


	//EmailTemplates
	// not necessary to be honest in game not sure if we need it as this may need ot be done inside the dashboard why unreal engine need to create emails!
	//Just keeping this note for you @Anas
	//static FSQwackFlockEndpoints CreateEmailTemplate;// create document


	
	// Report Error Endpoints

	//These may be internal and Game developers may not need to access them directly from unreal engine and we report it to backend directly and developer can see it ? so gett and delete may not be necessary?
	static FSQwackFlockEndpoints CreateReportError;// report error
	static FSQwackFlockEndpoints UpdateReportErrorByID;// update report error by id

	// Games Endpoints
	// I do not see create game necessary from unreal engine side as game is already created from dashboard but get may be necessary

	/*
	 * here is the plan no wrappers or any thing is needed just config file generated from dashbaord 
	 */
	static FSQwackFlockEndpoints GetGameByID; // get game by id
	static FSQwackFlockEndpoints GetStudioByID; // Get studio by id


	// Game Commands Endpoints
	static FSQwackFlockEndpoints CreateGameCommand; // create game command
	static FSQwackFlockEndpoints GetGameCommands; // list game commands (all)
	static FSQwackFlockEndpoints UpdateGameCommandByID; // update game command by id
	static FSQwackFlockEndpoints DeleteGameCommandByID; // delete game command by id
	static FSQwackFlockEndpoints ExecuteGameCommandByID; // execute game command by id

	// Game Command executions Endpoints (seems like this is going to report to dashboard?) 
	static FSQwackFlockEndpoints CreateGameCommandExecution; // create game command execution
	static FSQwackFlockEndpoints GetGameCommandExecutions; // list game command executions (all)
	static FSQwackFlockEndpoints GetGameCommandExecutionByID; // get game command execution by id


	// Game Evenets Endpoints
	static FSQwackFlockEndpoints CreateGameEvent; // create game event
	static FSQwackFlockEndpoints GetGameEvents; // list game events (all)
	static FSQwackFlockEndpoints UpdateGameEventByID; // update game event by id
	static FSQwackFlockEndpoints DeleteGameEventByID; // delete game event by id
	static FSQwackFlockEndpoints ExecuteGameEventByID; // execute game event by id

	// Game versions Endpoints you cannot create game version from unreal engine side it is created from dashboard when you create new version
	static FSQwackFlockEndpoints GetGameVersionByID; // get game version by version id
	static FSQwackFlockEndpoints GetGameVersionsByGameID; // list game versions (all)



	// Tickets are for dashboard so not defining anything here

	// leaderboards Endpoints
	/* not sure if any developer needs to create leaderboard from unreal engine side ? maybe just get and update scores ? 
	 * let me know your thoughts @Anas
	 * either way I introduced all endpoints here for you to decide
	 */
	static FSQwackFlockEndpoints CreateLeaderboard; // create leaderboard
	static FSQwackFlockEndpoints GetLeaderboards; // list leaderboards (all)
	static FSQwackFlockEndpoints GetLeaderboardByID; // get leaderboard by id
	static FSQwackFlockEndpoints UpdateLeaderboardByID; // update leaderboard by id
	static FSQwackFlockEndpoints DeleteLeaderboardByID; // delete leaderboard by id

	// Patches Endpoints (Not sure what that is but I introduced all endpoints here as well)
	static FSQwackFlockEndpoints CreatePatch; // create patch
	static FSQwackFlockEndpoints GetPatches; // list patches (all)
	static FSQwackFlockEndpoints GetPatchByID; // get patch by id
	static FSQwackFlockEndpoints UpdatePatchByID; // update patch by id
	static FSQwackFlockEndpoints DeletePatchByID; // delete patch by id


	// player login Endpoints ( may not be needed as steam login is enough ? )
	static FSQwackFlockEndpoints PlayerLogin; // player login
	static FSQwackFlockEndpoints CreatePlayer; // create player login (Aka register)
	static FSQwackFlockEndpoints GetPlayerByID; // get player by id
	static FSQwackFlockEndpoints GetPlayers; // list players (all) -> TBH this should not be necessary from unreal engine side
	static FSQwackFlockEndpoints UpdatePlayerByID; // update player by id
	static FSQwackFlockEndpoints LinkAuthUsingLoginType; // Link to QwackFlock account using login type (eg steam,epic etc)
	// player data Endpoints

	
	// something here is not right endpoint for updating player data is data id shouldnt be player id?
	static FSQwackFlockEndpoints CreatePlayerData; // create player data
	static FSQwackFlockEndpoints GetPlayerDatas; // list player data (all)
	static FSQwackFlockEndpoints GetPlayerDataByPlayerID; // get player data by player id
	static FSQwackFlockEndpoints UpdatePlayerDataByDataID; // update player data by player id



	// player templates Endpoints templates created by dashboard not in ue
	static FSQwackFlockEndpoints GetPlayerTemplates; // list player templates (all)
	static FSQwackFlockEndpoints GetPlayerTemplateByID; // get player template by id


	//Segments Endpoints
	static FSQwackFlockEndpoints CreateSegment; // create segment
	static FSQwackFlockEndpoints GetSegments; // list segments (all)
	static FSQwackFlockEndpoints UpdateSegmentByID; // update segment by id
	static FSQwackFlockEndpoints DeleteSegmentByID; // delete segment by id
	static FSQwackFlockEndpoints GetSegmentByID; // get segment by id

	// shops Endpoints
	static FSQwackFlockEndpoints CreateShop; // create shop
	static FSQwackFlockEndpoints GetShops; // list shops (all)
	static FSQwackFlockEndpoints GetShopByID; // get shop by id
	static FSQwackFlockEndpoints UpdateShopByID; // update shop by id
	static FSQwackFlockEndpoints DeleteShopByID; // delete shop by id

	// shop items Endpoints
	static FSQwackFlockEndpoints CreateShopItem; // create shop item
	static FSQwackFlockEndpoints GetShopItems; // list shop items (all)
	static FSQwackFlockEndpoints GetShopItemByID; // get shop item by id
	static FSQwackFlockEndpoints UpdateShopItemByID; // update shop item by id

	// studio membership Endpoints rest of endpoints are for dashboard only not for unreal engine side
	static FSQwackFlockEndpoints GetStudioMemberships; // list studio memberships (all)
	static FSQwackFlockEndpoints GetStudioMembershipByID; // get studio membership by id

	// stduio team members ( may be needed for credits so I will include gets only here) rest for dashboard only
	static FSQwackFlockEndpoints GetStudioTeam; // list studio team members (all)
	static FSQwackFlockEndpoints GetStudioTeamByID; // get studio team member by id

	// Third Party Integrations Endpoints (not sure if needed from unreal engine side)
	static FSQwackFlockEndpoints GetThirdPartyIntegrations; // list third party integrations (all)
	static FSQwackFlockEndpoints GetThirdPartyIntegrationByID; // get third party integration by id

	// user assets Endpoints
	static FSQwackFlockEndpoints CreateUserAsset; // create user asset
	static FSQwackFlockEndpoints UpdateUserAssetByID; // update user asset by id
	static FSQwackFlockEndpoints DeleteUserAssetByID; // delete user asset by id
	static FSQwackFlockEndpoints GetUserAssets; // list user assets (all)
	static FSQwackFlockEndpoints GetUserAssetByID; // get user asset by id


	
	static const TCHAR* QwackHttpVerb(EQwackSDKHTTPType Type)
	{
		switch (Type)
		{
		case EQwackSDKHTTPType::GET:
			return TEXT("GET");
		case EQwackSDKHTTPType::POST:
			return TEXT("POST");
		case EQwackSDKHTTPType::DELETE:
			return TEXT("DELETE");
		case EQwackSDKHTTPType::PUT:
			return TEXT("PUT");
		case EQwackSDKHTTPType::HEAD:
			return TEXT("HEAD");
		case EQwackSDKHTTPType::OPTIONS:
			return TEXT("OPTIONS");
		case EQwackSDKHTTPType::PATCH:
			return TEXT("PATCH");
		case EQwackSDKHTTPType::CREATE:
			return TEXT("POST");
		case EQwackSDKHTTPType::UPLOAD:
			return TEXT("POST");
		default:
			return TEXT("GET");
		}
	}

private:
	static FString GameBaseURI; // ofc base uri will be qwack etc 
	static FString AuthBaseURI; // this is to be updated as well

	static FSQwackFlockEndpoints SetupEndpoints(const FString& Endpooint,EQwackSDKHTTPType type );
	static FSQwackFlockEndpoints AuthEndpoint(const FString& Endpooint,EQwackSDKHTTPType type );

	

};
