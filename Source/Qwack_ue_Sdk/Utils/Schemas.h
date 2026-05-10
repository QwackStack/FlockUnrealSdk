#pragma once

#include "CoreMinimal.h"
#include "Qwack_ue_Sdk/HTTPClient/HTTPResponse.h"
#include "UObject/Object.h"
#include "Schemas.generated.h"

/*
 *Generic Flock API Response Schema for Qwack SDK
 *Most Flock API responses will follow this structure where:
 * error - indicates if there was an error
 * response - contains additional info
 * result contains the actual data returned by the API
 * This won't be exposed to BP to keep it clean and let c++ do dirty work behind the scenes and instead will define much cleaner struct 
 */
USTRUCT()
struct FFLockApiEnvelope
{
	GENERATED_BODY()
	UPROPERTY() FString error_json = "";
	UPROPERTY() FString response_json;
	UPROPERTY() FString result_json;
	
};

USTRUCT(BlueprintType)
struct FFlockOpResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category="Flock|Response") bool bSuccess = false;
	UPROPERTY(BlueprintReadWrite, Category="Flock|Response") int32 StatusCode = 0;
	UPROPERTY(BlueprintReadWrite, Category="Flock|Response") FString ErrorMessage ;
	UPROPERTY(BlueprintReadWrite, Category="Flock|Response") FString ResultJson ;
	
};

/*
* "platforms": [
	{
	  "id": "string",
	  "name": "string",
	  "external_id": "string"
	}
	  ],
	Flock Platform Schema for Qwack SDK
 */
USTRUCT(BlueprintType)
struct FFlockPlatform
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category="Flock|Platform") FString id = "";
	UPROPERTY(BlueprintReadWrite, Category="Flock|Platform") FString name = "";
	UPROPERTY(BlueprintReadWrite, Category="Flock|Platform") FString external_id = "";
	
};
/*
 *
 *Request Schema for Qwack SDK
*{
  "name": "string",
  "platforms": [
	{
	  "id": "string",
	  "name": "string",
	  "external_id": "string"
	}
  ],
  "game_id": "string"
}
 */

/**
 * Request payload used to create a new Achievement in Flock.
 *
 * This struct maps directly to the POST /achievement endpoint.
 *
 * Usage notes:
 * - `name` is the display name of the achievement.
 * - `platforms` defines which platforms this achievement applies to
 *   (each platform includes its id, name, and external_id).
 * - `game_id` must match the target game this achievement belongs to.
 *
 */
USTRUCT(BlueprintType)
struct FFlockCreateAchievementRequest
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Achievements") FString name;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Achievements") TArray<FFlockPlatform> platforms;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Achievements") FString game_id;
	
	
};
/*
 *I do not like the fact that we had to create a separate struct for update when the fields are exactly the same as create, but to be discussed later
 *the reason i created separate struct is to avoid confusion in the future if the fields diverge, and if any changes happen to one struct it won't affect the other
 */ 
USTRUCT(BlueprintType)
struct FFlockUpdateAchievementRequest
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Achievements") FString name;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Achievements") TArray<FFlockPlatform> platforms;
	
};


/*
 *	
*/
USTRUCT(BlueprintType)
struct FFlockAchievement
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category="Flock|Achievement") FString id;
	UPROPERTY(BlueprintReadWrite, Category="Flock|Achievement") FString name;
	UPROPERTY(BlueprintReadWrite, Category="Flock|Achievement") TArray<FFlockPlatform> platforms;
	UPROPERTY(BlueprintReadWrite, Category="Flock|Achievement") FString game_id;
	UPROPERTY(BlueprintReadWrite, Category="Flock|Achievement") FString created_at;
	UPROPERTY(BlueprintReadWrite, Category="Flock|Achievement") FString updated_at;
	
};

USTRUCT(BlueprintType)
struct FFlockAchievementPage
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category="Flock|Achievement") TArray<FFlockAchievement> items;
	UPROPERTY(BlueprintReadWrite, Category="Flock|Achievement") int32 total = 0;
	UPROPERTY(BlueprintReadWrite, Category="Flock|Achievement") int32 page = 1;
	UPROPERTY(BlueprintReadWrite, Category="Flock|Achievement") int32 limit = 10;
	
};

// Response Schema for Qwack SDK developer would be using this to parse the response this is for single achievement response
USTRUCT(BlueprintType)
struct FFlockAchievementSingleResponse : public FQwackHTTPResponse
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category="Flock|Response") FFlockOpResult Meta; // Generic API response envelope errors and stuff
	UPROPERTY(BlueprintReadWrite, Category="Flock|Achievement") FFlockAchievement Achievement; // Actual Achievement data
	
};
// Response Schema for Qwack SDK developer would be using this to parse the response this is for paginated achievement response
USTRUCT(BlueprintType)
struct FFlockAchievementPageResponse
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category="Flock|Response") FFlockOpResult Meta; // Generic API response envelope errors and stuff
	UPROPERTY(BlueprintReadWrite, Category="Flock|Achievement") FFlockAchievementPage AchievementPage; // Actual Achievement data
	
};

USTRUCT(BlueprintType)
struct FQwackEmptyRequest
{
	GENERATED_BODY()
	
};
// we get to templates later
