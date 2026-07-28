// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Auth/FlockFileTokenStore.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AES.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	constexpr int32 AesBlockSize = 16;

	FAES::FAESKey DeriveKey(const FString& KeyContext)
	{
		// LoginId binds the key to this machine/user; the context + fixed salt keep it distinct
		// per game and per format revision. SHA-1 twice fills the 32-byte AES key without pulling
		// in a crypto plugin — this is key stretching for obfuscation, not a security boundary.
		const FString Seed = FPlatformMisc::GetLoginId() + KeyContext + TEXT("FlockTokenStore.v1");
		const FTCHARToUTF8 Utf8(*Seed);

		uint8 First[20];
		uint8 Second[20];
		FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), First);
		FSHA1::HashBuffer(First, sizeof(First), Second);

		FAES::FAESKey Key;
		FMemory::Memcpy(Key.Key, First, 20);
		FMemory::Memcpy(Key.Key + 20, Second, 12);
		return Key;
	}
}

FFlockFileTokenStore::FFlockFileTokenStore(const FString& InFilePath, const FString& InKeyContext)
	: FilePath(InFilePath.IsEmpty() ? DefaultPath() : InFilePath)
	, KeyContext(InKeyContext)
{
}

FString FFlockFileTokenStore::DefaultPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Flock"), TEXT("auth.dat"));
}

void FFlockFileTokenStore::Save(const FFlockStoredTokens& Tokens)
{
	if (Tokens.AccessToken.IsEmpty())
	{
		Clear();
		return;
	}

	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("access_token"), Tokens.AccessToken);
	Obj->SetStringField(TEXT("refresh_token"), Tokens.RefreshToken);
	if (Tokens.AuthMethod.IsSet())
	{
		Obj->SetStringField(TEXT("auth_method"),
			StaticEnum<EFlockAuthMethod>()->GetNameStringByValue(static_cast<int64>(Tokens.AuthMethod.GetValue())));
	}

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Obj, Writer))
	{
		return;
	}

	const FTCHARToUTF8 Utf8(*Json);
	TArray<uint8> Bytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());

	// PKCS#7 pad to the AES block size (a full extra block when already aligned).
	const int32 Pad = AesBlockSize - (Bytes.Num() % AesBlockSize);
	for (int32 Index = 0; Index < Pad; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Pad));
	}

	FAES::EncryptData(Bytes.GetData(), Bytes.Num(), DeriveKey(KeyContext));
	FFileHelper::SaveArrayToFile(Bytes, *FilePath);
}

bool FFlockFileTokenStore::Load(FFlockStoredTokens& OutTokens)
{
	TArray<uint8> Bytes;
	// FILEREAD_Silent: a missing token file on first launch (or after logout) is normal, not a warning.
	if (!FFileHelper::LoadFileToArray(Bytes, *FilePath, FILEREAD_Silent))
	{
		return false;
	}

	// Anything that fails from here on is a corrupt or foreign-keyed file — remove it so the next
	// launch doesn't retry a dead payload.
	auto Corrupt = [this]()
	{
		IFileManager::Get().Delete(*FilePath);
		return false;
	};

	if (Bytes.Num() == 0 || Bytes.Num() % AesBlockSize != 0)
	{
		return Corrupt();
	}

	FAES::DecryptData(Bytes.GetData(), Bytes.Num(), DeriveKey(KeyContext));

	const uint8 Pad = Bytes.Last();
	if (Pad == 0 || Pad > AesBlockSize || Pad > Bytes.Num())
	{
		return Corrupt();
	}
	for (int32 Index = Bytes.Num() - Pad; Index < Bytes.Num(); ++Index)
	{
		if (Bytes[Index] != Pad)
		{
			return Corrupt();
		}
	}
	Bytes.SetNum(Bytes.Num() - Pad);

	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	const FString Json(Converted.Length(), Converted.Get());

	TSharedPtr<FJsonObject> Obj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		return Corrupt();
	}

	FFlockStoredTokens Loaded;
	Obj->TryGetStringField(TEXT("access_token"), Loaded.AccessToken);
	Obj->TryGetStringField(TEXT("refresh_token"), Loaded.RefreshToken);
	FString MethodName;
	if (Obj->TryGetStringField(TEXT("auth_method"), MethodName))
	{
		const int64 Value = StaticEnum<EFlockAuthMethod>()->GetValueByNameString(MethodName);
		if (Value != INDEX_NONE)
		{
			Loaded.AuthMethod = static_cast<EFlockAuthMethod>(Value);
		}
	}

	if (Loaded.AccessToken.IsEmpty())
	{
		return Corrupt();
	}

	OutTokens = MoveTemp(Loaded);
	return true;
}

void FFlockFileTokenStore::Clear()
{
	IFileManager::Get().Delete(*FilePath);
}
