// Flock SDK GameInstance subsystem — entry point for v1 auth, analytics, and log_event APIs.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Qwack_ue_Sdk/Auth/QwackAuthSubsystem.h"
#include "Qwack_ue_Sdk/Cache/FlockEventSpool.h"
#include "Qwack_ue_Sdk/Utils/Schemas.h"
#include "QwackFlockSubsystem.generated.h"

class USHTTPClient;
struct FSQwackFlockEndpoints;

UCLASS()
class QWACK_UE_SDK_API UQwackFlockSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ---------------- Auth state ----------------

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth")
	void SetAccessToken(const FString& InAccessToken);

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth")
	FString GetAccessToken() const { return AccessToken; }

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth")
	FString GetRefreshToken() const { return RefreshTokenValue; }

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth")
	FString GetPlayerId() const { return PlayerId; }

	UPROPERTY(BlueprintAssignable, Category = "Flock|Auth")
	FFlockOnAuthChanged OnAccessTokenChanged;

	// ---------------- Callback delegate types ----------------

	DECLARE_DYNAMIC_DELEGATE_OneParam(FFlockOnAuthResponse,     const FFlockPlayerAuthResponse&,    Response);
	DECLARE_DYNAMIC_DELEGATE_OneParam(FFlockOnAuthTestResponse, const FFlockAuthTestResponse&,      Response);
	DECLARE_DYNAMIC_DELEGATE_OneParam(FFlockOnSessionStart,     const FFlockSessionStartResponse&,  Response);
	DECLARE_DYNAMIC_DELEGATE_OneParam(FFlockOnGenericResponse,  const FFlockGenericResponse&,       Response);

	// ---------------- Auth — /v1/player ----------------

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth")
	void RegisterPlayer(const FFlockPlayerRegisterRequest& Request, const FFlockOnAuthResponse& Callback);

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth")
	void LoginPlayer(const FFlockPlayerLoginRequest& Request, const FFlockOnAuthResponse& Callback);

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth")
	void RefreshToken(const FFlockPlayerRefreshRequest& Request, const FFlockOnAuthResponse& Callback);

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth")
	void AuthTest(const FFlockOnAuthTestResponse& Callback);

	// ---------------- Analytics — /v1/analytics ----------------

	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics")
	void StartSession(const FFlockSessionStartRequest& Request, const FFlockOnSessionStart& Callback);

	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics")
	void EndSession(const FString& SessionId, const FFlockSessionEndRequest& Request, const FFlockOnGenericResponse& Callback);

	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics")
	void TrackEvent(const FFlockAnalyticsEventRequest& Request, const FFlockOnGenericResponse& Callback);

	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics")
	void TrackEvents(const FFlockAnalyticsEventsRequest& Request, const FFlockOnGenericResponse& Callback);

	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics")
	void RecordTransaction(const FFlockTransactionRequest& Request, const FFlockOnGenericResponse& Callback);

	// ---------------- Log events — /v1/log_event ----------------

	// Convenience: type = debug. `ExtraDataJson` is a free-form JSON string (e.g. "{\"foo\":1}").
	UFUNCTION(BlueprintCallable, Category = "Flock|Log")
	void LogDebug(const FString& Message, const FString& ExtraDataJson, const FFlockOnGenericResponse& Callback);

	// Convenience: type = logic_error. `LogicalExpression` is the assertion or invariant that failed.
	UFUNCTION(BlueprintCallable, Category = "Flock|Log")
	void LogError(const FString& Message, const FString& LogicalExpression, const FString& ExtraDataJson, const FFlockOnGenericResponse& Callback);

	// Convenience: type = exception. `Traceback` is a free-form string; `ErrorDataJson` and `ExtraDataJson` are JSON strings.
	UFUNCTION(BlueprintCallable, Category = "Flock|Log")
	void LogException(const FString& Message, const FString& ErrorMessage, const FString& ErrorCode, const FString& Traceback, const FString& ErrorDataJson, const FString& ExtraDataJson, const FFlockOnGenericResponse& Callback);

	// Lower-level: full control over the request payload.
	UFUNCTION(BlueprintCallable, Category = "Flock|Log")
	void LogEvent(const FFlockLogEventRequest& Request, const FFlockOnGenericResponse& Callback);

	UFUNCTION(BlueprintCallable, Category = "Flock|Log")
	void LogEvents(const FFlockLogEventsRequest& Request, const FFlockOnGenericResponse& Callback);

private:
	UPROPERTY(Transient) TObjectPtr<USHTTPClient> HttpClient;

	UPROPERTY(Transient) FString AccessToken;
	UPROPERTY(Transient) FString RefreshTokenValue;
	UPROPERTY(Transient) FString PlayerId;

	// Disk-backed write-ahead spools — kept off the UObject graph so the per-event
	// file entries don't add GC walk cost. Null when caching is disabled in settings.
	TUniquePtr<FFlockEventSpool> AnalyticsSpool;
	TUniquePtr<FFlockEventSpool> LogEventSpool;

	enum class EFlockSpool : uint8 { Analytics, LogEvents };

	// --- helpers ---
	FString BuildUrl(const FString& Path) const;
	TMap<FString, FString> MakeHeaders(bool bIncludeAuth) const;
	void StoreAuthFromResponse(const FFlockPlayerAuth& Auth);

	FFlockEventSpool* GetSpool(EFlockSpool Which) const;
	static const FSQwackFlockEndpoints& GetBatchEndpoint(EFlockSpool Which);

	// Apply HTTP response to the spool: drop on success/permanent, leave on transient.
	// Triggers a follow-up flush when there's still pending work after a successful send.
	void OnSpoolResponse(EFlockSpool Which, const TArray<FString>& Handles, const FQwackHTTPResponse& R);

	// Drains one batch from the spool to its batch endpoint. Self-recurses while
	// the spool is non-empty and the previous batch landed.
	void FlushSpoolAsBatch(EFlockSpool Which);

	static FFlockOpResult MakeMeta(const struct FQwackHTTPResponse& R);
	static FString SerializeAnalyticsEvent(const FFlockAnalyticsEventRequest& Req);
	static FString SerializeAnalyticsEvents(const FFlockAnalyticsEventsRequest& Req);
	static FString SerializeLogEvent(const FFlockLogEventRequest& Req);
	static FString SerializeLogEvents(const FFlockLogEventsRequest& Req);

	// Common dispatcher — sends an HTTP request and routes the FQwackHTTPResponse into the callback.
	void Send(const FSQwackFlockEndpoints& Endpoint,
	          const FString& UrlOverride,        // if non-empty, used instead of BuildUrl(Endpoint.EndPoint)
	          const FString& Body,
	          bool bIncludeAuth,
	          TFunction<void(const FQwackHTTPResponse&)> OnDone) const;
};
