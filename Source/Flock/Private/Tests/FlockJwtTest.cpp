// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockJwt.h"
#include "Misc/Base64.h"

namespace
{
	FString Base64Url(const FString& In)
	{
		FString Encoded = FBase64::Encode(In);
		Encoded.ReplaceInline(TEXT("+"), TEXT("-"));
		Encoded.ReplaceInline(TEXT("/"), TEXT("_"));
		Encoded.ReplaceInline(TEXT("="), TEXT(""));
		return Encoded;
	}

	FString MakeJwt(const FString& PayloadJson)
	{
		return FString::Printf(TEXT("%s.%s.sig"), *Base64Url(TEXT("{\"alg\":\"none\"}")), *Base64Url(PayloadJson));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJwtParseTest, "Flock.Auth.Jwt.Parse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockJwtParseTest::RunTest(const FString& Parameters)
{
	const int64 Exp = (FDateTime::UtcNow() + FTimespan::FromHours(1)).ToUnixTimestamp();
	const int64 Iat = FDateTime::UtcNow().ToUnixTimestamp();
	const FString Token = MakeJwt(FString::Printf(
		TEXT("{\"sub\":\"p-1\",\"game_id\":\"g-1\",\"email\":\"a@b.c\",\"username\":\"Duck\",\"role\":\"player\",\"exp\":%lld,\"iat\":%lld,\"iss\":\"flock\",\"aud\":\"game\"}"),
		Exp, Iat));

	FFlockJwtClaims Claims;
	FString Error;
	TestTrue(TEXT("parses"), FFlockJwt::Parse(Token, Claims, Error));
	TestEqual(TEXT("player id from sub"), Claims.PlayerId, FString(TEXT("p-1")));
	TestEqual(TEXT("game id"), Claims.GameId, FString(TEXT("g-1")));
	TestEqual(TEXT("email"), Claims.Email, FString(TEXT("a@b.c")));
	TestEqual(TEXT("username"), Claims.Username, FString(TEXT("Duck")));
	TestEqual(TEXT("role"), Claims.Role, FString(TEXT("player")));
	TestEqual(TEXT("issuer"), Claims.Issuer, FString(TEXT("flock")));
	TestEqual(TEXT("audience"), Claims.Audience, FString(TEXT("game")));
	TestTrue(TEXT("exp set"), Claims.ExpirationTime.IsSet());
	TestEqual(TEXT("exp value"), Claims.ExpirationTime.GetValue().ToUnixTimestamp(), Exp);
	TestTrue(TEXT("iat set"), Claims.IssuedAt.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJwtClaimFallbacksTest, "Flock.Auth.Jwt.ClaimFallbacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockJwtClaimFallbacksTest::RunTest(const FString& Parameters)
{
	FFlockJwtClaims Claims;
	FString Error;

	// player_id fallback order: sub > playerId > player_id > userId > user_id.
	TestTrue(TEXT("playerId"), FFlockJwt::Parse(MakeJwt(TEXT("{\"playerId\":\"p-2\"}")), Claims, Error));
	TestEqual(TEXT("playerId used"), Claims.PlayerId, FString(TEXT("p-2")));

	TestTrue(TEXT("user_id"), FFlockJwt::Parse(MakeJwt(TEXT("{\"user_id\":\"p-3\"}")), Claims, Error));
	TestEqual(TEXT("user_id used"), Claims.PlayerId, FString(TEXT("p-3")));

	TestTrue(TEXT("sub wins"), FFlockJwt::Parse(MakeJwt(TEXT("{\"user_id\":\"p-4\",\"sub\":\"p-5\"}")), Claims, Error));
	TestEqual(TEXT("sub preferred"), Claims.PlayerId, FString(TEXT("p-5")));

	// game id: gameId | game_id | gid; username: username | name.
	TestTrue(TEXT("gid"), FFlockJwt::Parse(MakeJwt(TEXT("{\"gid\":\"g-2\",\"name\":\"N\"}")), Claims, Error));
	TestEqual(TEXT("gid used"), Claims.GameId, FString(TEXT("g-2")));
	TestEqual(TEXT("name used"), Claims.Username, FString(TEXT("N")));

	// Numeric claim values stringify.
	TestTrue(TEXT("numeric sub"), FFlockJwt::Parse(MakeJwt(TEXT("{\"sub\":12345}")), Claims, Error));
	TestEqual(TEXT("numeric stringified"), Claims.PlayerId, FString(TEXT("12345")));

	// Missing exp -> unset optional, not an error.
	TestTrue(TEXT("no exp ok"), FFlockJwt::Parse(MakeJwt(TEXT("{\"sub\":\"p\"}")), Claims, Error));
	TestFalse(TEXT("exp unset"), Claims.ExpirationTime.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJwtRejectsTest, "Flock.Auth.Jwt.Rejects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockJwtRejectsTest::RunTest(const FString& Parameters)
{
	FFlockJwtClaims Claims;
	FString Error;

	TestFalse(TEXT("empty"), FFlockJwt::Parse(TEXT(""), Claims, Error));
	TestFalse(TEXT("whitespace"), FFlockJwt::Parse(TEXT("   "), Claims, Error));
	TestFalse(TEXT("two parts"), FFlockJwt::Parse(TEXT("a.b"), Claims, Error));
	TestFalse(TEXT("four parts"), FFlockJwt::Parse(TEXT("a.b.c.d"), Claims, Error));
	TestFalse(TEXT("bad base64url length"), FFlockJwt::Parse(TEXT("h.abcde.s"), Claims, Error)); // len%4==1
	TestFalse(TEXT("payload not json"), FFlockJwt::Parse(
		FString::Printf(TEXT("h.%s.s"), *Base64Url(TEXT("not json"))), Claims, Error));
	TestFalse(TEXT("error populated"), Error.IsEmpty());
	return true;
}

#endif // WITH_AUTOMATION_TESTS
