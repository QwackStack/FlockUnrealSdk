// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockResult.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Models/FlockAuthModels.h"
#include "FlockAuthAsyncActions.generated.h"

class FFlockAuthProvider;

/**
 * Blueprint async nodes for the auth feature. Each node resolves the SDK from its world context on
 * Activate; when the SDK is unavailable or uninitialized the failure pin fires with a Validation
 * error. Exactly one pin fires per activation. Success/failure pins share one delegate signature
 * per node so wildcard pins line up; the unused half of the payload is default-constructed.
 *
 * Sync state (Is Authenticated, Get Player Id, Logout) lives on UFlockSubsystem directly.
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockLoginPin, const FFlockPlayerLoginResponse&, Response, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockRegisterPin, const FFlockRegisterResult&, Result, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFlockRestorePin);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockAuthAccountPin, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockNameAvailablePin, bool, bAvailable, const FFlockError&, Error);

UCLASS()
class FLOCK_API UFlockLoginAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockLoginPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockLoginPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Login With Email"), Category = "Flock|Auth")
	static UFlockLoginAction* LoginWithEmail(UObject* WorldContextObject, const FString& Email, const FString& Password);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Login With Device"), Category = "Flock|Auth")
	static UFlockLoginAction* LoginWithDevice(UObject* WorldContextObject, const FString& DeviceId);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Login With Google"), Category = "Flock|Auth")
	static UFlockLoginAction* LoginWithGoogle(UObject* WorldContextObject, const FString& IdToken);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Login With Apple"), Category = "Flock|Auth")
	static UFlockLoginAction* LoginWithApple(UObject* WorldContextObject, const FString& IdentityToken);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Login With Steam"), Category = "Flock|Auth")
	static UFlockLoginAction* LoginWithSteam(UObject* WorldContextObject, const FString& SessionTicket);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Login With Facebook"), Category = "Flock|Auth")
	static UFlockLoginAction* LoginWithFacebook(UObject* WorldContextObject, const FString& FacebookId);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Login With Discord"), Category = "Flock|Auth")
	static UFlockLoginAction* LoginWithDiscord(UObject* WorldContextObject, const FString& DiscordId);

	virtual void Activate() override;

private:
	static UFlockLoginAction* Make(UObject* InWorldContextObject,
		TFunction<FFlockRequestHandle(FFlockAuthProvider&, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)>)> InStart);
	void Complete(const TFlockResult<FFlockPlayerLoginResponse>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	TFunction<FFlockRequestHandle(FFlockAuthProvider&, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)>)> Start;
};

UCLASS()
class FLOCK_API UFlockRegisterAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockRegisterPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockRegisterPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Register With Email"), Category = "Flock|Auth")
	static UFlockRegisterAction* RegisterWithEmail(UObject* WorldContextObject, const FString& Email, const FString& Password, const FString& Name);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Register With Device"), Category = "Flock|Auth")
	static UFlockRegisterAction* RegisterWithDevice(UObject* WorldContextObject, const FString& DeviceId, const FString& Name);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Register With Google"), Category = "Flock|Auth")
	static UFlockRegisterAction* RegisterWithGoogle(UObject* WorldContextObject, const FString& IdToken, const FString& Name);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Register With Apple"), Category = "Flock|Auth")
	static UFlockRegisterAction* RegisterWithApple(UObject* WorldContextObject, const FString& IdentityToken, const FString& Name);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Register With Steam"), Category = "Flock|Auth")
	static UFlockRegisterAction* RegisterWithSteam(UObject* WorldContextObject, const FString& SessionTicket, const FString& Name);

	virtual void Activate() override;

private:
	static UFlockRegisterAction* Make(UObject* InWorldContextObject,
		TFunction<FFlockRequestHandle(FFlockAuthProvider&, TFunction<void(TFlockResult<FFlockRegisterResult>)>)> InStart);
	void Complete(const TFlockResult<FFlockRegisterResult>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	TFunction<FFlockRequestHandle(FFlockAuthProvider&, TFunction<void(TFlockResult<FFlockRegisterResult>)>)> Start;
};

UCLASS()
class FLOCK_API UFlockRestoreSessionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** A persisted session was resumed; the player is signed in. */
	UPROPERTY(BlueprintAssignable)
	FFlockRestorePin OnRestored;

	/** No usable persisted session (none stored, unusable tokens, or the SDK was unavailable). */
	UPROPERTY(BlueprintAssignable)
	FFlockRestorePin OnNoSession;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Restore Session"), Category = "Flock|Auth")
	static UFlockRestoreSessionAction* RestoreSession(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;
};

UCLASS()
class FLOCK_API UFlockAuthAccountAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockAuthAccountPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockAuthAccountPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Forgot Password"), Category = "Flock|Auth")
	static UFlockAuthAccountAction* ForgotPassword(UObject* WorldContextObject, const FString& Email);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Reset Password"), Category = "Flock|Auth")
	static UFlockAuthAccountAction* ResetPassword(UObject* WorldContextObject, const FString& Email, const FString& Code, const FString& NewPassword);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Send Email Verification"), Category = "Flock|Auth")
	static UFlockAuthAccountAction* SendEmailVerification(UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Verify Email"), Category = "Flock|Auth")
	static UFlockAuthAccountAction* VerifyEmail(UObject* WorldContextObject, const FString& Code);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Revoke Token"), Category = "Flock|Auth")
	static UFlockAuthAccountAction* RevokeToken(UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Refresh Token"), Category = "Flock|Auth")
	static UFlockAuthAccountAction* RefreshToken(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	static UFlockAuthAccountAction* Make(UObject* InWorldContextObject,
		TFunction<void(FFlockAuthProvider&, TFunction<void(bool, const FFlockError&)>)> InStart);
	void Complete(bool bSuccess, const FFlockError& Error);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	TFunction<void(FFlockAuthProvider&, TFunction<void(bool, const FFlockError&)>)> Start;
};

UCLASS()
class FLOCK_API UFlockNameAvailableAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The check completed; bAvailable is advisory (a race can still lose at register time). */
	UPROPERTY(BlueprintAssignable)
	FFlockNameAvailablePin OnResult;

	UPROPERTY(BlueprintAssignable)
	FFlockNameAvailablePin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Is Name Available"), Category = "Flock|Auth")
	static UFlockNameAvailableAction* IsNameAvailable(UObject* WorldContextObject, const FString& Name);

	virtual void Activate() override;

private:
	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString Name;
};
