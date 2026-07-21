// Copyright 2022, Qwacks. All Rights Reserved.

#include "Auth/FlockJwt.h"
#include "Misc/Base64.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

bool FFlockJwt::Parse(const FString& Token, FFlockJwtClaims& OutClaims, FString& OutError)
{
	if (Token.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("Token cannot be null or empty");
		return false;
	}

	TArray<FString> Parts;
	Token.ParseIntoArray(Parts, TEXT("."), /*bCullEmpty*/ false);
	if (Parts.Num() != 3)
	{
		OutError = TEXT("Invalid JWT format: expected 3 dot-separated parts");
		return false;
	}

	FString PayloadJson;
	if (!Base64UrlDecode(Parts[1], PayloadJson))
	{
		OutError = TEXT("Invalid JWT payload encoding");
		return false;
	}

	TSharedPtr<FJsonObject> Payload;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PayloadJson);
	if (!FJsonSerializer::Deserialize(Reader, Payload) || !Payload.IsValid())
	{
		OutError = TEXT("JWT payload is not a JSON object");
		return false;
	}

	const TSharedRef<FJsonObject> Claims = Payload.ToSharedRef();
	OutClaims = FFlockJwtClaims();
	OutClaims.PlayerId = GetClaimString(Claims, { TEXT("sub"), TEXT("playerId"), TEXT("player_id"), TEXT("userId"), TEXT("user_id") });
	OutClaims.GameId = GetClaimString(Claims, { TEXT("gameId"), TEXT("game_id"), TEXT("gid") });
	OutClaims.Email = GetClaimString(Claims, { TEXT("email") });
	OutClaims.Username = GetClaimString(Claims, { TEXT("username"), TEXT("name") });
	OutClaims.Role = GetClaimString(Claims, { TEXT("role") });
	OutClaims.Issuer = GetClaimString(Claims, { TEXT("iss") });
	OutClaims.Audience = GetClaimString(Claims, { TEXT("aud") });
	OutClaims.ExpirationTime = GetClaimTime(Claims, TEXT("exp"));
	OutClaims.IssuedAt = GetClaimTime(Claims, TEXT("iat"));
	OutClaims.RawClaims = Payload;
	return true;
}

bool FFlockJwt::Base64UrlDecode(const FString& In, FString& OutDecoded)
{
	FString Standard = In;
	Standard.ReplaceInline(TEXT("-"), TEXT("+"));
	Standard.ReplaceInline(TEXT("_"), TEXT("/"));
	switch (Standard.Len() % 4)
	{
	case 0: break;
	case 2: Standard += TEXT("=="); break;
	case 3: Standard += TEXT("="); break;
	default: return false; // len % 4 == 1 is never valid base64
	}

	TArray<uint8> Bytes;
	if (!FBase64::Decode(Standard, Bytes))
	{
		return false;
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	OutDecoded = FString(Converted.Length(), Converted.Get());
	return true;
}

FString FFlockJwt::GetClaimString(const TSharedRef<FJsonObject>& Claims, std::initializer_list<const TCHAR*> Keys)
{
	for (const TCHAR* Key : Keys)
	{
		const TSharedPtr<FJsonValue> Value = Claims->TryGetField(Key);
		if (!Value.IsValid() || Value->IsNull())
		{
			continue;
		}
		switch (Value->Type)
		{
		case EJson::String: return Value->AsString();
		case EJson::Number:
		{
			const double Number = Value->AsNumber();
			// Integral claims (ids) must not pick up a ".0" suffix.
			return FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number))
				? FString::Printf(TEXT("%lld"), static_cast<int64>(Number))
				: FString::SanitizeFloat(Number);
		}
		case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
		default: break; // arrays/objects aren't meaningful as a scalar claim
		}
	}
	return FString();
}

TOptional<FDateTime> FFlockJwt::GetClaimTime(const TSharedRef<FJsonObject>& Claims, const TCHAR* Key)
{
	double Seconds = 0.0;
	if (Claims->TryGetNumberField(Key, Seconds))
	{
		return FDateTime::FromUnixTimestamp(static_cast<int64>(Seconds));
	}
	return TOptional<FDateTime>();
}
