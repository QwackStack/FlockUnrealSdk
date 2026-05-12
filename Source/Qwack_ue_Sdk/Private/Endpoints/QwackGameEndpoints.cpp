#include "QwackGameEndpoints.h"

// Auth
FSQwackFlockEndpoints UQwackFlockGameEndpoints::PlayerLogin        = Make(TEXT("/v1/player/login"),         EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::PlayerRegister     = Make(TEXT("/v1/player/register"),      EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::PlayerTokenRefresh = Make(TEXT("/v1/player/token/refresh"), EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::PlayerAuthTest     = Make(TEXT("/v1/player/auth-test"),     EQwackSDKHTTPType::GET);

// Analytics
FSQwackFlockEndpoints UQwackFlockGameEndpoints::StartSession       = Make(TEXT("/v1/analytics/sessions"),              EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::EndSession         = Make(TEXT("/v1/analytics/sessions/{session_id}"), EQwackSDKHTTPType::PATCH);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::TrackEvent         = Make(TEXT("/v1/analytics/events/single"),         EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::TrackEvents        = Make(TEXT("/v1/analytics/events"),                EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::RecordTransaction  = Make(TEXT("/v1/analytics/transactions"),          EQwackSDKHTTPType::POST);

// Log events
FSQwackFlockEndpoints UQwackFlockGameEndpoints::LogEvent           = Make(TEXT("/v1/log_event/single"),                EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::LogEvents          = Make(TEXT("/v1/log_event"),                       EQwackSDKHTTPType::POST);

FSQwackFlockEndpoints UQwackFlockGameEndpoints::Make(const FString& Path, EQwackSDKHTTPType T)
{
	FSQwackFlockEndpoints E;
	E.EndPoint = Path;
	E.RequestType = T;
	return E;
}

const TCHAR* UQwackFlockGameEndpoints::QwackHttpVerb(EQwackSDKHTTPType T)
{
	switch (T)
	{
	case EQwackSDKHTTPType::GET:    return TEXT("GET");
	case EQwackSDKHTTPType::POST:   return TEXT("POST");
	case EQwackSDKHTTPType::PUT:    return TEXT("PUT");
	case EQwackSDKHTTPType::PATCH:  return TEXT("PATCH");
	case EQwackSDKHTTPType::DELETE: return TEXT("DELETE");
	default:                        return TEXT("GET");
	}
}
