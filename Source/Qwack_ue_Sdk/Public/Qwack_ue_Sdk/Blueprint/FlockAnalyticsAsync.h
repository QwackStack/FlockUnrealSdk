// One Blueprint node per /v1/analytics call. Each node has "On Success" / "On Failed"
// exec pins carrying the response; no subsystem fetch or delegate binding required.
// Events/transactions are write-ahead cached by the SDK, so a transient failure still
// gets retried later even though this node reports On Failed for the immediate attempt.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Qwack_ue_Sdk/Blueprint/FlockBlueprintLibrary.h"
#include "FlockAnalyticsAsync.generated.h"

UCLASS()
class QWACK_UE_SDK_API UFlockStartSessionAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FFlockSessionStartResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockSessionStartResultPin OnFailed;

	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Start Session"))
	static UFlockStartSessionAsync* FlockStartSession(const UObject* WorldContextObject, const FFlockSessionStartRequest& Request);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleResponse(const FFlockSessionStartResponse& Response);

	TWeakObjectPtr<const UObject> WorldContext;
	UPROPERTY()
	FFlockSessionStartRequest Req;
};

UCLASS()
class QWACK_UE_SDK_API UFlockEndSessionAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnFailed;

	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock End Session"))
	static UFlockEndSessionAsync* FlockEndSession(const UObject* WorldContextObject, const FString& SessionId, const FFlockSessionEndRequest& Request);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleResponse(const FFlockGenericResponse& Response);

	TWeakObjectPtr<const UObject> WorldContext;
	UPROPERTY()
	FString SessionId;
	UPROPERTY()
	FFlockSessionEndRequest Req;
};

UCLASS()
class QWACK_UE_SDK_API UFlockTrackEventAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnFailed;

	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Track Event"))
	static UFlockTrackEventAsync* FlockTrackEvent(const UObject* WorldContextObject, const FFlockAnalyticsEventRequest& Request);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleResponse(const FFlockGenericResponse& Response);

	TWeakObjectPtr<const UObject> WorldContext;
	UPROPERTY()
	FFlockAnalyticsEventRequest Req;
};

UCLASS()
class QWACK_UE_SDK_API UFlockTrackEventsAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnFailed;

	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Track Events"))
	static UFlockTrackEventsAsync* FlockTrackEvents(const UObject* WorldContextObject, const FFlockAnalyticsEventsRequest& Request);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleResponse(const FFlockGenericResponse& Response);

	TWeakObjectPtr<const UObject> WorldContext;
	UPROPERTY()
	FFlockAnalyticsEventsRequest Req;
};

UCLASS()
class QWACK_UE_SDK_API UFlockRecordTransactionAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockGenericResultPin OnFailed;

	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Record Transaction"))
	static UFlockRecordTransactionAsync* FlockRecordTransaction(const UObject* WorldContextObject, const FFlockTransactionRequest& Request);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleResponse(const FFlockGenericResponse& Response);

	TWeakObjectPtr<const UObject> WorldContext;
	UPROPERTY()
	FFlockTransactionRequest Req;
};
