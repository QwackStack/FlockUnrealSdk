// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Claims extracted from an access token. PlayerId/GameId tolerate several claim spellings; the
 * first matching key wins. Times are UTC. RawClaims keeps the full payload for diagnostics or
 * claims this SDK version doesn't type.
 */
struct FLOCK_API FFlockJwtClaims
{
	FString PlayerId;
	FString GameId;
	FString Email;
	FString Username;
	FString Role;
	FString Issuer;
	FString Audience;
	TOptional<FDateTime> ExpirationTime;
	TOptional<FDateTime> IssuedAt;
	TSharedPtr<FJsonObject> RawClaims;
};

/** Decodes a JWT's payload into claims. Parse-only — signature verification is the backend's job. */
class FLOCK_API FFlockJwt
{
public:
	/**
	 * Parses Token's payload into OutClaims. Returns false with OutError on: empty token, not
	 * exactly three dot-separated parts, invalid base64url, or a non-object JSON payload.
	 */
	static bool Parse(const FString& Token, FFlockJwtClaims& OutClaims, FString& OutError);

private:
	/** base64url -> UTF-8 string. Returns false on invalid input (including len % 4 == 1). */
	static bool Base64UrlDecode(const FString& In, FString& OutDecoded);

	/** First non-null claim under any of Keys, stringified (numbers/bools included). */
	static FString GetClaimString(const TSharedRef<FJsonObject>& Claims, std::initializer_list<const TCHAR*> Keys);

	/** Unix-seconds claim -> UTC datetime; unset when absent or non-numeric. */
	static TOptional<FDateTime> GetClaimTime(const TSharedRef<FJsonObject>& Claims, const TCHAR* Key);
};
