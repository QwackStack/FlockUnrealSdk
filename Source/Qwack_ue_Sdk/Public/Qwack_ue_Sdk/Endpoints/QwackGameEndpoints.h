// Flock v1 endpoint table — auth + analytics + log_event only.
// EndPoint holds the path; the subsystem prepends UQwackConfig::GameBaseURI at request time.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "QwackGameEndpoints.generated.h"

UENUM(BlueprintType)
enum class EQwackSDKHTTPType : uint8
{
	GET    = 0 UMETA(DisplayName = "GET"),
	POST   = 1 UMETA(DisplayName = "POST"),
	PUT    = 2 UMETA(DisplayName = "PUT"),
	PATCH  = 3 UMETA(DisplayName = "PATCH"),
	DELETE = 4 UMETA(DisplayName = "DELETE")
};

USTRUCT(BlueprintType)
struct FSQwackFlockEndpoints
{
	GENERATED_BODY()

	// Path only, e.g. "/v1/player/login". May contain placeholders like "{session_id}"
	// which callers must substitute before sending the request.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "QwackCoreSDK|Endpoints")
	FString EndPoint;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "QwackCoreSDK|HTTP")
	EQwackSDKHTTPType RequestType = EQwackSDKHTTPType::GET;
};

UCLASS()
class QWACK_UE_SDK_API UQwackFlockGameEndpoints : public UObject
{
	GENERATED_BODY()
public:
	// ---- Auth ----
	static FSQwackFlockEndpoints PlayerLogin;          // POST /v1/player/login
	static FSQwackFlockEndpoints PlayerRegister;       // POST /v1/player/register
	static FSQwackFlockEndpoints PlayerTokenRefresh;   // POST /v1/player/token/refresh
	static FSQwackFlockEndpoints PlayerAuthTest;       // GET  /v1/player/auth-test

	// ---- Analytics ----
	static FSQwackFlockEndpoints StartSession;         // POST  /v1/analytics/sessions
	static FSQwackFlockEndpoints EndSession;           // PATCH /v1/analytics/sessions/{session_id}
	static FSQwackFlockEndpoints TrackEvent;           // POST  /v1/analytics/events/single
	static FSQwackFlockEndpoints TrackEvents;          // POST  /v1/analytics/events
	static FSQwackFlockEndpoints RecordTransaction;    // POST  /v1/analytics/transactions

	// ---- Log events ----
	static FSQwackFlockEndpoints LogEvent;             // POST /v1/log_event/single
	static FSQwackFlockEndpoints LogEvents;            // POST /v1/log_event

	static const TCHAR* QwackHttpVerb(EQwackSDKHTTPType T);

private:
	static FSQwackFlockEndpoints Make(const FString& Path, EQwackSDKHTTPType T);
};
