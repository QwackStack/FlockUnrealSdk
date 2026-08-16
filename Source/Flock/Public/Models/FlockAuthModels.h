// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockEventModels.h"
#include "FlockAuthModels.generated.h"

/**
 * Wire models for the player auth endpoints (OpenAPI /v1/player/*). Field names map to the
 * snake_case wire via FFlockJsonUtils (LoginType -> login_type). Request structs are serialized
 * with empty-string omission so optional members drop off the wire entirely; response bools follow
 * the wire-model convention (no b prefix).
 */

// ── Requests ──

/** Generic login body (/player/login): LoginType selects which identity fields the backend reads. */
USTRUCT()
struct FLOCK_API FFlockPlayerLoginRequest
{
	GENERATED_BODY()

	UPROPERTY() FString LoginType;
	UPROPERTY() FString Email;
	UPROPERTY() FString Password;
	UPROPERTY() FString GoogleId;
	UPROPERTY() FString DeviceId;
	UPROPERTY() FString DeviceType;
	UPROPERTY() FString AppleId;
	UPROPERTY() FString FacebookId;
	UPROPERTY() FString SteamId;
	UPROPERTY() FString DiscordId;
};

USTRUCT()
struct FLOCK_API FFlockPlayerEmailRegistrationRequest
{
	GENERATED_BODY()

	UPROPERTY() FString Email;
	UPROPERTY() FString Password;
	/** Optional display name — server-enforced unique; omitted from the wire when empty. */
	UPROPERTY() FString Name;
};

USTRUCT()
struct FLOCK_API FFlockPlayerDeviceLoginRequest
{
	GENERATED_BODY()

	UPROPERTY() FString DeviceType;
	UPROPERTY() FString DeviceId;
};

USTRUCT()
struct FLOCK_API FFlockPlayerDeviceRegistrationRequest
{
	GENERATED_BODY()

	UPROPERTY() FString DeviceType;
	UPROPERTY() FString DeviceId;
	UPROPERTY() FString Name;
};

USTRUCT()
struct FLOCK_API FFlockPlayerGoogleLoginRequest
{
	GENERATED_BODY()

	UPROPERTY() FString IdToken;
};

USTRUCT()
struct FLOCK_API FFlockPlayerGoogleRegistrationRequest
{
	GENERATED_BODY()

	UPROPERTY() FString IdToken;
	UPROPERTY() FString Name;
};

USTRUCT()
struct FLOCK_API FFlockPlayerAppleLoginRequest
{
	GENERATED_BODY()

	UPROPERTY() FString IdentityToken;
};

USTRUCT()
struct FLOCK_API FFlockPlayerAppleRegistrationRequest
{
	GENERATED_BODY()

	UPROPERTY() FString IdentityToken;
	UPROPERTY() FString Name;
};

USTRUCT()
struct FLOCK_API FFlockPlayerSteamLoginRequest
{
	GENERATED_BODY()

	UPROPERTY() FString SessionTicket;
};

USTRUCT()
struct FLOCK_API FFlockPlayerSteamRegistrationRequest
{
	GENERATED_BODY()

	UPROPERTY() FString SessionTicket;
	UPROPERTY() FString Name;
};

USTRUCT()
struct FLOCK_API FFlockPlayerRefreshTokenRequest
{
	GENERATED_BODY()

	UPROPERTY() FString PlayerId;
	UPROPERTY() FString RefreshToken;
};

USTRUCT()
struct FLOCK_API FFlockPlayerPasswordForgotRequest
{
	GENERATED_BODY()

	UPROPERTY() FString Email;
};

USTRUCT()
struct FLOCK_API FFlockPlayerPasswordResetRequest
{
	GENERATED_BODY()

	UPROPERTY() FString Email;
	UPROPERTY() FString Code;
	UPROPERTY() FString NewPassword;
};

USTRUCT()
struct FLOCK_API FFlockPlayerEmailVerifyRequest
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
};

USTRUCT()
struct FLOCK_API FFlockPlayerLinkEmailRequest
{
	GENERATED_BODY()

	UPROPERTY() FString Email;
	UPROPERTY() FString Password;
};

USTRUCT()
struct FLOCK_API FFlockPlayerLinkDeviceRequest
{
	GENERATED_BODY()

	UPROPERTY() FString DeviceType;
	UPROPERTY() FString DeviceId;
};

// Every OAuth provider links with a bare token — the login routes' per-provider field names don't apply here.
USTRUCT()
struct FLOCK_API FFlockPlayerLinkOAuthRequest
{
	GENERATED_BODY()

	UPROPERTY() FString Token;
};

// ── Responses (BlueprintType — surfaced by the auth async nodes) ──

USTRUCT(BlueprintType)
struct FLOCK_API FFlockPlayerLoginResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString AccessToken;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString RefreshToken;
};

USTRUCT(BlueprintType)
struct FLOCK_API FFlockAuthActionResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool Success = false;
};

USTRUCT(BlueprintType)
struct FLOCK_API FFlockTokenRevokeResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool Revoked = false;
};

USTRUCT(BlueprintType)
struct FLOCK_API FFlockNameAvailableResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool Available = false;
};

/** One credential attached to a player. No secrets — the token/password never comes back. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockPlayerLinkedAccount
{
	GENERATED_BODY()

	/** Raw wire spelling of the provider (a LoginType value). */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Provider;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString ProviderUserId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Email;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool EmailVerified = false;

	/**
	 * Typed view of Provider — pass straight to Unlink. Unknown for a provider this SDK version
	 * predates. Not on the wire: the provider fills it in after deserializing each account.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	EFlockCredentialProvider ProviderType = EFlockCredentialProvider::Unknown;
};

USTRUCT(BlueprintType)
struct FLOCK_API FFlockPlayerAccountsResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockPlayerLinkedAccount> Accounts;
};

/** Registration outcome: success either created an account (bAlreadyRegistered false, Response set) or found the identity already registered (bAlreadyRegistered true, Response empty). Not a wire model. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockRegisterResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockPlayerLoginResponse Response;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool bAlreadyRegistered = false;
};
