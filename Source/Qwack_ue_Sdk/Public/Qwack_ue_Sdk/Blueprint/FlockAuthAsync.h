// One Blueprint node per /v1/player auth call. Each node has "On Success" / "On Failed"
// exec pins carrying the response; no subsystem fetch or delegate binding required.
// Successful login / register / refresh also stores the tokens in the SDK automatically.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Qwack_ue_Sdk/Blueprint/FlockBlueprintLibrary.h"
#include "FlockAuthAsync.generated.h"

UCLASS()
class QWACK_UE_SDK_API UFlockRegisterPlayerAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FFlockAuthResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockAuthResultPin OnFailed;

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Register Player"))
	static UFlockRegisterPlayerAsync* FlockRegisterPlayer(const UObject* WorldContextObject, const FFlockPlayerRegisterRequest& Request);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleResponse(const FFlockPlayerAuthResponse& Response);

	TWeakObjectPtr<const UObject> WorldContext;
	UPROPERTY()
	FFlockPlayerRegisterRequest Req;
};

UCLASS()
class QWACK_UE_SDK_API UFlockLoginPlayerAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FFlockAuthResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockAuthResultPin OnFailed;

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Login Player"))
	static UFlockLoginPlayerAsync* FlockLoginPlayer(const UObject* WorldContextObject, const FFlockPlayerLoginRequest& Request);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleResponse(const FFlockPlayerAuthResponse& Response);

	TWeakObjectPtr<const UObject> WorldContext;
	UPROPERTY()
	FFlockPlayerLoginRequest Req;
};

UCLASS()
class QWACK_UE_SDK_API UFlockRefreshTokenAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FFlockAuthResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockAuthResultPin OnFailed;

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Refresh Token"))
	static UFlockRefreshTokenAsync* FlockRefreshToken(const UObject* WorldContextObject, const FFlockPlayerRefreshRequest& Request);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleResponse(const FFlockPlayerAuthResponse& Response);

	TWeakObjectPtr<const UObject> WorldContext;
	UPROPERTY()
	FFlockPlayerRefreshRequest Req;
};

UCLASS()
class QWACK_UE_SDK_API UFlockAuthTestAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FFlockAuthTestResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockAuthTestResultPin OnFailed;

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Auth Test"))
	static UFlockAuthTestAsync* FlockAuthTest(const UObject* WorldContextObject);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleResponse(const FFlockAuthTestResponse& Response);

	TWeakObjectPtr<const UObject> WorldContext;
};
