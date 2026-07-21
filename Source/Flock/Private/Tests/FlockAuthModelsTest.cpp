// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Models/FlockAuthModels.h"
#include "Http/FlockJsonUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAuthModelsRequestWireTest, "Flock.Auth.Models.RequestWire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAuthModelsRequestWireTest::RunTest(const FString& Parameters)
{
	// Email login: only login_type/email/password on the wire; unused provider ids drop.
	{
		FFlockPlayerLoginRequest Req;
		Req.LoginType = TEXT("email");
		Req.Email = TEXT("a@b.c");
		Req.Password = TEXT("pw");
		FString Json;
		TestTrue(TEXT("serializes"), FFlockJsonUtils::StructToWireJson(Req, Json, /*bOmitEmptyStrings*/ true));
		TestTrue(TEXT("login_type"), Json.Contains(TEXT("\"login_type\":\"email\"")));
		TestTrue(TEXT("email"), Json.Contains(TEXT("\"email\":\"a@b.c\"")));
		TestTrue(TEXT("password"), Json.Contains(TEXT("\"password\":\"pw\"")));
		TestFalse(TEXT("no google_id"), Json.Contains(TEXT("google_id")));
		TestFalse(TEXT("no device_id"), Json.Contains(TEXT("device_id")));
		TestFalse(TEXT("no facebook_id"), Json.Contains(TEXT("facebook_id")));
	}
	// Device registration with optional name omitted when empty, present when set.
	{
		FFlockPlayerDeviceRegistrationRequest Req;
		Req.DeviceType = TEXT("Windows");
		Req.DeviceId = TEXT("dev-1");
		FString Json;
		TestTrue(TEXT("serializes"), FFlockJsonUtils::StructToWireJson(Req, Json, true));
		TestTrue(TEXT("device_type"), Json.Contains(TEXT("\"device_type\":\"Windows\"")));
		TestTrue(TEXT("device_id"), Json.Contains(TEXT("\"device_id\":\"dev-1\"")));
		TestFalse(TEXT("no empty name"), Json.Contains(TEXT("\"name\"")));

		Req.Name = TEXT("Duck");
		FString Json2;
		TestTrue(TEXT("serializes named"), FFlockJsonUtils::StructToWireJson(Req, Json2, true));
		TestTrue(TEXT("name present"), Json2.Contains(TEXT("\"name\":\"Duck\"")));
	}
	// Refresh request wire keys.
	{
		FFlockPlayerRefreshTokenRequest Req;
		Req.PlayerId = TEXT("p-1");
		Req.RefreshToken = TEXT("r-1");
		FString Json;
		TestTrue(TEXT("serializes"), FFlockJsonUtils::StructToWireJson(Req, Json, true));
		TestTrue(TEXT("player_id"), Json.Contains(TEXT("\"player_id\":\"p-1\"")));
		TestTrue(TEXT("refresh_token"), Json.Contains(TEXT("\"refresh_token\":\"r-1\"")));
	}
	// Social/provider-token requests.
	{
		FFlockPlayerGoogleLoginRequest Google; Google.IdToken = TEXT("g");
		FFlockPlayerAppleLoginRequest Apple; Apple.IdentityToken = TEXT("a");
		FFlockPlayerSteamLoginRequest Steam; Steam.SessionTicket = TEXT("s");
		FString GJson, AJson, SJson;
		FFlockJsonUtils::StructToWireJson(Google, GJson, true);
		FFlockJsonUtils::StructToWireJson(Apple, AJson, true);
		FFlockJsonUtils::StructToWireJson(Steam, SJson, true);
		TestTrue(TEXT("id_token"), GJson.Contains(TEXT("\"id_token\":\"g\"")));
		TestTrue(TEXT("identity_token"), AJson.Contains(TEXT("\"identity_token\":\"a\"")));
		TestTrue(TEXT("session_ticket"), SJson.Contains(TEXT("\"session_ticket\":\"s\"")));
	}
	// Password/email flows.
	{
		FFlockPlayerPasswordResetRequest Req;
		Req.Email = TEXT("a@b.c"); Req.Code = TEXT("123"); Req.NewPassword = TEXT("np");
		FString Json;
		FFlockJsonUtils::StructToWireJson(Req, Json, true);
		TestTrue(TEXT("code"), Json.Contains(TEXT("\"code\":\"123\"")));
		TestTrue(TEXT("new_password"), Json.Contains(TEXT("\"new_password\":\"np\"")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAuthModelsResponseWireTest, "Flock.Auth.Models.ResponseWire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAuthModelsResponseWireTest::RunTest(const FString& Parameters)
{
	{
		FFlockPlayerLoginResponse Out;
		FString Err;
		TestTrue(TEXT("login parses"), FFlockJsonUtils::WireJsonToStruct(
			TEXT("{\"player_id\":\"p-1\",\"access_token\":\"at\",\"refresh_token\":\"rt\"}"), Out, Err));
		TestEqual(TEXT("player id"), Out.PlayerId, FString(TEXT("p-1")));
		TestEqual(TEXT("access"), Out.AccessToken, FString(TEXT("at")));
		TestEqual(TEXT("refresh"), Out.RefreshToken, FString(TEXT("rt")));
	}
	{
		FFlockAuthActionResponse Out;
		FString Err;
		TestTrue(TEXT("action parses"), FFlockJsonUtils::WireJsonToStruct(TEXT("{\"success\":true}"), Out, Err));
		TestTrue(TEXT("success"), Out.Success);
	}
	{
		FFlockTokenRevokeResponse Out;
		FString Err;
		TestTrue(TEXT("revoke parses"), FFlockJsonUtils::WireJsonToStruct(TEXT("{\"revoked\":true}"), Out, Err));
		TestTrue(TEXT("revoked"), Out.Revoked);
	}
	{
		FFlockNameAvailableResponse Out;
		FString Err;
		TestTrue(TEXT("name parses"), FFlockJsonUtils::WireJsonToStruct(
			TEXT("{\"name\":\"Duck\",\"available\":false}"), Out, Err));
		TestEqual(TEXT("name"), Out.Name, FString(TEXT("Duck")));
		TestFalse(TEXT("available"), Out.Available);
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
