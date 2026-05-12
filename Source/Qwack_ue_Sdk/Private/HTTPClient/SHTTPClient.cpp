#include "SHTTPClient.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "HTTPResponse.h"

const FString USHTTPClient::UserAgent = FString::Format(TEXT("X-UnrealEngine-Agent/{0}"), { ENGINE_VERSION_STRING });
const FString USHTTPClient::UserInstanceIdentifier = FGuid::NewGuid().ToString();

USHTTPClient::USHTTPClient()
{
}

void USHTTPClient::SendRequest(const FString& endPoint, const FString& requestType, const FString& data,
	FQwackFlockResponse OnCompleteRequest, TMap<FString, FString> customHeaders) const
{
	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(endPoint);
	HttpRequest->SetVerb(requestType);
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpRequest->SetHeader(TEXT("User-Agent"), UserAgent);

	if (!data.IsEmpty())
	{
		HttpRequest->SetContentAsString(data);
		HttpRequest->SetHeader(TEXT("User-Instance-Identifier"), UserInstanceIdentifier);
	}

	for (const auto& Header : customHeaders)
	{
		HttpRequest->SetHeader(Header.Key, Header.Value);
	}

	HttpRequest->OnProcessRequestComplete().BindLambda(
		[OnCompleteRequest](FHttpRequestPtr Req, const FHttpResponsePtr& Response, bool bWasSuccessful)
		{
			FQwackHTTPResponse FlockResponse;
			FlockResponse.StatusCode = Response.IsValid() ? Response->GetResponseCode() : 0;
			FlockResponse.FullText   = Response.IsValid() ? Response->GetContentAsString() : TEXT("Failed to process request.");
			FlockResponse.success    = bWasSuccessful && Response.IsValid() && EHttpResponseCodes::IsOk(FlockResponse.StatusCode);
			OnCompleteRequest.ExecuteIfBound(FlockResponse);
		});

	HttpRequest->ProcessRequest();
}
