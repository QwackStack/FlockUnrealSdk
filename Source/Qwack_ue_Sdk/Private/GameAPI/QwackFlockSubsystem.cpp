#include "QwackFlockSubsystem.h"

#include "JsonObjectConverter.h"
#include "Misc/Paths.h"
#include "Qwack_ue_Sdk/Cache/FlockEventSpool.h"
#include "Qwack_ue_Sdk/Config/QwackSettings.h"
#include "Qwack_ue_Sdk/Endpoints/QwackGameEndpoints.h"
#include "Qwack_ue_Sdk/HTTPClient/HTTPResponse.h"
#include "Qwack_ue_Sdk/HTTPClient/SHTTPClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogFlockSdk, Log, All);

namespace
{
	// 2xx = the server accepted the payload — drop from cache.
	bool IsHttpSuccess(int32 Code) { return Code >= 200 && Code < 300; }

	// 4xx except 408 (timeout) and 429 (rate limit) — retrying won't help, drop from cache.
	// Everything else (0, 5xx, network failure) is transient and stays on disk.
	bool IsHttpPermanent(int32 Code)
	{
		return Code >= 400 && Code < 500 && Code != 408 && Code != 429;
	}
}

namespace
{
	// If Parent[Key] is a string that parses as a JSON object, replace it with the parsed object.
	// If the string is empty, the field is removed. Otherwise the string is left as-is.
	void EmbedJsonStringField(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Key)
	{
		if (!Parent.IsValid()) return;
		FString StrVal;
		if (!Parent->TryGetStringField(Key, StrVal)) return;
		if (StrVal.IsEmpty()) { Parent->RemoveField(Key); return; }

		TSharedPtr<FJsonObject> AsObj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(StrVal);
		if (FJsonSerializer::Deserialize(Reader, AsObj) && AsObj.IsValid())
		{
			Parent->SetObjectField(Key, AsObj);
		}
	}

	FString JsonObjectToString(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	TSharedPtr<FJsonObject> StructToJsonObject(const UStruct* StructDef, const void* StructPtr)
	{
		const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!FJsonObjectConverter::UStructToJsonObject(StructDef, StructPtr, Obj, 0, 0))
		{
			return nullptr;
		}
		return Obj;
	}
}

// =====================================================================
// Lifecycle
// =====================================================================

void UQwackFlockSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	HttpClient = NewObject<USHTTPClient>(this);

	if (const UQwackSettings* S = GetDefault<UQwackSettings>())
	{
		if (S->AnalyticsCacheFailedEvents)
		{
			const FString Root = FPaths::ProjectSavedDir() / TEXT("Flock");
			AnalyticsSpool = MakeUnique<FFlockEventSpool>(
				Root / TEXT("analytics_events"),
				S->AnalyticsMaxCachedEvents,
				S->AnalyticsCacheFlushBatchSize);
			LogEventSpool = MakeUnique<FFlockEventSpool>(
				Root / TEXT("log_events"),
				S->AnalyticsMaxCachedEvents,
				S->AnalyticsCacheFlushBatchSize);
		}
	}
}

void UQwackFlockSubsystem::Deinitialize()
{
	// Reset spools before HttpClient — they don't own the http client but the
	// in-flight callbacks reach back through TWeakObjectPtr<this>, which goes
	// stale once Super::Deinitialize returns.
	AnalyticsSpool.Reset();
	LogEventSpool.Reset();
	HttpClient = nullptr;
	Super::Deinitialize();
}

void UQwackFlockSubsystem::SetAccessToken(const FString& InAccessToken)
{
	if (AccessToken == InAccessToken) return;
	AccessToken = InAccessToken;
	OnAccessTokenChanged.Broadcast(AccessToken);

	// Token just became usable — drain anything that piled up while we were unauthenticated.
	if (!AccessToken.IsEmpty())
	{
		FlushSpoolAsBatch(EFlockSpool::Analytics);
		FlushSpoolAsBatch(EFlockSpool::LogEvents);
	}
}

void UQwackFlockSubsystem::StoreAuthFromResponse(const FFlockPlayerAuth& Auth)
{
	PlayerId = Auth.player_id;
	RefreshTokenValue = Auth.refresh_token;
	if (AccessToken != Auth.access_token)
	{
		AccessToken = Auth.access_token;
		OnAccessTokenChanged.Broadcast(AccessToken);
	}

	if (!AccessToken.IsEmpty())
	{
		FlushSpoolAsBatch(EFlockSpool::Analytics);
		FlushSpoolAsBatch(EFlockSpool::LogEvents);
	}
}

// =====================================================================
// Helpers
// =====================================================================

FString UQwackFlockSubsystem::BuildUrl(const FString& Path) const
{
	const UQwackSettings* S = GetDefault<UQwackSettings>();
	FString Base = S ? S->ApiUrl : FString();
	if (Base.IsEmpty())
	{
		UE_LOG(LogFlockSdk, Warning, TEXT("UQwackSettings::ApiUrl is empty — set it in Project Settings → Plugins → Flock"));
		return FString();
	}
	if (Base.EndsWith(TEXT("/"))) Base.LeftChopInline(1);
	return Base + Path;
}

TMap<FString, FString> UQwackFlockSubsystem::MakeHeaders(bool bIncludeAuth) const
{
	TMap<FString, FString> Headers;
	const UQwackSettings* S = GetDefault<UQwackSettings>();
	if (S)
	{
		if (!S->ApiKey.IsEmpty())
		{
			Headers.Add(TEXT("X-Flock-API-Key"), S->ApiKey);
		}
		if (!S->GameVersion.IsEmpty())
		{
			Headers.Add(TEXT("X-Game-Version-ID"), S->GameVersion);
		}
	}
	if (bIncludeAuth && !AccessToken.IsEmpty())
	{
		Headers.Add(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));
	}
	return Headers;
}

FFlockOpResult UQwackFlockSubsystem::MakeMeta(const FQwackHTTPResponse& R)
{
	FFlockOpResult Meta;
	Meta.StatusCode = R.StatusCode;
	Meta.bSuccess = (R.StatusCode >= 200 && R.StatusCode < 300);
	Meta.ResultJson = R.FullText;
	if (!Meta.bSuccess) Meta.ErrorMessage = R.FullText;
	return Meta;
}

void UQwackFlockSubsystem::Send(const FSQwackFlockEndpoints& Endpoint,
                                const FString& UrlOverride,
                                const FString& Body,
                                bool bIncludeAuth,
                                TFunction<void(const FQwackHTTPResponse&)> OnDone) const
{
	if (!HttpClient)
	{
		FQwackHTTPResponse R; R.StatusCode = 0; R.FullText = TEXT("HttpClient not initialized");
		OnDone(R); return;
	}
	const FString Url = UrlOverride.IsEmpty() ? BuildUrl(Endpoint.EndPoint) : UrlOverride;
	if (Url.IsEmpty())
	{
		FQwackHTTPResponse R; R.StatusCode = 0; R.FullText = TEXT("Empty URL — GameBaseURI not configured");
		OnDone(R); return;
	}
	const TCHAR* Verb = UQwackFlockGameEndpoints::QwackHttpVerb(Endpoint.RequestType);
	TMap<FString, FString> Headers = MakeHeaders(bIncludeAuth);

	FQwackFlockResponse Cb;
	Cb.BindLambda([OnDone](FQwackHTTPResponse R) { OnDone(R); });
	HttpClient->SendRequest(Url, Verb, Body, Cb, Headers);
}

// =====================================================================
// Serialization helpers for endpoints with free-form JSON fields
// =====================================================================

FString UQwackFlockSubsystem::SerializeAnalyticsEvent(const FFlockAnalyticsEventRequest& Req)
{
	const TSharedPtr<FJsonObject> Obj = StructToJsonObject(FFlockAnalyticsEventRequest::StaticStruct(), &Req);
	if (!Obj.IsValid()) return TEXT("{}");
	Obj->RemoveField(TEXT("PropertiesJson"));
	if (!Req.PropertiesJson.IsEmpty())
	{
		TSharedPtr<FJsonObject> Props;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Req.PropertiesJson);
		if (FJsonSerializer::Deserialize(Reader, Props) && Props.IsValid())
		{
			Obj->SetObjectField(TEXT("properties"), Props);
		}
	}
	return JsonObjectToString(Obj.ToSharedRef());
}

FString UQwackFlockSubsystem::SerializeAnalyticsEvents(const FFlockAnalyticsEventsRequest& Req)
{
	TArray<TSharedPtr<FJsonValue>> EventValues;
	EventValues.Reserve(Req.events.Num());
	for (const FFlockAnalyticsEventRequest& E : Req.events)
	{
		const FString S = SerializeAnalyticsEvent(E);
		TSharedPtr<FJsonObject> EventObj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(S);
		if (FJsonSerializer::Deserialize(Reader, EventObj) && EventObj.IsValid())
		{
			EventValues.Add(MakeShared<FJsonValueObject>(EventObj));
		}
	}
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("events"), EventValues);
	return JsonObjectToString(Root);
}

FString UQwackFlockSubsystem::SerializeLogEvent(const FFlockLogEventRequest& Req)
{
	const TSharedPtr<FJsonObject> Obj = StructToJsonObject(FFlockLogEventRequest::StaticStruct(), &Req);
	if (!Obj.IsValid()) return TEXT("{}");
	const TSharedPtr<FJsonObject>* DataObjPtr = nullptr;
	if (Obj->TryGetObjectField(TEXT("data"), DataObjPtr) && DataObjPtr && DataObjPtr->IsValid())
	{
		EmbedJsonStringField(*DataObjPtr, TEXT("error_data"));
		EmbedJsonStringField(*DataObjPtr, TEXT("extra_data"));
	}
	return JsonObjectToString(Obj.ToSharedRef());
}

FString UQwackFlockSubsystem::SerializeLogEvents(const FFlockLogEventsRequest& Req)
{
	TArray<TSharedPtr<FJsonValue>> EventValues;
	EventValues.Reserve(Req.events.Num());
	for (const FFlockLogEventRequest& E : Req.events)
	{
		const FString S = SerializeLogEvent(E);
		TSharedPtr<FJsonObject> EventObj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(S);
		if (FJsonSerializer::Deserialize(Reader, EventObj) && EventObj.IsValid())
		{
			EventValues.Add(MakeShared<FJsonValueObject>(EventObj));
		}
	}
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("events"), EventValues);
	return JsonObjectToString(Root);
}

// =====================================================================
// Auth
// =====================================================================

void UQwackFlockSubsystem::RegisterPlayer(const FFlockPlayerRegisterRequest& Request, const FFlockOnAuthResponse& Callback)
{
	FString Body;
	FJsonObjectConverter::UStructToJsonObjectString(Request, Body, 0, 0);

	Send(UQwackFlockGameEndpoints::PlayerRegister, FString(), Body, /*bIncludeAuth*/ false,
		[this, Callback](const FQwackHTTPResponse& R)
		{
			FFlockPlayerAuthResponse Out;
			Out.Meta = MakeMeta(R);
			if (Out.Meta.bSuccess)
			{
				FJsonObjectConverter::JsonObjectStringToUStruct(R.FullText, &Out.Auth, 0, 0);
				StoreAuthFromResponse(Out.Auth);
			}
			Callback.ExecuteIfBound(Out);
		});
}

void UQwackFlockSubsystem::LoginPlayer(const FFlockPlayerLoginRequest& Request, const FFlockOnAuthResponse& Callback)
{
	FString Body;
	FJsonObjectConverter::UStructToJsonObjectString(Request, Body, 0, 0);

	Send(UQwackFlockGameEndpoints::PlayerLogin, FString(), Body, /*bIncludeAuth*/ false,
		[this, Callback](const FQwackHTTPResponse& R)
		{
			FFlockPlayerAuthResponse Out;
			Out.Meta = MakeMeta(R);
			if (Out.Meta.bSuccess)
			{
				FJsonObjectConverter::JsonObjectStringToUStruct(R.FullText, &Out.Auth, 0, 0);
				StoreAuthFromResponse(Out.Auth);
			}
			Callback.ExecuteIfBound(Out);
		});
}

void UQwackFlockSubsystem::RefreshToken(const FFlockPlayerRefreshRequest& Request, const FFlockOnAuthResponse& Callback)
{
	FString Body;
	FJsonObjectConverter::UStructToJsonObjectString(Request, Body, 0, 0);

	Send(UQwackFlockGameEndpoints::PlayerTokenRefresh, FString(), Body, /*bIncludeAuth*/ false,
		[this, Callback](const FQwackHTTPResponse& R)
		{
			FFlockPlayerAuthResponse Out;
			Out.Meta = MakeMeta(R);
			if (Out.Meta.bSuccess)
			{
				FJsonObjectConverter::JsonObjectStringToUStruct(R.FullText, &Out.Auth, 0, 0);
				StoreAuthFromResponse(Out.Auth);
			}
			Callback.ExecuteIfBound(Out);
		});
}

void UQwackFlockSubsystem::AuthTest(const FFlockOnAuthTestResponse& Callback)
{
	Send(UQwackFlockGameEndpoints::PlayerAuthTest, FString(), FString(), /*bIncludeAuth*/ true,
		[Callback](const FQwackHTTPResponse& R)
		{
			FFlockAuthTestResponse Out;
			Out.Meta = MakeMeta(R);
			Out.RequesterJson = R.FullText;
			Callback.ExecuteIfBound(Out);
		});
}

// =====================================================================
// Sessions
// =====================================================================

void UQwackFlockSubsystem::StartSession(const FFlockSessionStartRequest& Request, const FFlockOnSessionStart& Callback)
{
	FString Body;
	FJsonObjectConverter::UStructToJsonObjectString(Request, Body, 0, 0);

	Send(UQwackFlockGameEndpoints::StartSession, FString(), Body, /*bIncludeAuth*/ true,
		[Callback](const FQwackHTTPResponse& R)
		{
			FFlockSessionStartResponse Out;
			Out.Meta = MakeMeta(R);
			if (Out.Meta.bSuccess)
			{
				TSharedPtr<FJsonObject> Obj;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(R.FullText);
				if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid())
				{
					Obj->TryGetStringField(TEXT("session_id"), Out.session_id);
				}
			}
			Callback.ExecuteIfBound(Out);
		});
}

void UQwackFlockSubsystem::EndSession(const FString& SessionId, const FFlockSessionEndRequest& Request, const FFlockOnGenericResponse& Callback)
{
	const FString Path = UQwackFlockGameEndpoints::EndSession.EndPoint.Replace(TEXT("{session_id}"), *SessionId);
	const FString Url = BuildUrl(Path);

	FString Body;
	FJsonObjectConverter::UStructToJsonObjectString(Request, Body, 0, 0);

	Send(UQwackFlockGameEndpoints::EndSession, Url, Body, /*bIncludeAuth*/ true,
		[Callback](const FQwackHTTPResponse& R)
		{
			FFlockGenericResponse Out;
			Out.Meta = MakeMeta(R);
			Callback.ExecuteIfBound(Out);
		});
}

// =====================================================================
// Events
// =====================================================================

void UQwackFlockSubsystem::TrackEvent(const FFlockAnalyticsEventRequest& Request, const FFlockOnGenericResponse& Callback)
{
	const FString Body = SerializeAnalyticsEvent(Request);

	// Write-ahead: persist the wire-ready body before sending. If the live send
	// succeeds we Remove(Handle); if it fails transiently the file stays for the
	// next flush trigger to retry as part of a batch.
	TArray<FString> Handles;
	if (AnalyticsSpool)
	{
		Handles.Add(AnalyticsSpool->Enqueue(Body));
	}

	TWeakObjectPtr<UQwackFlockSubsystem> WeakThis(this);
	Send(UQwackFlockGameEndpoints::TrackEvent, FString(), Body, /*bIncludeAuth*/ true,
		[WeakThis, Handles, Callback](const FQwackHTTPResponse& R)
		{
			FFlockGenericResponse Out; Out.Meta = MakeMeta(R);
			if (UQwackFlockSubsystem* Strong = WeakThis.Get())
			{
				Strong->OnSpoolResponse(EFlockSpool::Analytics, Handles, R);
			}
			Callback.ExecuteIfBound(Out);
		});
}

void UQwackFlockSubsystem::TrackEvents(const FFlockAnalyticsEventsRequest& Request, const FFlockOnGenericResponse& Callback)
{
	// Serialize each event once: feed both the per-event spool entry and the
	// batch payload from the same JSON, instead of walking UStruct reflection twice.
	TArray<FString> Bodies;
	Bodies.Reserve(Request.events.Num());
	for (const FFlockAnalyticsEventRequest& E : Request.events)
	{
		Bodies.Add(SerializeAnalyticsEvent(E));
	}

	TArray<FString> Handles;
	if (AnalyticsSpool && Bodies.Num() > 0)
	{
		Handles.Reserve(Bodies.Num());
		for (const FString& B : Bodies)
		{
			Handles.Add(AnalyticsSpool->Enqueue(B));
		}
	}

	FString Payload;
	Payload.Reserve(16 + Bodies.Num() * 256);
	Payload += TEXT("{\"events\":[");
	for (int32 i = 0; i < Bodies.Num(); ++i)
	{
		if (i > 0) Payload += TEXT(",");
		Payload += Bodies[i];
	}
	Payload += TEXT("]}");

	TWeakObjectPtr<UQwackFlockSubsystem> WeakThis(this);
	Send(UQwackFlockGameEndpoints::TrackEvents, FString(), Payload, /*bIncludeAuth*/ true,
		[WeakThis, Handles, Callback](const FQwackHTTPResponse& R)
		{
			FFlockGenericResponse Out; Out.Meta = MakeMeta(R);
			if (UQwackFlockSubsystem* Strong = WeakThis.Get())
			{
				Strong->OnSpoolResponse(EFlockSpool::Analytics, Handles, R);
			}
			Callback.ExecuteIfBound(Out);
		});
}

void UQwackFlockSubsystem::RecordTransaction(const FFlockTransactionRequest& Request, const FFlockOnGenericResponse& Callback)
{
	FString Body;
	FJsonObjectConverter::UStructToJsonObjectString(Request, Body, 0, 0);
	Send(UQwackFlockGameEndpoints::RecordTransaction, FString(), Body, /*bIncludeAuth*/ true,
		[Callback](const FQwackHTTPResponse& R)
		{
			FFlockGenericResponse Out; Out.Meta = MakeMeta(R);
			Callback.ExecuteIfBound(Out);
		});
}

// =====================================================================
// Log events
// =====================================================================

namespace
{
	FString CurrentGameVersion()
	{
		if (const UQwackSettings* S = GetDefault<UQwackSettings>()) return S->GameVersion;
		return FString();
	}
}

void UQwackFlockSubsystem::LogDebug(const FString& Message, const FString& ExtraDataJson, const FFlockOnGenericResponse& Callback)
{
	FFlockLogEventRequest Req;
	Req.message = Message;
	Req.timestamp = FDateTime::UtcNow().ToIso8601();
	Req.data.type = EFlockLogEventType::debug;
	Req.data.game_version = CurrentGameVersion();
	Req.data.extra_data = ExtraDataJson;
	LogEvent(Req, Callback);
}

void UQwackFlockSubsystem::LogError(const FString& Message, const FString& LogicalExpression, const FString& ExtraDataJson, const FFlockOnGenericResponse& Callback)
{
	FFlockLogEventRequest Req;
	Req.message = Message;
	Req.timestamp = FDateTime::UtcNow().ToIso8601();
	Req.data.type = EFlockLogEventType::logic_error;
	Req.data.game_version = CurrentGameVersion();
	Req.data.logical_expression = LogicalExpression;
	Req.data.extra_data = ExtraDataJson;
	LogEvent(Req, Callback);
}

void UQwackFlockSubsystem::LogException(const FString& Message, const FString& ErrorMessage, const FString& ErrorCode, const FString& Traceback, const FString& ErrorDataJson, const FString& ExtraDataJson, const FFlockOnGenericResponse& Callback)
{
	FFlockLogEventRequest Req;
	Req.message = Message;
	Req.timestamp = FDateTime::UtcNow().ToIso8601();
	Req.data.type = EFlockLogEventType::exception;
	Req.data.game_version = CurrentGameVersion();
	Req.data.error_message = ErrorMessage;
	Req.data.error_code = ErrorCode;
	Req.data.error_traceback = Traceback;
	Req.data.error_data = ErrorDataJson;
	Req.data.extra_data = ExtraDataJson;
	LogEvent(Req, Callback);
}

void UQwackFlockSubsystem::LogEvent(const FFlockLogEventRequest& Request, const FFlockOnGenericResponse& Callback)
{
	const FString Body = SerializeLogEvent(Request);

	TArray<FString> Handles;
	if (LogEventSpool)
	{
		Handles.Add(LogEventSpool->Enqueue(Body));
	}

	TWeakObjectPtr<UQwackFlockSubsystem> WeakThis(this);
	Send(UQwackFlockGameEndpoints::LogEvent, FString(), Body, /*bIncludeAuth*/ true,
		[WeakThis, Handles, Callback](const FQwackHTTPResponse& R)
		{
			FFlockGenericResponse Out; Out.Meta = MakeMeta(R);
			if (UQwackFlockSubsystem* Strong = WeakThis.Get())
			{
				Strong->OnSpoolResponse(EFlockSpool::LogEvents, Handles, R);
			}
			Callback.ExecuteIfBound(Out);
		});
}

void UQwackFlockSubsystem::LogEvents(const FFlockLogEventsRequest& Request, const FFlockOnGenericResponse& Callback)
{
	TArray<FString> Bodies;
	Bodies.Reserve(Request.events.Num());
	for (const FFlockLogEventRequest& E : Request.events)
	{
		Bodies.Add(SerializeLogEvent(E));
	}

	TArray<FString> Handles;
	if (LogEventSpool && Bodies.Num() > 0)
	{
		Handles.Reserve(Bodies.Num());
		for (const FString& B : Bodies)
		{
			Handles.Add(LogEventSpool->Enqueue(B));
		}
	}

	FString Payload;
	Payload.Reserve(16 + Bodies.Num() * 256);
	Payload += TEXT("{\"events\":[");
	for (int32 i = 0; i < Bodies.Num(); ++i)
	{
		if (i > 0) Payload += TEXT(",");
		Payload += Bodies[i];
	}
	Payload += TEXT("]}");

	TWeakObjectPtr<UQwackFlockSubsystem> WeakThis(this);
	Send(UQwackFlockGameEndpoints::LogEvents, FString(), Payload, /*bIncludeAuth*/ true,
		[WeakThis, Handles, Callback](const FQwackHTTPResponse& R)
		{
			FFlockGenericResponse Out; Out.Meta = MakeMeta(R);
			if (UQwackFlockSubsystem* Strong = WeakThis.Get())
			{
				Strong->OnSpoolResponse(EFlockSpool::LogEvents, Handles, R);
			}
			Callback.ExecuteIfBound(Out);
		});
}

// =====================================================================
// Spool dispatch
// =====================================================================

FFlockEventSpool* UQwackFlockSubsystem::GetSpool(EFlockSpool Which) const
{
	switch (Which)
	{
	case EFlockSpool::Analytics: return AnalyticsSpool.Get();
	case EFlockSpool::LogEvents: return LogEventSpool.Get();
	default: return nullptr;
	}
}

const FSQwackFlockEndpoints& UQwackFlockSubsystem::GetBatchEndpoint(EFlockSpool Which)
{
	return Which == EFlockSpool::Analytics
		? UQwackFlockGameEndpoints::TrackEvents
		: UQwackFlockGameEndpoints::LogEvents;
}

void UQwackFlockSubsystem::OnSpoolResponse(EFlockSpool Which, const TArray<FString>& Handles, const FQwackHTTPResponse& R)
{
	FFlockEventSpool* S = GetSpool(Which);
	if (!S || Handles.Num() == 0) return;

	if (IsHttpSuccess(R.StatusCode) || IsHttpPermanent(R.StatusCode))
	{
		S->RemoveMany(Handles);
	}

	// Opportunistic drain: we just confirmed connectivity to the server, so
	// take the chance to send anything that piled up while we were offline.
	if (IsHttpSuccess(R.StatusCode) && S->PendingCount() > 0)
	{
		FlushSpoolAsBatch(Which);
	}
}

void UQwackFlockSubsystem::FlushSpoolAsBatch(EFlockSpool Which)
{
	FFlockEventSpool* S = GetSpool(Which);
	if (!S || S->PendingCount() == 0) return;
	if (!S->TryBeginFlush()) return;

	TArray<FString> Handles;
	TArray<FString> Bodies;
	S->ReadBatch(Handles, Bodies);
	if (Bodies.Num() == 0)
	{
		S->EndFlush();
		return;
	}

	FString Payload;
	Payload.Reserve(16 + Bodies.Num() * 256);
	Payload += TEXT("{\"events\":[");
	for (int32 i = 0; i < Bodies.Num(); ++i)
	{
		if (i > 0) Payload += TEXT(",");
		Payload += Bodies[i];
	}
	Payload += TEXT("]}");

	TWeakObjectPtr<UQwackFlockSubsystem> WeakThis(this);
	Send(GetBatchEndpoint(Which), FString(), Payload, /*bIncludeAuth*/ true,
		[WeakThis, Which, Handles](const FQwackHTTPResponse& R)
		{
			UQwackFlockSubsystem* Strong = WeakThis.Get();
			if (!Strong) return;
			FFlockEventSpool* Spool = Strong->GetSpool(Which);
			if (!Spool) return;

			if (IsHttpSuccess(R.StatusCode) || IsHttpPermanent(R.StatusCode))
			{
				Spool->RemoveMany(Handles);
			}
			Spool->EndFlush();

			// Keep draining while we're online and there's still work to do.
			if (IsHttpSuccess(R.StatusCode) && Spool->PendingCount() > 0)
			{
				Strong->FlushSpoolAsBatch(Which);
			}
		});
}
