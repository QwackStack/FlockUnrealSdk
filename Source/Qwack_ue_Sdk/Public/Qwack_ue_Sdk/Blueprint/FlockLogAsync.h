// One Blueprint node per /v1/log_event call. Each node has "On Success" / "On Failed"
// exec pins carrying the response; no subsystem fetch or delegate binding required.
// LogDebug / LogError / LogException are the simple plain-string nodes; LogEvent /
// LogEvents take the full request struct for advanced control. Log events are
// write-ahead cached, so a transient failure is still retried later by the SDK.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Qwack_ue_Sdk/Blueprint/FlockBlueprintLibrary.h"
#include "FlockLogAsync.generated.h"

UCLASS()
class QWACK_UE_SDK_API UFlockLogDebugAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnFailed;

	// ExtraDataJson is a free-form JSON object string (use Make Flock Json Object,
	// or leave empty). type is set to "debug" automatically.
	UFUNCTION(BlueprintCallable, Category = "Flock|Log",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Log Debug",
		        AdvancedDisplay = "ExtraDataJson"))
	static UFlockLogDebugAsync* FlockLogDebug(const UObject* WorldContextObject, const FString& Message, const FString& ExtraDataJson);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleResponse(const FFlockGenericResponse& Response);

	TWeakObjectPtr<const UObject> WorldContext;
	UPROPERTY()
	FString Message;
	UPROPERTY()
	FString ExtraDataJson;
};

UCLASS()
class QWACK_UE_SDK_API UFlockLogErrorAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnFailed;

	// LogicalExpression is the assertion / invariant that failed. type = "logic_error".
	UFUNCTION(BlueprintCallable, Category = "Flock|Log",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Log Error",
		        AdvancedDisplay = "ExtraDataJson"))
	static UFlockLogErrorAsync* FlockLogError(const UObject* WorldContextObject, const FString& Message, const FString& LogicalExpression, const FString& ExtraDataJson);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleResponse(const FFlockGenericResponse& Response);

	TWeakObjectPtr<const UObject> WorldContext;
	UPROPERTY()
	FString Message;
	UPROPERTY()
	FString LogicalExpression;
	UPROPERTY()
	FString ExtraDataJson;
};

UCLASS()
class QWACK_UE_SDK_API UFlockLogExceptionAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnFailed;

	// Traceback is a free-form string; ErrorDataJson / ExtraDataJson are JSON object
	// strings. type = "exception".
	UFUNCTION(BlueprintCallable, Category = "Flock|Log",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Log Exception",
		        AdvancedDisplay = "Traceback,ErrorDataJson,ExtraDataJson"))
	static UFlockLogExceptionAsync* FlockLogException(const UObject* WorldContextObject, const FString& Message, const FString& ErrorMessage, const FString& ErrorCode, const FString& Traceback, const FString& ErrorDataJson, const FString& ExtraDataJson);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleResponse(const FFlockGenericResponse& Response);

	TWeakObjectPtr<const UObject> WorldContext;
	UPROPERTY()
	FString Message;
	UPROPERTY()
	FString ErrorMessage;
	UPROPERTY()
	FString ErrorCode;
	UPROPERTY()
	FString Traceback;
	UPROPERTY()
	FString ErrorDataJson;
	UPROPERTY()
	FString ExtraDataJson;
};

UCLASS()
class QWACK_UE_SDK_API UFlockLogEventAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnFailed;

	UFUNCTION(BlueprintCallable, Category = "Flock|Log",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Log Event"))
	static UFlockLogEventAsync* FlockLogEvent(const UObject* WorldContextObject, const FFlockLogEventRequest& Request);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleResponse(const FFlockGenericResponse& Response);

	TWeakObjectPtr<const UObject> WorldContext;
	UPROPERTY()
	FFlockLogEventRequest Req;
};

UCLASS()
class QWACK_UE_SDK_API UFlockLogEventsAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnFailed;

	UFUNCTION(BlueprintCallable, Category = "Flock|Log",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Log Events"))
	static UFlockLogEventsAsync* FlockLogEvents(const UObject* WorldContextObject, const FFlockLogEventsRequest& Request);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleResponse(const FFlockGenericResponse& Response);

	TWeakObjectPtr<const UObject> WorldContext;
	UPROPERTY()
	FFlockLogEventsRequest Req;
};
