// /v1/log_event endpoints — debug / logic_error / exception. Owns a disk-backed
// FFlockEventSpool for write-ahead retry. Binds to UQwackAuthSubsystem::OnAccessTokenChanged
// to drain the spool whenever a fresh token arrives.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Qwack_ue_Sdk/Utils/Schemas.h"
#include "QwackLogEventSubsystem.generated.h"

class FFlockEventSpool;
struct FQwackHTTPResponse;

UCLASS()
class QWACK_UE_SDK_API UQwackLogEventSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	DECLARE_DYNAMIC_DELEGATE_OneParam(FFlockOnGenericResponse, const FFlockGenericResponse&, Response);

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
	TUniquePtr<FFlockEventSpool> Spool;

	UFUNCTION()
	void HandleAccessTokenChanged(const FString& Token);

	void OnSpoolResponse(const TArray<FString>& Handles, const FQwackHTTPResponse& R);
	void FlushSpoolAsBatch();

	// Non-static: needs GetGameInstance() to reach the context subsystem.
	FString SerializeEvent(const FFlockLogEventRequest& Req) const;
	static FString BuildBatchPayload(const TArray<FString>& Bodies);
};
