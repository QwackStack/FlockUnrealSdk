// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockEventModels.h"

FString FFlockCredentialProviders::ToWire(EFlockCredentialProvider Provider)
{
	switch (Provider)
	{
	case EFlockCredentialProvider::DeviceId: return TEXT("device_id");
	case EFlockCredentialProvider::Email: return TEXT("email");
	case EFlockCredentialProvider::Google: return TEXT("google");
	case EFlockCredentialProvider::Apple: return TEXT("apple");
	case EFlockCredentialProvider::Facebook: return TEXT("facebook");
	case EFlockCredentialProvider::Steam: return TEXT("steam");
	case EFlockCredentialProvider::Discord: return TEXT("discord");
	// Unknown has no wire spelling — callers turn the empty string into a Validation error.
	default: return FString();
	}
}

EFlockCredentialProvider FFlockCredentialProviders::Parse(const FString& Wire)
{
	if (Wire.IsEmpty())
	{
		return EFlockCredentialProvider::Unknown;
	}

	const FString Lower = Wire.ToLower();
	if (Lower == TEXT("device_id")) { return EFlockCredentialProvider::DeviceId; }
	if (Lower == TEXT("email")) { return EFlockCredentialProvider::Email; }
	if (Lower == TEXT("google")) { return EFlockCredentialProvider::Google; }
	if (Lower == TEXT("apple")) { return EFlockCredentialProvider::Apple; }
	if (Lower == TEXT("facebook")) { return EFlockCredentialProvider::Facebook; }
	if (Lower == TEXT("steam")) { return EFlockCredentialProvider::Steam; }
	if (Lower == TEXT("discord")) { return EFlockCredentialProvider::Discord; }
	return EFlockCredentialProvider::Unknown;
}
