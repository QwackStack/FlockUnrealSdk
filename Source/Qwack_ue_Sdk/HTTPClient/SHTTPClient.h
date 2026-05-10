// USHTTPClient.h

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "HTTPResponse.h"


#include "SHTTPClient.generated.h"
struct FQwackHTTPResponse;


/**
 * 
 */
UCLASS()
class QWACK_UE_SDK_API USHTTPClient : public UObject
{
	GENERATED_BODY()

public:
	USHTTPClient();

	void SendRequest(const FString& endPoint, const FString& requestType, const FString& data, FQwackFlockResponse OnCompleteRequest, TMap
	                 <FString, FString> customHeaders) const;
	void GetToken(const FString& TokenEndpoint, bool bPlayer, const FString& SteamToken, const FQwackHTTPResponse& OnCompleteRequest) const;
private:
	static const FString UserAgent;
	static const FString UserInstanceIdentifier;

	
};
