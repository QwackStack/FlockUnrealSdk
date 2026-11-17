// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockResult.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Models/FlockAuthModels.h"
#include "FlockAuthAsyncActions.generated.h"

class FFlockAuthProvider;
class UFlockLocalPlayer;

/**
 * Blueprint async nodes for the auth feature. Each node resolves the SDK from its world context on
 * Activate; when the SDK is unavailable or uninitialized the failure pin fires with a Validation
 * error. Exactly one pin fires per activation. Success/failure pins share one delegate signature
 * per node so wildcard pins line up; the unused half of the payload is default-constructed.
 *
 * Sync state (Is Authenticated, Get Player Id, Logout) lives on UFlockSubsystem directly.
 *
 * The Local Player pin, left empty, acts for the main player; set, it acts for that local player
 * (UFlockSubsystem::AddLocalPlayer), whose Is Authenticated and Get Player Id are on the local player itself.
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockLoginPin, const FFlockPlayerLoginResponse&, Response, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockRegisterPin, const FFlockRegisterResult&, Result, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFlockRestorePin);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockAuthAccountPin, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockNameAvailablePin, bool, bAvailable, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockAccountsPin, const TArray<FFlockPlayerLinkedAccount>&, Accounts, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockRemoveLocalPlayerPin, const FFlockTokenRevokeResponse&, Response, const FFlockError&, Error);

UCLASS()
class FLOCK_API UFlockLoginAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockLoginPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockLoginPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Login With Email", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockLoginAction* LoginWithEmail(UObject* WorldContextObject, const FString& Email, const FString& Password, UFlockLocalPlayer* LocalPlayer = nullptr);

	/** Signs in the player registered with Device Id. Leave Device Id empty to use this install's (Flock Get This Device Id); any other id is sent as it is. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Login With Device", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockLoginAction* LoginWithDevice(UObject* WorldContextObject, const FString& DeviceId, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Login With Google", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockLoginAction* LoginWithGoogle(UObject* WorldContextObject, const FString& IdToken, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Login With Apple", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockLoginAction* LoginWithApple(UObject* WorldContextObject, const FString& IdentityToken, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Login With Steam", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockLoginAction* LoginWithSteam(UObject* WorldContextObject, const FString& SessionTicket, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Login With Facebook", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockLoginAction* LoginWithFacebook(UObject* WorldContextObject, const FString& FacebookId, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Login With Discord", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockLoginAction* LoginWithDiscord(UObject* WorldContextObject, const FString& DiscordId, UFlockLocalPlayer* LocalPlayer = nullptr);

	virtual void Activate() override;

private:
	static UFlockLoginAction* Make(UObject* InWorldContextObject, UFlockLocalPlayer* InLocalPlayer,
		TFunction<FFlockRequestHandle(FFlockAuthProvider&, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)>)> InStart);
	void Complete(const TFlockResult<FFlockPlayerLoginResponse>& Result);

	/** Weak: the game instance keeps this node, which must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;

	UPROPERTY()
	TObjectPtr<UFlockLocalPlayer> LocalPlayer;

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

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Register With Email", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockRegisterAction* RegisterWithEmail(UObject* WorldContextObject, const FString& Email, const FString& Password, const FString& Name, UFlockLocalPlayer* LocalPlayer = nullptr);

	/** Registers a new player with Device Id and signs them in. Leave Device Id empty to use this install's (Flock Get This Device Id); any other id is sent as it is. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Register With Device", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockRegisterAction* RegisterWithDevice(UObject* WorldContextObject, const FString& DeviceId, const FString& Name, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Register With Google", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockRegisterAction* RegisterWithGoogle(UObject* WorldContextObject, const FString& IdToken, const FString& Name, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Register With Apple", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockRegisterAction* RegisterWithApple(UObject* WorldContextObject, const FString& IdentityToken, const FString& Name, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Register With Steam", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockRegisterAction* RegisterWithSteam(UObject* WorldContextObject, const FString& SessionTicket, const FString& Name, UFlockLocalPlayer* LocalPlayer = nullptr);

	virtual void Activate() override;

private:
	static UFlockRegisterAction* Make(UObject* InWorldContextObject, UFlockLocalPlayer* InLocalPlayer,
		TFunction<FFlockRequestHandle(FFlockAuthProvider&, TFunction<void(TFlockResult<FFlockRegisterResult>)>)> InStart);
	void Complete(const TFlockResult<FFlockRegisterResult>& Result);

	/** Weak: the game instance keeps this node, which must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;

	UPROPERTY()
	TObjectPtr<UFlockLocalPlayer> LocalPlayer;

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

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Restore Session", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockRestoreSessionAction* RestoreSession(UObject* WorldContextObject, UFlockLocalPlayer* LocalPlayer = nullptr);

	virtual void Activate() override;

private:
	/** Weak: the game instance keeps this node, which must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;

	UPROPERTY()
	TObjectPtr<UFlockLocalPlayer> LocalPlayer;
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

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Forgot Password", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockAuthAccountAction* ForgotPassword(UObject* WorldContextObject, const FString& Email, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Reset Password", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockAuthAccountAction* ResetPassword(UObject* WorldContextObject, const FString& Email, const FString& Code, const FString& NewPassword, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Send Email Verification", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockAuthAccountAction* SendEmailVerification(UObject* WorldContextObject, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Verify Email", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockAuthAccountAction* VerifyEmail(UObject* WorldContextObject, const FString& Code, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Revoke Token", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockAuthAccountAction* RevokeToken(UObject* WorldContextObject, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Refresh Token", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockAuthAccountAction* RefreshToken(UObject* WorldContextObject, UFlockLocalPlayer* LocalPlayer = nullptr);

	virtual void Activate() override;

private:
	static UFlockAuthAccountAction* Make(UObject* InWorldContextObject, UFlockLocalPlayer* InLocalPlayer,
		TFunction<void(FFlockAuthProvider&, TFunction<void(bool, const FFlockError&)>)> InStart);
	void Complete(bool bSuccess, const FFlockError& Error);

	/** Weak: the game instance keeps this node, which must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;

	UPROPERTY()
	TObjectPtr<UFlockLocalPlayer> LocalPlayer;

	TFunction<void(FFlockAuthProvider&, TFunction<void(bool, const FFlockError&)>)> Start;
};

/**
 * Account linking. Every node answers with the player's full updated credential list, so a link or
 * unlink doubles as a refresh of it. Unlink takes the typed provider, which a listed account carries
 * back on its Provider Type pin.
 */
UCLASS()
class FLOCK_API UFlockAccountLinkAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockAccountsPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockAccountsPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Get Linked Accounts", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockAccountLinkAction* GetLinkedAccounts(UObject* WorldContextObject, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Link Email", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockAccountLinkAction* LinkEmail(UObject* WorldContextObject, const FString& Email, const FString& Password, UFlockLocalPlayer* LocalPlayer = nullptr);

	/** Attaches Device Id to the signed-in player. Leave Device Id empty to use this install's (Flock Get This Device Id); any other id is sent as it is. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Link Device", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockAccountLinkAction* LinkDevice(UObject* WorldContextObject, const FString& DeviceId, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Link Google", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockAccountLinkAction* LinkGoogle(UObject* WorldContextObject, const FString& IdToken, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Link Apple", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockAccountLinkAction* LinkApple(UObject* WorldContextObject, const FString& IdentityToken, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Link Steam", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockAccountLinkAction* LinkSteam(UObject* WorldContextObject, const FString& SessionTicket, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Link Facebook", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockAccountLinkAction* LinkFacebook(UObject* WorldContextObject, const FString& FacebookToken, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Link Discord", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockAccountLinkAction* LinkDiscord(UObject* WorldContextObject, const FString& DiscordToken, UFlockLocalPlayer* LocalPlayer = nullptr);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Unlink Account", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockAccountLinkAction* Unlink(UObject* WorldContextObject, EFlockCredentialProvider Provider, UFlockLocalPlayer* LocalPlayer = nullptr);

	virtual void Activate() override;

private:
	static UFlockAccountLinkAction* Make(UObject* InWorldContextObject, UFlockLocalPlayer* InLocalPlayer,
		TFunction<FFlockRequestHandle(FFlockAuthProvider&, TFunction<void(TFlockResult<FFlockPlayerAccountsResponse>)>)> InStart);
	void Complete(const TFlockResult<FFlockPlayerAccountsResponse>& Result);

	/** Weak: the game instance keeps this node, which must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;

	UPROPERTY()
	TObjectPtr<UFlockLocalPlayer> LocalPlayer;

	TFunction<FFlockRequestHandle(FFlockAuthProvider&, TFunction<void(TFlockResult<FFlockPlayerAccountsResponse>)>)> Start;
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

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Is Name Available", AdvancedDisplay = "LocalPlayer"), Category = "Flock|Auth")
	static UFlockNameAvailableAction* IsNameAvailable(UObject* WorldContextObject, const FString& Name, UFlockLocalPlayer* LocalPlayer = nullptr);

	virtual void Activate() override;

private:
	/** Weak: the game instance keeps this node, which must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;

	UPROPERTY()
	TObjectPtr<UFlockLocalPlayer> LocalPlayer;

	FString Name;
};

/**
 * Signs a local player out on the server and here, and ends them (UFlockSubsystem::RemoveLocalPlayer). Success carries
 * the sign-out's answer; a player who was not signed in succeeds at once.
 */
UCLASS()
class FLOCK_API UFlockRemoveLocalPlayerAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockRemoveLocalPlayerPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockRemoveLocalPlayerPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Flock Remove Local Player"), Category = "Flock|Local Players")
	static UFlockRemoveLocalPlayerAction* RemoveLocalPlayer(UObject* WorldContextObject, UFlockLocalPlayer* LocalPlayer);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockTokenRevokeResponse>& Result);

	/** Weak: the game instance keeps this node, which must not keep a level's object (and so its world) alive past a level change. */
	TWeakObjectPtr<UObject> WorldContextObject;

	UPROPERTY()
	TObjectPtr<UFlockLocalPlayer> LocalPlayer;
};
