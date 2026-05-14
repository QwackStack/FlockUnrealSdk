#include "QwackAnalyticsSubsystem.h"

#include "Engine/GameInstance.h"
#include "JsonObjectConverter.h"
#include "Misc/Paths.h"
#include "Qwack_ue_Sdk/Auth/QwackAuthSubsystem.h"
#include "Qwack_ue_Sdk/Cache/FlockEventSpool.h"
#include "Qwack_ue_Sdk/Config/QwackConfigSubsystem.h"
#include "Qwack_ue_Sdk/Config/QwackSettings.h"
#include "Qwack_ue_Sdk/Context/QwackContextSubsystem.h"
#include "Qwack_ue_Sdk/Endpoints/QwackGameEndpoints.h"
#include "Qwack_ue_Sdk/GameAPI/QwackFlockGameSubsystem.h"
#include "Qwack_ue_Sdk/GameAPI/QwackJsonHelpers.h"
#include "Qwack_ue_Sdk/HTTPClient/HTTPResponse.h"

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

	UQwackFlockGameSubsystem* GetGame(const UGameInstanceSubsystem* Self)
	{
		if (!Self) return nullptr;
		const UGameInstance* GI = Self->GetGameInstance();
		return GI ? GI->GetSubsystem<UQwackFlockGameSubsystem>() : nullptr;
	}
}

void UQwackAnalyticsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
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
				Root / TEXT("analytics_events"),
				S->AnalyticsMaxCachedEvents,
				S->AnalyticsCacheFlushBatchSize);
		}

		if (UQwackAuthSubsystem* Auth = GI->GetSubsystem<UQwackAuthSubsystem>())
		{
			Auth->OnAccessTokenChanged.AddDynamic(this, &UQwackAnalyticsSubsystem::HandleAccessTokenChanged);
		}
	}
}

void UQwackAnalyticsSubsystem::Deinitialize()
{
	if (const UGameInstance* GI = GetGameInstance())
	{
		if (UQwackAuthSubsystem* Auth = GI->GetSubsystem<UQwackAuthSubsystem>())
		{
			Auth->OnAccessTokenChanged.RemoveDynamic(this, &UQwackAnalyticsSubsystem::HandleAccessTokenChanged);
		}
	}
	Spool.Reset();
	Super::Deinitialize();
}

void UQwackAnalyticsSubsystem::HandleAccessTokenChanged(const FString& Token)
{
	if (!Token.IsEmpty())
	{
		FlushSpoolAsBatch();
	}
}

// =====================================================================
// Serialization
// =====================================================================

FString UQwackAnalyticsSubsystem::SerializeEvent(const FFlockAnalyticsEventRequest& Req) const
{
	const TSharedPtr<FJsonObject> Obj = QwackJson::StructToJsonObject(FFlockAnalyticsEventRequest::StaticStruct(), &Req);
	if (!Obj.IsValid()) return TEXT("{}");
	Obj->RemoveField(TEXT("PropertiesJson"));

	// Start from the caller's properties (if any), then merge SDK defaults. Caller keys
	// already present remain untouched — caller-supplied values always win.
	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	if (!Req.PropertiesJson.IsEmpty())
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Req.PropertiesJson);
		TSharedPtr<FJsonObject> UserProps;
		if (FJsonSerializer::Deserialize(Reader, UserProps) && UserProps.IsValid())
		{
			Props = UserProps.ToSharedRef();
		}
	}
	if (const UGameInstance* GI = GetGameInstance())
	{
		if (const UQwackContextSubsystem* Ctx = GI->GetSubsystem<UQwackContextSubsystem>())
		{
			Ctx->MergeDefaults(Props.ToSharedRef());
		}
	}
	Obj->SetObjectField(TEXT("properties"), Props);

	return QwackJson::JsonObjectToString(Obj.ToSharedRef());
}

FString UQwackAnalyticsSubsystem::BuildBatchPayload(const TArray<FString>& Bodies)
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
// Sessions
// =====================================================================

void UQwackAnalyticsSubsystem::StartSession(const FFlockSessionStartRequest& Request, const FFlockOnSessionStart& Callback)
{
	UQwackFlockGameSubsystem* Game = GetGame(this);
	if (!Game) { FFlockSessionStartResponse Out; Callback.ExecuteIfBound(Out); return; }

	FString Body;
	FJsonObjectConverter::UStructToJsonObjectString(Request, Body, 0, 0);

	TWeakObjectPtr<UQwackAnalyticsSubsystem> WeakThis(this);
	Game->Send(UQwackFlockGameEndpoints::StartSession, FString(), Body, /*bIncludeAuth*/ true,
		[WeakThis, Callback](const FQwackHTTPResponse& R)
		{
			FFlockSessionStartResponse Out;
			Out.Meta = UQwackFlockGameSubsystem::MakeMeta(R);
			if (Out.Meta.bSuccess)
			{
				TSharedPtr<FJsonObject> Obj;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(R.FullText);
				if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid())
				{
					Obj->TryGetStringField(TEXT("session_id"), Out.session_id);
				}
				// Cache for default-field injection on subsequent events.
				if (UQwackAnalyticsSubsystem* Strong = WeakThis.Get())
				{
					if (const UGameInstance* GI = Strong->GetGameInstance())
					{
						if (UQwackContextSubsystem* Ctx = GI->GetSubsystem<UQwackContextSubsystem>())
						{
							Ctx->SetSessionId(Out.session_id);
						}
					}
				}
			}
			Callback.ExecuteIfBound(Out);
		});
}

void UQwackAnalyticsSubsystem::EndSession(const FString& SessionId, const FFlockSessionEndRequest& Request, const FFlockOnGenericResponse& Callback)
{
	UQwackFlockGameSubsystem* Game = GetGame(this);
	if (!Game) { FFlockGenericResponse Out; Callback.ExecuteIfBound(Out); return; }

	const FString Path = UQwackFlockGameEndpoints::EndSession.EndPoint.Replace(TEXT("{session_id}"), *SessionId);
	const FString Url = Game->BuildUrl(Path);

	FString Body;
	FJsonObjectConverter::UStructToJsonObjectString(Request, Body, 0, 0);

	TWeakObjectPtr<UQwackAnalyticsSubsystem> WeakThis(this);
	Game->Send(UQwackFlockGameEndpoints::EndSession, Url, Body, /*bIncludeAuth*/ true,
		[WeakThis, Callback](const FQwackHTTPResponse& R)
		{
			FFlockGenericResponse Out;
			Out.Meta = UQwackFlockGameSubsystem::MakeMeta(R);
			if (Out.Meta.bSuccess)
			{
				if (UQwackAnalyticsSubsystem* Strong = WeakThis.Get())
				{
					if (const UGameInstance* GI = Strong->GetGameInstance())
					{
						if (UQwackContextSubsystem* Ctx = GI->GetSubsystem<UQwackContextSubsystem>())
						{
							Ctx->ClearSessionId();
						}
					}
				}
			}
			Callback.ExecuteIfBound(Out);
		});
}

// =====================================================================
// Events
// =====================================================================

void UQwackAnalyticsSubsystem::TrackEvent(const FFlockAnalyticsEventRequest& Request, const FFlockOnGenericResponse& Callback)
{
	UQwackFlockGameSubsystem* Game = GetGame(this);
	if (!Game) { FFlockGenericResponse Out; Callback.ExecuteIfBound(Out); return; }

	const FString Body = SerializeEvent(Request);

	// Write-ahead: persist before sending. On 2xx/permanent we remove the handle;
	// on transient failure the file sits on disk until the next flush trigger.
	TArray<FString> Handles;
	if (Spool)
	{
		Handles.Add(Spool->Enqueue(Body));
	}

	TWeakObjectPtr<UQwackAnalyticsSubsystem> WeakThis(this);
	Game->Send(UQwackFlockGameEndpoints::TrackEvent, FString(), Body, /*bIncludeAuth*/ true,
		[WeakThis, Handles, Callback](const FQwackHTTPResponse& R)
		{
			FFlockGenericResponse Out; Out.Meta = UQwackFlockGameSubsystem::MakeMeta(R);
			if (UQwackAnalyticsSubsystem* Strong = WeakThis.Get())
			{
				Strong->OnSpoolResponse(Handles, R);
			}
			Callback.ExecuteIfBound(Out);
		});
}

void UQwackAnalyticsSubsystem::TrackEvents(const FFlockAnalyticsEventsRequest& Request, const FFlockOnGenericResponse& Callback)
{
	UQwackFlockGameSubsystem* Game = GetGame(this);
	if (!Game) { FFlockGenericResponse Out; Callback.ExecuteIfBound(Out); return; }

	// Serialize once: feed both the spool entries and the batch payload from the same
	// JSON, instead of walking UStruct reflection twice.
	TArray<FString> Bodies;
	Bodies.Reserve(Request.events.Num());
	for (const FFlockAnalyticsEventRequest& E : Request.events)
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

	TWeakObjectPtr<UQwackAnalyticsSubsystem> WeakThis(this);
	Game->Send(UQwackFlockGameEndpoints::TrackEvents, FString(), Payload, /*bIncludeAuth*/ true,
		[WeakThis, Handles, Callback](const FQwackHTTPResponse& R)
		{
			FFlockGenericResponse Out; Out.Meta = UQwackFlockGameSubsystem::MakeMeta(R);
			if (UQwackAnalyticsSubsystem* Strong = WeakThis.Get())
			{
				Strong->OnSpoolResponse(Handles, R);
			}
			Callback.ExecuteIfBound(Out);
		});
}

void UQwackAnalyticsSubsystem::RecordTransaction(const FFlockTransactionRequest& Request, const FFlockOnGenericResponse& Callback)
{
	UQwackFlockGameSubsystem* Game = GetGame(this);
	if (!Game) { FFlockGenericResponse Out; Callback.ExecuteIfBound(Out); return; }

	FString Body;
	FJsonObjectConverter::UStructToJsonObjectString(Request, Body, 0, 0);
	Game->Send(UQwackFlockGameEndpoints::RecordTransaction, FString(), Body, /*bIncludeAuth*/ true,
		[Callback](const FQwackHTTPResponse& R)
		{
			FFlockGenericResponse Out; Out.Meta = UQwackFlockGameSubsystem::MakeMeta(R);
			Callback.ExecuteIfBound(Out);
		});
}

// =====================================================================
// Spool dispatch
// =====================================================================

void UQwackAnalyticsSubsystem::OnSpoolResponse(const TArray<FString>& Handles, const FQwackHTTPResponse& R)
{
	if (!Spool || Handles.Num() == 0) return;

	if (IsHttpSuccess(R.StatusCode) || IsHttpPermanent(R.StatusCode))
	{
		Spool->RemoveMany(Handles);
	}

	// Opportunistic drain: we just confirmed connectivity, so flush anything that piled
	// up while we were offline.
	if (IsHttpSuccess(R.StatusCode) && Spool->PendingCount() > 0)
	{
		FlushSpoolAsBatch();
	}
}

void UQwackAnalyticsSubsystem::FlushSpoolAsBatch()
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

	TWeakObjectPtr<UQwackAnalyticsSubsystem> WeakThis(this);
	Game->Send(UQwackFlockGameEndpoints::TrackEvents, FString(), Payload, /*bIncludeAuth*/ true,
		[WeakThis, Handles](const FQwackHTTPResponse& R)
		{
			UQwackAnalyticsSubsystem* Strong = WeakThis.Get();
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
