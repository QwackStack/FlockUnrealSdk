// Copyright 2022, Qwack. All Rights Reserved.


#include "QwackSDKUtils.h"
#include "Schemas.h"
#include "../Config/QwackConfig.h"


#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include "openssl/pem.h" // Include for PEM decoding
#include "openssl/rsa.h" // Include for RSA signature verification
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END
#undef UI

FString UQwackSDKUtils::ClientID = "";
FString UQwackSDKUtils::ClientSecret = "";

bool UQwackSDKUtils::IsValidSteamTicket(const FString& Ticket) {
	// Define the regex pattern for a Steam ticket (exact length of 288 hex characters)
	FRegexPattern Pattern(TEXT("^[0-9A-F]{288}$"));
	FRegexMatcher Matcher(Pattern, Ticket);

	// Try to find a match
	return Matcher.FindNext();
}
FString UQwackSDKUtils::GetValueFromQwackJson(const FString& FullJsonResponse, const FString& Key)
{
	FString Value;

	// Parse the JSON string from the full response
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FullJsonResponse);
	if (FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		// Check if the JSON object is valid and contains the key
		if (JsonObject.IsValid() && JsonObject->HasField(Key))
		{
			// Try to get the value associated with the key
			JsonObject->TryGetStringField(Key, Value);
		}
	}

	return Value;
}

void UQwackSDKUtils::LogLongMessage(const FString& Message, int32 MaxLength = 1024)
{
	for (int32 i = 0; i < Message.Len(); i += MaxLength)
	{
		FString Chunk = Message.Mid(i, FMath::Min(MaxLength, Message.Len() - i));
	}
}

void UQwackSDKUtils::LoadSettings()
{
	// Specify the filename of the configuration file
	const FString Filename = TEXT("ServerAuth.ini");

	// Construct the full path to the configuration file within the project's Config directory
	FString ConfigFilePath = FPaths::Combine(FPaths::ProjectConfigDir(), Filename);

	// Normalize the path to ensure it's in a standard format
	FString NormalizedConfigFilePath = FConfigCacheIni::NormalizeConfigIniPath(ConfigFilePath);

	FConfigFile ConfigFile;


	if (!FPaths::FileExists(NormalizedConfigFilePath))
	{
		// Provide default values and return early
		ClientID = TEXT("DefaultClientID");
		ClientSecret = TEXT("DefaultClientSecret");
		return;
	}
	// Load the configuration file
	bool bSuccess = FConfigCacheIni::LoadExternalIniFile(
		ConfigFile, // ConfigFile reference
		*NormalizedConfigFilePath, // Full path to the ini file
		nullptr, // EngineConfigDir (optional, set to nullptr)
		nullptr, // SourceConfigDir (optional, set to nullptr)
		false, // bIsBaseIniName (optional, set to false)
		nullptr, // Platform (optional, set to nullptr)
		false, // bForceReload (optional, set to false)
		false, // bWriteDestIni (optional, set to false)
		false, // bAllowGeneratedIniWhenCooked (optional, set to false)
		nullptr // GeneratedConfigDir (optional, set to nullptr)
	);

	if (bSuccess)
	{
		// Read ClientID from the configuration file
		if (ConfigFile.GetString(TEXT("AuthSettings"), TEXT("ClientID"), ClientID))
		{
			//good to go
		}

		// Read ClientSecret from the configuration file
		if (ConfigFile.GetString(TEXT("AuthSettings"), TEXT("ClientSecret"), ClientSecret))
		{
			//GOOD TO GO
		}
	}
	else
	{
		// Configuration file does not exist or failed to load
		// Provide default values for ClientID and ClientSecret
		ClientID = TEXT("DefaultClientID");
		ClientSecret = TEXT("DefaultClientSecret");
	}
}

FString UQwackSDKUtils::GetClientID()
{
	LoadSettings();
	return ClientID;
}

FString UQwackSDKUtils::GetClientSecret()
{
	LoadSettings();
	return ClientSecret;
}


TSharedPtr<FJsonObject> UQwackSDKUtils::ParseJwtPayloadToJson(const FString& JwtToken)
{
	FString PayloadString = DecodeJwtPayload(JwtToken);
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PayloadString);

	if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
	{
		return JsonObject;
	}

	return nullptr;
}



const UQwackConfig* UQwackSDKUtils::GetQwackConfig()
{
	return GetDefault<UQwackConfig>();
}

bool UQwackSDKUtils::ParseFlockEnvolopeResponse(const FString& FullText, FFLockApiEnvelope& OutResult)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FullText);
	if(!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}
	auto ToString = [](const TSharedPtr<FJsonValue>& V) -> FString
	{
		if (!V.IsValid()) return TEXT("");
		FString S;
		const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&S);
		FJsonSerializer::Serialize(V.ToSharedRef(), TEXT(""), W);
		return S;
	};
	OutResult.error_json    = ToString(Root->TryGetField(TEXT("error")));
	OutResult.response_json = ToString(Root->TryGetField(TEXT("response")));
	OutResult.result_json   = ToString(Root->TryGetField(TEXT("result")));
	return true;
}

// token expiration

bool UQwackSDKUtils::IsTokenExpired(const FString& JwtToken)
{
	auto PayloadObject = ParseJwtPayloadToJson(JwtToken);
	if (PayloadObject.IsValid())
	{
		// The exp claim is usually in Unix timestamp format (seconds since epoch)
		int64 ExpirationTime = 0;
		if (PayloadObject->TryGetNumberField(TEXT("exp"), ExpirationTime))
		{
			// Get the current time in Unix timestamp format
			const FDateTime Now = FDateTime::UtcNow();
			const int64 NowTimestamp = Now.ToUnixTimestamp();
            
			// Check if current time is greater than expiration time
			return NowTimestamp > ExpirationTime;
		}
	}

	// If we cannot find the expiration claim, assume expired or invalid
	return true;
}
/*
 * for each vale in jsongarray
 *  get json field
 */
FString UQwackSDKUtils::DecodeJwtPayload(const FString& JwtToken)
{
	int32 PayloadStartIndex, PayloadEndIndex;
	JwtToken.FindChar('.', PayloadStartIndex);
	JwtToken.FindLastChar('.', PayloadEndIndex);
	FString JwtPayload = JwtToken.Mid(PayloadStartIndex + 1, PayloadEndIndex - PayloadStartIndex - 1);

	// Replace characters for standard Base64 encoding
	FString DecodedPayload = JwtPayload.Replace(TEXT("-"), TEXT("+")).Replace(TEXT("_"), TEXT("/"));

	// Decode the Base64 encoded payload to binary data
	TArray<uint8> PayloadBytes;
	FBase64::Decode(DecodedPayload, PayloadBytes);

	// Convert binary data to string
	FString PayloadString = FString(UTF8_TO_TCHAR(PayloadBytes.GetData()), PayloadBytes.Num());

	return PayloadString;
}


