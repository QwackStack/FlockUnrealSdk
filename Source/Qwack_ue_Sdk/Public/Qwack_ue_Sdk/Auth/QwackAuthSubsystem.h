// Player auth state and /v1/player endpoints. Holds the active access/refresh tokens
// and the player id, and broadcasts OnAccessTokenChanged whenever the access token
// transitions. Feature subsystems read the token through GetAccessToken() lazily so
// rotation is automatic, and bind to OnAccessTokenChanged to drain spools on login.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Qwack_ue_Sdk/Utils/Schemas.h"
#include "QwackAuthSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockOnAuthChanged, const FString&, AccessToken);

UCLASS()
class QWACK_UE_SDK_API UQwackAuthSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

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

	DECLARE_DYNAMIC_DELEGATE_OneParam(FFlockOnAuthResponse,     const FFlockPlayerAuthResponse&, Response);
	DECLARE_DYNAMIC_DELEGATE_OneParam(FFlockOnAuthTestResponse, const FFlockAuthTestResponse&,   Response);

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth")
	void RegisterPlayer(const FFlockPlayerRegisterRequest& Request, const FFlockOnAuthResponse& Callback);

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth")
	void LoginPlayer(const FFlockPlayerLoginRequest& Request, const FFlockOnAuthResponse& Callback);

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth")
	void RefreshToken(const FFlockPlayerRefreshRequest& Request, const FFlockOnAuthResponse& Callback);

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth")
	void AuthTest(const FFlockOnAuthTestResponse& Callback);

private:
	UPROPERTY(Transient) FString AccessToken;
	UPROPERTY(Transient) FString RefreshTokenValue;
	UPROPERTY(Transient) FString PlayerId;

	void StoreAuthFromResponse(const FFlockPlayerAuth& Auth);
};
