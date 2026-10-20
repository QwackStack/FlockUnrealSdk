// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Config/FlockGameKey.h"

#include "FlockInitConfig.h"
#include "Misc/SecureHash.h"

FString FFlockGameKey::Of(const FString& ApiUrl, const FString& ApiKey)
{
	const FString Text = FFlockInitConfig::TrimApiUrl(ApiUrl).ToLower() + TEXT("\n") + ApiKey.TrimStartAndEnd();
	const FTCHARToUTF8 Utf8(*Text);
	uint8 Hash[FSHA1::DigestSize];
	FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash);
	return BytesToHexLower(Hash, 8);
}

FString FFlockGameKey::Named(const FString& Name, const FString& GameKey)
{
	return GameKey.IsEmpty() ? Name : Name + TEXT("-") + GameKey;
}
