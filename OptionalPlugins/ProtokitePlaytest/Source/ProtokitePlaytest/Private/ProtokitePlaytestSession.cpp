// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "ProtokitePlaytestSession.h"

#include "Dom/JsonObject.h"
#include "FlockSubsystem.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

bool FProtokitePlaytestSessionStartResult::FromWireObject(const TSharedRef<FJsonObject>& Object,
	FProtokitePlaytestSessionStartResult& OutResult, FString& OutError)
{
	OutResult = FProtokitePlaytestSessionStartResult();
	FString SessionId;
	Object->TryGetStringField(TEXT("session_id"), SessionId);
	if (!IsUsablePlaytestId(SessionId, MAX_int32))
	{
		OutError = FString::Printf(TEXT("Protokite's answer names no usable session id: '%s'."), *SessionId);
		return false;
	}
	OutResult.SessionId = SessionId;
	return true;
}

bool FProtokitePlaytestSessionEndResult::FromWireObject(const TSharedRef<FJsonObject>& Object,
	FProtokitePlaytestSessionEndResult& OutResult, FString& OutError)
{
	OutResult = FProtokitePlaytestSessionEndResult();
	return true;
}

FString FProtokitePlaytestSessionStartRequest::ToJson() const
{
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	if (!Identity.SteamId.IsEmpty())
	{
		Body->SetStringField(TEXT("steam_id"), Identity.SteamId);
	}
	if (!Identity.DeviceId.IsEmpty())
	{
		Body->SetStringField(TEXT("device_id"), Identity.DeviceId);
	}
	if (!Identity.PlayerName.IsEmpty())
	{
		Body->SetStringField(TEXT("player_name"), Identity.PlayerName);
	}
	if (!FlockSessionId.IsEmpty())
	{
		Body->SetStringField(TEXT("flock_session_id"), FlockSessionId);
	}

	const TSharedRef<FJsonObject> Debug = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Fact : DebugInfo)
	{
		Debug->SetStringField(Fact.Key, Fact.Value);
	}
	Body->SetObjectField(TEXT("extra_debug"), Debug);

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Body, Writer);
	return Json;
}

TMap<FString, FString> MakePlaytestSessionDebugInfo(const FString& MapName, EProtokitePlaytestConsentChoice PlayerConsent,
	bool bAskedThePlayer)
{
	TMap<FString, FString> Facts;
	// First, because it is what says whether the rest of this session was allowed to hold anything at all.
	Facts.Add(TEXT("playtest_consent"), ProtokitePlaytestConsent::ToWire(PlayerConsent));
	Facts.Add(TEXT("playtest_consent_asked"), bAskedThePlayer ? TEXT("true") : TEXT("false"));
	Facts.Add(TEXT("engine_version"), FEngineVersion::Current().ToString());
	Facts.Add(TEXT("build_configuration"), LexToString(FApp::GetBuildConfiguration()));
	const FString Gpu = FPlatformMisc::GetPrimaryGPUBrand();
	if (!Gpu.IsEmpty())
	{
		Facts.Add(TEXT("gpu"), Gpu);
	}
	if (!MapName.IsEmpty())
	{
		Facts.Add(TEXT("map"), MapName);
	}
	Facts.Add(TEXT("sdk_version"), UFlockSubsystem::SdkVersion);
	return Facts;
}

bool FProtokitePlaytestFormSubmitResult::FromWireObject(const TSharedRef<FJsonObject>& Object,
	FProtokitePlaytestFormSubmitResult& OutResult, FString& OutError)
{
	OutResult = FProtokitePlaytestFormSubmitResult();
	return true;
}
