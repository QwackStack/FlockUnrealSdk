// Flock v1 schemas — auth, analytics, log_event.
// Field names mirror the JSON keys in the OpenAPI spec to keep serialization 1:1.

#pragma once

#include "CoreMinimal.h"
#include "Qwack_ue_Sdk/HTTPClient/HTTPResponse.h"
#include "UObject/Object.h"
#include "Schemas.generated.h"

// =====================================================================
// Common
// =====================================================================

USTRUCT(BlueprintType)
struct FFlockOpResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Response")
	bool bSuccess = false;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Response")
	int32 StatusCode = 0;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Response")
	FString ErrorMessage;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Response")
	FString ResultJson;
};

USTRUCT(BlueprintType)
struct FFlockGenericResponse
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Response")
	FFlockOpResult Meta;
};

// =====================================================================
// Auth — POST /v1/player/login, /register, /token/refresh, GET /auth-test
// =====================================================================

UENUM(BlueprintType)
enum class EFlockLoginType : uint8
{
	device_id UMETA(DisplayName = "device_id"),
	email UMETA(DisplayName = "email"),
	google UMETA(DisplayName = "google"),
	apple UMETA(DisplayName = "apple"),
	facebook UMETA(DisplayName = "facebook"),
	steam UMETA(DisplayName = "steam"),
	discord UMETA(DisplayName = "discord")
};

USTRUCT(BlueprintType)
struct FFlockPlayerLoginRequest
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	EFlockLoginType login_type = EFlockLoginType::email;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString email;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString password;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString google_id;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString device_id;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString device_type;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString apple_id;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString facebook_id;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString steam_id;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString discord_id;
};

USTRUCT(BlueprintType)
struct FFlockPlayerRegisterRequest
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString email;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString password;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString name;
};

USTRUCT(BlueprintType)
struct FFlockPlayerRefreshRequest
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString player_id;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString refresh_token;
};

// Returned by login / register / token-refresh.
USTRUCT(BlueprintType)
struct FFlockPlayerAuth
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString player_id;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString access_token;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString refresh_token;
};

USTRUCT(BlueprintType)
struct FFlockPlayerAuthResponse
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Response")
	FFlockOpResult Meta;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FFlockPlayerAuth Auth;
};

// GET /v1/player/auth-test returns a free-form "Requester" object — kept as raw JSON.
USTRUCT(BlueprintType)
struct FFlockAuthTestResponse
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Response")
	FFlockOpResult Meta;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Auth")
	FString RequesterJson;
};

// =====================================================================
// Sessions — POST /v1/analytics/sessions, PATCH /v1/analytics/sessions/{id}
// =====================================================================

USTRUCT(BlueprintType)
struct FFlockSessionStartRequest
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Sessions")
	FString player_id;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Sessions")
	FString platform;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Sessions")
	FString device_type;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Sessions")
	FString game_version_id;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Sessions")
	FString started_at; // ISO-8601
};

USTRUCT(BlueprintType)
struct FFlockSessionEndRequest
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Sessions")
	int32 duration_seconds = 0;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Sessions")
	int32 screens_viewed = 0;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Sessions")
	bool is_bounce = false;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Sessions")
	FString ended_at; // ISO-8601
};

USTRUCT(BlueprintType)
struct FFlockSessionStartResponse
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Response")
	FFlockOpResult Meta;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Sessions")
	FString session_id;
};

// =====================================================================
// Analytics events — POST /v1/analytics/events, /events/single
// =====================================================================

USTRUCT(BlueprintType)
struct FFlockAnalyticsEventRequest
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Analytics")
	FString player_id;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Analytics")
	FString event_name;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Analytics")
	FString event_category;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Analytics")
	FString session_id;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Analytics")
	FString timestamp; // ISO-8601

	// Free-form JSON object encoded as a string. e.g. {"score":100,"level":3}. Empty string serializes as {}.
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Analytics")
	FString PropertiesJson;
};

USTRUCT(BlueprintType)
struct FFlockAnalyticsEventsRequest
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Analytics")
	TArray<FFlockAnalyticsEventRequest> events;
};

// =====================================================================
// Transactions — POST /v1/analytics/transactions
// =====================================================================

USTRUCT(BlueprintType)
struct FFlockTransactionRequest
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Transactions")
	FString player_id;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Transactions")
	float amount = 0.f;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Transactions")
	FString currency_id;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Transactions")
	FString currency_code;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Transactions")
	FString session_id;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Transactions")
	FString shop_item_id;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Transactions")
	int32 quantity = 1;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Transactions")
	FString transaction_type;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Transactions")
	FString status;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Transactions")
	FString payment_provider;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Transactions")
	FString external_transaction_id;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Transactions")
	FString created_at;
};

// =====================================================================
// Log events — POST /v1/log_event, /v1/log_event/single
// =====================================================================

UENUM(BlueprintType)
enum class EFlockLogEventType : uint8
{
	exception UMETA(DisplayName = "exception"),
	logic_error UMETA(DisplayName = "logic_error"),
	debug UMETA(DisplayName = "debug")
};

USTRUCT(BlueprintType)
struct FFlockLogEventData
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Log")
	EFlockLogEventType type = EFlockLogEventType::debug;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Log")
	FString game_version;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Log")
	FString logical_expression;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Log")
	FString error_message;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Log")
	FString error_code;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Log")
	FString error_data; // free-form JSON string
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Log")
	FString error_traceback;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Log")
	TArray<FString> error_traceback_lines;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Log")
	FString extra_data; // free-form JSON string
};

USTRUCT(BlueprintType)
struct FFlockLogEventRequest
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Log")
	FString message;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Log")
	FFlockLogEventData data;
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Log")
	FString timestamp; // ISO-8601
};

USTRUCT(BlueprintType)
struct FFlockLogEventsRequest
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category = "Flock|Log")
	TArray<FFlockLogEventRequest> events;
};
