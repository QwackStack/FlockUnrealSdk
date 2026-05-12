#include "QwackLogEventSubsystem.h"

#include "Engine/GameInstance.h"
#include "JsonObjectConverter.h"
#include "Misc/Paths.h"
#include "Qwack_ue_Sdk/Auth/QwackAuthSubsystem.h"
#include "Qwack_ue_Sdk/Cache/FlockEventSpool.h"
#include "Qwack_ue_Sdk/Config/QwackConfigSubsystem.h"
#include "Qwack_ue_Sdk/Config/QwackSettings.h"
#include "Qwack_ue_Sdk/Endpoints/QwackGameEndpoints.h"
#include "Qwack_ue_Sdk/GameAPI/QwackFlockGameSubsystem.h"
#include "Qwack_ue_Sdk/GameAPI/QwackJsonHelpers.h"
#include "Qwack_ue_Sdk/HTTPClient/HTTPResponse.h"

namespace
{
	bool IsHttpSuccess(int32 Code) { return Code >= 200 && Code < 300; }

	bool IsHttpPermanent(int32 Code)
	{
		return Code >= 400 && Code < 500 && Code != 408 && Code != 429;
	}

	UQwackFlockGameSubsystem* GetGame(const UGameInstanceSubsystem* Self)
	{
		if (!Self) return nullptr;
		const UGameInstance* GI = Self->GetGameInstance();
		return GI ? GI->GetSubsystem<UQwackFlockGameSubsystem>() : nullptr;
	}

	FString CurrentGameVersion(const UGameInstanceSubsystem* Self)
	{
		if (!Self) return FString();
		if (const UGameInstance* GI = Self->GetGameInstance())
		{
			if (const UQwackConfigSubsystem* Config = GI->GetSubsystem<UQwackConfigSubsystem>())
			{
				return Config->GetGameVersion();
			}
		}
		return FString();
	}
}

void UQwackLogEventSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (const UGameInstance* GI = GetGameInstance())
	{
		const UQwackConfigSubsystem* Config = GI->GetSubsystem<UQwackConfigSubsystem>();
		const UQwackSettings* S = Config ? Config->GetSettings() : GetDefault<UQwackSettings>();
		if (S && S->AnalyticsCacheFailedEvents)
		{
			const FString Root = FPaths::ProjectSavedDir() / TEXT("Flock");
			Spool = MakeUnique<FFlockEventSpool>(
				Root / TEXT("log_events"),
				S->AnalyticsMaxCachedEvents,
				S->AnalyticsCacheFlushBatchSize);
		}

		if (UQwackAuthSubsystem* Auth = GI->GetSubsystem<UQwackAuthSubsystem>())
		{
			Auth->OnAccessTokenChanged.AddDynamic(this, &UQwackLogEventSubsystem::HandleAccessTokenChanged);
		}
	}
}

void UQwackLogEventSubsystem::Deinitialize()
{
	if (const UGameInstance* GI = GetGameInstance())
	{
		if (UQwackAuthSubsystem* Auth = GI->GetSubsystem<UQwackAuthSubsystem>())
		{
			Auth->OnAccessTokenChanged.RemoveDynamic(this, &UQwackLogEventSubsystem::HandleAccessTokenChanged);
		}
	}
	Spool.Reset();
	Super::Deinitialize();
}

void UQwackLogEventSubsystem::HandleAccessTokenChanged(const FString& Token)
{
	if (!Token.IsEmpty())
	{
		FlushSpoolAsBatch();
	}
}

// =====================================================================
// Serialization
// =====================================================================

FString UQwackLogEventSubsystem::SerializeEvent(const FFlockLogEventRequest& Req)
{
	const TSharedPtr<FJsonObject> Obj = QwackJson::StructToJsonObject(FFlockLogEventRequest::StaticStruct(), &Req);
	if (!Obj.IsValid()) return TEXT("{}");
	const TSharedPtr<FJsonObject>* DataObjPtr = nullptr;
	if (Obj->TryGetObjectField(TEXT("data"), DataObjPtr) && DataObjPtr && DataObjPtr->IsValid())
	{
		QwackJson::EmbedJsonStringField(*DataObjPtr, TEXT("error_data"));
		QwackJson::EmbedJsonStringField(*DataObjPtr, TEXT("extra_data"));
	}
	return QwackJson::JsonObjectToString(Obj.ToSharedRef());
}

FString UQwackLogEventSubsystem::BuildBatchPayload(const TArray<FString>& Bodies)
{
	FString Payload;
	Payload.Reserve(16 + Bodies.Num() * 256);
	Payload += TEXT("{\"events\":[");
	for (int32 i = 0; i < Bodies.Num(); ++i)
	{
		if (i > 0) Payload += TEXT(",");
		Payload += Bodies[i];
	}
	Payload += TEXT("]}");
	return Payload;
}

// =====================================================================
// Convenience entry points
// =====================================================================

void UQwackLogEventSubsystem::LogDebug(const FString& Message, const FString& ExtraDataJson, const FFlockOnGenericResponse& Callback)
{
	FFlockLogEventRequest Req;
	Req.message = Message;
	Req.timestamp = FDateTime::UtcNow().ToIso8601();
	Req.data.type = EFlockLogEventType::debug;
	Req.data.game_version = CurrentGameVersion(this);
	Req.data.extra_data = ExtraDataJson;
	LogEvent(Req, Callback);
}

void UQwackLogEventSubsystem::LogError(const FString& Message, const FString& LogicalExpression, const FString& ExtraDataJson, const FFlockOnGenericResponse& Callback)
{
	FFlockLogEventRequest Req;
	Req.message = Message;
	Req.timestamp = FDateTime::UtcNow().ToIso8601();
	Req.data.type = EFlockLogEventType::logic_error;
	Req.data.game_version = CurrentGameVersion(this);
	Req.data.logical_expression = LogicalExpression;
	Req.data.extra_data = ExtraDataJson;
	LogEvent(Req, Callback);
}

void UQwackLogEventSubsystem::LogException(const FString& Message, const FString& ErrorMessage, const FString& ErrorCode, const FString& Traceback, const FString& ErrorDataJson, const FString& ExtraDataJson, const FFlockOnGenericResponse& Callback)
{
	FFlockLogEventRequest Req;
	Req.message = Message;
	Req.timestamp = FDateTime::UtcNow().ToIso8601();
	Req.data.type = EFlockLogEventType::exception;
	Req.data.game_version = CurrentGameVersion(this);
	Req.data.error_message = ErrorMessage;
	Req.data.error_code = ErrorCode;
	Req.data.error_traceback = Traceback;
	Req.data.error_data = ErrorDataJson;
	Req.data.extra_data = ExtraDataJson;
	LogEvent(Req, Callback);
}

void UQwackLogEventSubsystem::LogEvent(const FFlockLogEventRequest& Request, const FFlockOnGenericResponse& Callback)
{
	UQwackFlockGameSubsystem* Game = GetGame(this);
	if (!Game) { FFlockGenericResponse Out; Callback.ExecuteIfBound(Out); return; }

	const FString Body = SerializeEvent(Request);

	TArray<FString> Handles;
	if (Spool)
	{
		Handles.Add(Spool->Enqueue(Body));
	}

	TWeakObjectPtr<UQwackLogEventSubsystem> WeakThis(this);
	Game->Send(UQwackFlockGameEndpoints::LogEvent, FString(), Body, /*bIncludeAuth*/ true,
		[WeakThis, Handles, Callback](const FQwackHTTPResponse& R)
		{
			FFlockGenericResponse Out; Out.Meta = UQwackFlockGameSubsystem::MakeMeta(R);
			if (UQwackLogEventSubsystem* Strong = WeakThis.Get())
			{
				Strong->OnSpoolResponse(Handles, R);
			}
			Callback.ExecuteIfBound(Out);
		});
}

void UQwackLogEventSubsystem::LogEvents(const FFlockLogEventsRequest& Request, const FFlockOnGenericResponse& Callback)
{
	UQwackFlockGameSubsystem* Game = GetGame(this);
	if (!Game) { FFlockGenericResponse Out; Callback.ExecuteIfBound(Out); return; }

	TArray<FString> Bodies;
	Bodies.Reserve(Request.events.Num());
	for (const FFlockLogEventRequest& E : Request.events)
	{
		Bodies.Add(SerializeEvent(E));
	}

	TArray<FString> Handles;
	if (Spool && Bodies.Num() > 0)
	{
		Handles.Reserve(Bodies.Num());
		for (const FString& B : Bodies)
		{
			Handles.Add(Spool->Enqueue(B));
		}
	}

	const FString Payload = BuildBatchPayload(Bodies);

	TWeakObjectPtr<UQwackLogEventSubsystem> WeakThis(this);
	Game->Send(UQwackFlockGameEndpoints::LogEvents, FString(), Payload, /*bIncludeAuth*/ true,
		[WeakThis, Handles, Callback](const FQwackHTTPResponse& R)
		{
			FFlockGenericResponse Out; Out.Meta = UQwackFlockGameSubsystem::MakeMeta(R);
			if (UQwackLogEventSubsystem* Strong = WeakThis.Get())
			{
				Strong->OnSpoolResponse(Handles, R);
			}
			Callback.ExecuteIfBound(Out);
		});
}

// =====================================================================
// Spool dispatch
// =====================================================================

void UQwackLogEventSubsystem::OnSpoolResponse(const TArray<FString>& Handles, const FQwackHTTPResponse& R)
{
	if (!Spool || Handles.Num() == 0) return;

	if (IsHttpSuccess(R.StatusCode) || IsHttpPermanent(R.StatusCode))
	{
		Spool->RemoveMany(Handles);
	}

	if (IsHttpSuccess(R.StatusCode) && Spool->PendingCount() > 0)
	{
		FlushSpoolAsBatch();
	}
}

void UQwackLogEventSubsystem::FlushSpoolAsBatch()
{
	if (!Spool || Spool->PendingCount() == 0) return;
	if (!Spool->TryBeginFlush()) return;

	TArray<FString> Handles;
	TArray<FString> Bodies;
	Spool->ReadBatch(Handles, Bodies);
	if (Bodies.Num() == 0)
	{
		Spool->EndFlush();
		return;
	}

	UQwackFlockGameSubsystem* Game = GetGame(this);
	if (!Game)
	{
		Spool->EndFlush();
		return;
	}

	const FString Payload = BuildBatchPayload(Bodies);

	TWeakObjectPtr<UQwackLogEventSubsystem> WeakThis(this);
	Game->Send(UQwackFlockGameEndpoints::LogEvents, FString(), Payload, /*bIncludeAuth*/ true,
		[WeakThis, Handles](const FQwackHTTPResponse& R)
		{
			UQwackLogEventSubsystem* Strong = WeakThis.Get();
			if (!Strong || !Strong->Spool) return;

			if (IsHttpSuccess(R.StatusCode) || IsHttpPermanent(R.StatusCode))
			{
				Strong->Spool->RemoveMany(Handles);
			}
			Strong->Spool->EndFlush();

			if (IsHttpSuccess(R.StatusCode) && Strong->Spool->PendingCount() > 0)
			{
				Strong->FlushSpoolAsBatch();
			}
		});
}
