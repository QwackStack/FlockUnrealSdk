// Copyright 2022, Qwacks. All Rights Reserved.

#include "Providers/FlockAnalyticsProvider.h"

#include "Analytics/FlockAnalyticsJson.h"
#include "Analytics/FlockLogSink.h"
#include "Http/FlockEndpoints.h"
#include "Http/FlockJsonUtils.h"
#include "Misc/App.h"

namespace
{
	/** Keys of the app_termination payload — see the termination-detection contract. */
	const TCHAR* TerminationEventName = TEXT("app_termination");
	const TCHAR* KeyPreviousSessionId = TEXT("previous_session_id");
	const TCHAR* KeyClassification = TEXT("classification");
	const TCHAR* KeyLastAliveAt = TEXT("last_alive_at");
	const TCHAR* KeyExceptionCount = TEXT("unhandled_exception_count");
	const TCHAR* KeyAppVersion = TEXT("app_version");
	const TCHAR* KeySdkVersion = TEXT("sdk_version");

	FString PlatformDeviceType()
	{
#if PLATFORM_ANDROID || PLATFORM_IOS
		return TEXT("Mobile");
#elif PLATFORM_DESKTOP
		return TEXT("Desktop");
#else
		return TEXT("Console");
#endif
	}

	/**
	 * Core-only device facts. Screen metrics and the graphics device name are deliberately left unset:
	 * reading them means depending on ApplicationCore/RHI, which is not worth a module dependency for
	 * fields nothing consumes yet, and which would not be available under -nullrhi anyway.
	 */
	FFlockDeviceInfo CollectDeviceInfo(const FString& SdkVersion)
	{
		FFlockDeviceInfo Info;
		Info.Platform = FString(FPlatformProperties::IniPlatformName());
		Info.OperatingSystem = FPlatformMisc::GetOSVersion();
		Info.DeviceModel = FPlatformMisc::GetDeviceMakeAndModel();
		Info.DeviceType = PlatformDeviceType();
		Info.AppVersion = FApp::GetBuildVersion();
		Info.SystemLanguage = FPlatformMisc::GetDefaultLanguage();
		Info.SystemMemoryMb = static_cast<int32>(FPlatformMemory::GetPhysicalGBRam()) * 1024;
		Info.SdkVersion = SdkVersion;
		return Info;
	}
}

FFlockAnalyticsProvider::FFlockAnalyticsProvider(const TSharedRef<FFlockHttpClient>& InClient,
	const FFlockRetryPolicy& InPolicy, const TSharedRef<IFlockLogger>& InLogger,
	const TSharedRef<FFlockAuthSession>& InSession, const TWeakObjectPtr<UFlockEvents>& InEvents,
	const FString& InVersionedApiUrl, const FFlockAnalyticsConfig& InConfig,
	const FFlockAnalyticsDependencies& InDependencies, const FString& InGameVersion, const FString& InSdkVersion)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Config(InConfig)
	, Deps(InDependencies)
	, AuthSessionRef(InSession)
	, Events(InEvents)
	, VersionedApiUrl(InVersionedApiUrl)
	, GameVersion(InGameVersion)
	, SdkVersion(InSdkVersion)
{
	SetAuthSession(InSession);
	if (Deps.ConsentStore.IsValid())
	{
		bConsent = Deps.ConsentStore->ResolveEffective(Config.bRequireExplicitConsent);
	}
}

FFlockAnalyticsProvider::~FFlockAnalyticsProvider()
{
	// Not Shutdown(): ending a session here would dispatch HTTP from a destructor. The subsystem
	// calls Shutdown() while the provider is still alive; this only releases the local hooks.
	if (LogSink.IsValid())
	{
		LogSink->Stop();
	}
	if (Deps.Pump.IsValid())
	{
		Deps.Pump->Stop();
	}
}

FString FFlockAnalyticsProvider::AnalyticsUrl(const FString& Endpoint) const
{
	return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Endpoint);
}

bool FFlockAnalyticsProvider::IsCollecting() const
{
	return Config.bEnabled && bConsent;
}

bool FFlockAnalyticsProvider::HasConsent() const
{
	return bConsent;
}

int32 FFlockAnalyticsProvider::GetPendingEventCount() const
{
	return Deps.LogEventCache.IsValid() ? Deps.LogEventCache->PendingCount() : 0;
}

bool FFlockAnalyticsProvider::HasActiveSession() const
{
	return Deps.Session.IsValid() && Deps.Session->IsActive();
}

FString FFlockAnalyticsProvider::GetCurrentSessionId() const
{
	return Deps.Session.IsValid() ? Deps.Session->GetServerSessionId() : FString();
}

FFlockSessionSnapshot FFlockAnalyticsProvider::GetCurrentSnapshot() const
{
	return Deps.Session.IsValid() ? Deps.Session->TakeSnapshot() : FFlockSessionSnapshot();
}

void FFlockAnalyticsProvider::Initialize()
{
	if (bInitialized || !Config.bEnabled)
	{
		return;
	}
	bInitialized = true;

	// Before anything else writes a marker of its own.
	ReportSurvivingTermination();

	if (Deps.bEnableLogSink)
	{
		LogSink = MakeShared<FFlockLogSink>();
		const TWeakPtr<FFlockAnalyticsProvider> WeakSelf = AsShared();
		LogSink->OnFatal.AddLambda([WeakSelf](const FFlockCapturedLog& Captured)
		{
			// Crash path: spool to disk only. There is no time for a round trip.
			const TSharedPtr<FFlockAnalyticsProvider> Self = WeakSelf.Pin();
			if (Self.IsValid())
			{
				Self->LogException(Captured.Message, Captured.StackTrace);
			}
		});
		LogSink->Start();
	}

	if (Deps.Pump.IsValid())
	{
		const TWeakPtr<FFlockAnalyticsProvider> WeakSelf = AsShared();
		Deps.Pump->OnTick.AddLambda([WeakSelf](float Delta)
		{
			const TSharedPtr<FFlockAnalyticsProvider> Self = WeakSelf.Pin();
			if (Self.IsValid())
			{
				Self->HandleTick(Delta);
			}
		});
		Deps.Pump->OnBackgroundChanged.AddLambda([WeakSelf](bool bBackgrounded)
		{
			const TSharedPtr<FFlockAnalyticsProvider> Self = WeakSelf.Pin();
			if (Self.IsValid())
			{
				Self->HandleBackgroundChanged(bBackgrounded);
			}
		});
		Deps.Pump->OnQuit.AddLambda([WeakSelf]()
		{
			const TSharedPtr<FFlockAnalyticsProvider> Self = WeakSelf.Pin();
			if (Self.IsValid())
			{
				Self->HandleQuit();
			}
		});
		Deps.Pump->Start();
	}
}

void FFlockAnalyticsProvider::Shutdown()
{
	if (!bInitialized)
	{
		return;
	}
	bInitialized = false;

	if (Config.bAutoEndSessionOnQuit && HasActiveSession())
	{
		EndSession(EFlockSessionEndReason::Quit);
	}

	if (LogSink.IsValid())
	{
		LogSink->Stop();
		LogSink.Reset();
	}
	if (Deps.Pump.IsValid())
	{
		Deps.Pump->Stop();
	}
	// A clean shutdown removes the tombstone, so the next launch reports no dirty exit.
	if (Deps.TerminationTracker.IsValid())
	{
		Deps.TerminationTracker->StopTracking();
	}
}

void FFlockAnalyticsProvider::ReportSurvivingTermination()
{
	if (!Deps.TerminationTracker.IsValid())
	{
		return;
	}

	FFlockTerminationMarker Survivor;
	if (!Deps.TerminationTracker->ReadSurvivingMarker(Survivor))
	{
		return;
	}

	// Always drop the marker, even when we cannot report it — otherwise a dirty exit found while
	// consent is off would be re-reported on every single launch from now on.
	Deps.TerminationTracker->ClearMarker();

	if (!IsCollecting())
	{
		return;
	}

	// Reported as `debug`, not `exception`: it is a record *about* a previous crash, and counting it
	// as an exception would inflate the backend's exception statistics with non-exception entries.
	FFlockLogEventRequest Event = MakeLogEvent(EFlockLogEventType::Debug, TerminationEventName);
	Event.Data.ExtraData.Add(KeyPreviousSessionId,
		Survivor.ServerSessionId.IsEmpty() ? Survivor.SessionId : Survivor.ServerSessionId);
	Event.Data.ExtraData.Add(KeyClassification, FFlockTerminationTracker::Classify(Survivor));
	Event.Data.ExtraData.Add(KeyLastAliveAt, Survivor.LastAliveUtc.ToIso8601());
	Event.Data.ExtraData.Add(KeyExceptionCount, FString::FromInt(Survivor.ExceptionCount));
	Event.Data.ExtraData.Add(KeyAppVersion, Survivor.AppVersion);
	Event.Data.ExtraData.Add(KeySdkVersion, Survivor.SdkVersion);
	QueueLogEvent(MoveTemp(Event));
}

void FFlockAnalyticsProvider::SetConsent(bool bGranted)
{
	if (bConsent == bGranted)
	{
		return;
	}
	bConsent = bGranted;

	if (Deps.ConsentStore.IsValid())
	{
		Deps.ConsentStore->Save(bGranted);
	}

	if (!bGranted)
	{
		// Opting out ends the session and drops what was queued: a player who withdraws consent
		// should not leave collected data sitting on disk waiting to be sent.
		if (HasActiveSession())
		{
			EndSession(EFlockSessionEndReason::Manual);
		}
		if (Deps.LogEventCache.IsValid())
		{
			Deps.LogEventCache->Clear();
		}
		if (Deps.TerminationTracker.IsValid())
		{
			Deps.TerminationTracker->StopTracking();
		}
	}
	else if (Config.bAutoStartSession && !KnownPlayerId.IsEmpty() && !HasActiveSession())
	{
		// The opt-in flow: the player signed in while collection was gated, so no session could open
		// then. This is the moment it can.
		StartSession(KnownPlayerId);
	}

	if (UFlockEvents* EventsPtr = Events.Get())
	{
		EventsPtr->InvokeConsentChanged(bGranted);
	}
}

FFlockLogEventRequest FFlockAnalyticsProvider::MakeLogEvent(EFlockLogEventType Type, const FString& Message) const
{
	FFlockLogEventRequest Event;
	Event.Message = Message;
	Event.Timestamp = FDateTime::UtcNow().ToIso8601();
	Event.Data.Type = Type;
	Event.Data.GameVersion = GameVersion;
	return Event;
}

void FFlockAnalyticsProvider::QueueLogEvent(FFlockLogEventRequest&& Event)
{
	if (!IsCollecting())
	{
		return;
	}

	const FString Payload = FFlockAnalyticsJson::SerializeEvent(Event);

	if (Deps.LogEventCache.IsValid())
	{
		Deps.LogEventCache->Enqueue(Payload);
		return;
	}

	// No spool configured: deliver straight away and accept that a failure loses the entry.
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = AuthSessionRef;
	const FString Url = AnalyticsUrl(FlockEndpoints::LogEventSingle);
	Execute<FFlockAnalyticsAck>(
		[ClientRef, SessionRef, Url, Payload](TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnAttempt)
		{
			return ClientRef->PostJsonRaw<FFlockAnalyticsAck>(Url, SessionRef->GetAuthHeaders(), Payload, MoveTemp(OnAttempt));
		},
		nullptr, TEXT("Log event"));
}

void FFlockAnalyticsProvider::LogEvent(const FString& Message, const TMap<FString, FString>& ExtraData)
{
	FFlockLogEventRequest Event = MakeLogEvent(EFlockLogEventType::Debug, Message);
	Event.Data.ExtraData = ExtraData;
	QueueLogEvent(MoveTemp(Event));
}

void FFlockAnalyticsProvider::LogError(const FString& Message, const FString& LogicalExpression,
	const FString& ErrorCode, const TMap<FString, FString>& ErrorData, const TMap<FString, FString>& ExtraData)
{
	FFlockLogEventRequest Event = MakeLogEvent(EFlockLogEventType::LogicError, Message);
	Event.Data.LogicalExpression = LogicalExpression;
	Event.Data.ErrorCode = ErrorCode;
	Event.Data.ErrorMessage = Message;
	Event.Data.ErrorData = ErrorData;
	Event.Data.ExtraData = ExtraData;
	QueueLogEvent(MoveTemp(Event));
}

void FFlockAnalyticsProvider::LogException(const FString& Message, const FString& StackTrace,
	const TMap<FString, FString>& ErrorData, const TMap<FString, FString>& ExtraData)
{
	FFlockLogEventRequest Event = MakeLogEvent(EFlockLogEventType::Exception, Message);
	Event.Data.ErrorMessage = Message;
	Event.Data.ErrorTraceback = StackTrace;
	if (!StackTrace.IsEmpty())
	{
		StackTrace.ParseIntoArrayLines(Event.Data.ErrorTracebackLines);
	}
	Event.Data.ErrorData = ErrorData;
	Event.Data.ExtraData = ExtraData;
	QueueLogEvent(MoveTemp(Event));

	// Exception pressure is context for the next launch's termination report.
	if (Deps.TerminationTracker.IsValid())
	{
		Deps.TerminationTracker->NoteException();
	}
}

void FFlockAnalyticsProvider::RecordScreenView(const FString& ScreenName)
{
	if (!IsCollecting() || !Deps.Session.IsValid())
	{
		return;
	}
	Deps.Session->RecordScreenView(ScreenName);
}

void FFlockAnalyticsProvider::StartSession(const FString& InPlayerId,
	TFunction<void(TFlockResult<FString>)> OnComplete)
{
	// Remembered before the consent gate, so a sign-in that happens while gated still leaves us able
	// to open the session the moment consent arrives.
	if (!InPlayerId.IsEmpty())
	{
		KnownPlayerId = InPlayerId;
	}

	if (!IsCollecting() || !Deps.Session.IsValid())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FString>::Fail(FFlockError::Make(EFlockErrorType::Validation,
				TEXT("Analytics is disabled or consent has not been granted"))));
		}
		return;
	}
	if (!RequireNotEmpty<FString>(InPlayerId, TEXT("PlayerId"), OnComplete))
	{
		return;
	}
	if (Deps.Session->IsActive())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FString>::Ok(Deps.Session->GetServerSessionId()));
		}
		return;
	}

	const FString LocalId = Deps.Session->Start(InPlayerId);

	// Start tombstoning immediately: a crash before the server replies still counts as a dirty exit.
	if (Deps.TerminationTracker.IsValid())
	{
		FFlockTerminationMarker Seed;
		Seed.SessionId = LocalId;
		Seed.PlayerId = InPlayerId;
		Seed.AppVersion = FApp::GetBuildVersion();
		Seed.SdkVersion = SdkVersion;
		Deps.TerminationTracker->BeginTracking(Seed);
	}

	if (UFlockEvents* EventsPtr = Events.Get())
	{
		EventsPtr->InvokeSessionStarted(LocalId);
	}

	FFlockSessionStartRequest Request;
	Request.PlayerId = InPlayerId;
	const FFlockDeviceInfo Device = CollectDeviceInfo(SdkVersion);
	Request.Platform = Device.Platform;
	Request.DeviceType = Device.DeviceType;
	Request.GameVersionId = GameVersion;
	Request.StartedAt = FDateTime::UtcNow().ToIso8601();

	FString Body;
	FFlockJsonUtils::StructToWireJson(Request, Body, /*bOmitEmptyStrings*/ true);

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = AuthSessionRef;
	const FString Url = AnalyticsUrl(FlockEndpoints::AnalyticsSessions);
	const TWeakPtr<FFlockAnalyticsProvider> WeakSelf = AsShared();

	Execute<FFlockSessionStartResponse>(
		[ClientRef, SessionRef, Url, Body](TFunction<void(TFlockResult<FFlockSessionStartResponse>)> OnAttempt)
		{
			return ClientRef->PostJsonRaw<FFlockSessionStartResponse>(Url, SessionRef->GetAuthHeaders(), Body, MoveTemp(OnAttempt));
		},
		[WeakSelf, OnComplete](TFlockResult<FFlockSessionStartResponse> Result)
		{
			const TSharedPtr<FFlockAnalyticsProvider> Self = WeakSelf.Pin();
			if (Self.IsValid() && Result.bSuccess)
			{
				// The local session keeps running either way; the server id only affects delivery.
				if (Self->Deps.Session.IsValid())
				{
					Self->Deps.Session->SetServerSessionId(Result.Value.SessionId);
				}
				if (Self->Deps.TerminationTracker.IsValid())
				{
					Self->Deps.TerminationTracker->SetServerSessionId(Result.Value.SessionId);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result.bSuccess
					? TFlockResult<FString>::Ok(Result.Value.SessionId)
					: TFlockResult<FString>::Fail(Result.Error));
			}
		},
		TEXT("Start session"));
}

void FFlockAnalyticsProvider::EndSession(EFlockSessionEndReason Reason,
	TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete)
{
	if (!Deps.Session.IsValid() || !Deps.Session->IsActive())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockAnalyticsAck>::Ok(FFlockAnalyticsAck()));
		}
		return;
	}

	Deps.Session->End();
	const FFlockSessionSnapshot Snapshot = Deps.Session->TakeSnapshot();
	const FString ServerId = Snapshot.ServerSessionId;

	if (UFlockEvents* EventsPtr = Events.Get())
	{
		FFlockSessionEndedArgs Args;
		Args.Snapshot = Snapshot;
		Args.Reason = Reason;
		EventsPtr->InvokeSessionEnded(Args);
	}

	// A session that never got a server id has nothing to close out remotely.
	if (ServerId.IsEmpty())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockAnalyticsAck>::Ok(FFlockAnalyticsAck()));
		}
		return;
	}

	FFlockSessionEndRequest Request;
	Request.DurationSeconds = FMath::RoundToInt(Snapshot.DurationSeconds);
	Request.ScreensViewed = Snapshot.ScreensViewed;
	Request.IsBounce = Snapshot.IsBounce;
	Request.EndedAt = Snapshot.EndTimeUtc;

	FString Body;
	FFlockJsonUtils::StructToWireJson(Request, Body, /*bOmitEmptyStrings*/ true);

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = AuthSessionRef;
	const FString Url = AnalyticsUrl(FlockEndpoints::AnalyticsSessionById(ServerId));

	Execute<FFlockAnalyticsAck>(
		[ClientRef, SessionRef, Url, Body](TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnAttempt)
		{
			return ClientRef->PatchJsonRaw<FFlockAnalyticsAck>(Url, SessionRef->GetAuthHeaders(), Body, MoveTemp(OnAttempt));
		},
		MoveTemp(OnComplete), TEXT("End session"));
}

void FFlockAnalyticsProvider::Flush(TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete)
{
	if (!Deps.LogEventCache.IsValid() || Deps.LogEventCache->PendingCount() == 0)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockAnalyticsAck>::Ok(FFlockAnalyticsAck()));
		}
		return;
	}
	if (bFlushInFlight)
	{
		// Overlapping flushes would send the same batch twice; the in-flight one will drain it.
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockAnalyticsAck>::Ok(FFlockAnalyticsAck()));
		}
		return;
	}

	bFlushInFlight = true;
	SendNextBatch(MoveTemp(OnComplete));
}

void FFlockAnalyticsProvider::SendNextBatch(TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete)
{
	TArray<FString> Handles;
	TArray<FString> Payloads;
	Deps.LogEventCache->PeekBatch(FMath::Max(Config.CacheFlushBatchSize, 1), Handles, Payloads);

	if (Handles.Num() == 0)
	{
		bFlushInFlight = false;
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockAnalyticsAck>::Ok(FFlockAnalyticsAck()));
		}
		return;
	}

	TArray<FFlockLogEventRequest> Batch;
	TArray<FString> Undeliverable;
	for (int32 Index = 0; Index < Handles.Num(); ++Index)
	{
		FFlockLogEventRequest Event;
		if (FFlockAnalyticsJson::DeserializeEvent(Payloads[Index], Event))
		{
			Batch.Add(MoveTemp(Event));
		}
		else
		{
			// A spool entry we can no longer parse will never become deliverable; drop it rather
			// than wedging the queue behind it forever.
			Undeliverable.Add(Handles[Index]);
		}
	}
	for (const FString& Handle : Undeliverable)
	{
		Deps.LogEventCache->Remove(Handle);
	}
	if (Batch.Num() == 0)
	{
		SendNextBatch(MoveTemp(OnComplete));
		return;
	}

	const FString Body = FFlockAnalyticsJson::SerializeEvents(Batch);
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = AuthSessionRef;
	const FString Url = AnalyticsUrl(FlockEndpoints::LogEvent);
	const TWeakPtr<FFlockAnalyticsProvider> WeakSelf = AsShared();

	// Handles the batch carried, so only what was actually sent is acknowledged.
	TArray<FString> SentHandles = Handles;
	for (const FString& Handle : Undeliverable)
	{
		SentHandles.Remove(Handle);
	}

	Execute<FFlockAnalyticsAck>(
		[ClientRef, SessionRef, Url, Body](TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnAttempt)
		{
			return ClientRef->PostJsonRaw<FFlockAnalyticsAck>(Url, SessionRef->GetAuthHeaders(), Body, MoveTemp(OnAttempt));
		},
		[WeakSelf, SentHandles, OnComplete](TFlockResult<FFlockAnalyticsAck> Result)
		{
			const TSharedPtr<FFlockAnalyticsProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			if (!Result.bSuccess)
			{
				// Leave the batch spooled; the next flush retries it.
				Self->bFlushInFlight = false;
				if (OnComplete)
				{
					OnComplete(Result);
				}
				return;
			}

			for (const FString& Handle : SentHandles)
			{
				Self->Deps.LogEventCache->Remove(Handle);
			}
			Self->SendNextBatch(OnComplete);
		},
		TEXT("Flush log events"));
}

void FFlockAnalyticsProvider::EraseLocalData()
{
	if (Deps.LogEventCache.IsValid())
	{
		Deps.LogEventCache->Clear();
	}
	if (Deps.TerminationTracker.IsValid())
	{
		Deps.TerminationTracker->StopTracking();
	}
	if (Deps.ConsentStore.IsValid())
	{
		Deps.ConsentStore->Clear();
	}
}

void FFlockAnalyticsProvider::DrainLogSink()
{
	if (!LogSink.IsValid())
	{
		return;
	}
	FFlockCapturedLog Captured;
	while (LogSink->Dequeue(Captured))
	{
		TMap<FString, FString> ExtraData;
		ExtraData.Add(TEXT("category"), Captured.Category.ToString());
		LogException(Captured.Message, Captured.StackTrace, TMap<FString, FString>(), ExtraData);
	}
}

void FFlockAnalyticsProvider::HandleTick(float DeltaSeconds)
{
	if (!IsCollecting())
	{
		return;
	}

	if (Deps.Session.IsValid())
	{
		Deps.Session->Tick(DeltaSeconds);
	}

	DrainLogSink();

	// Heartbeat is local: it refreshes the death-time estimate on disk. No round trip.
	if (Config.HeartbeatIntervalSeconds > 0.f)
	{
		HeartbeatAccumulator += DeltaSeconds;
		if (HeartbeatAccumulator >= Config.HeartbeatIntervalSeconds)
		{
			HeartbeatAccumulator = 0.f;
			if (Deps.Session.IsValid())
			{
				Deps.Session->MarkHeartbeat();
			}
			if (Deps.TerminationTracker.IsValid())
			{
				Deps.TerminationTracker->HandleHeartbeat();
			}
		}
	}

	if (Config.EventBufferFlushIntervalSeconds > 0.f)
	{
		FlushAccumulator += DeltaSeconds;
		if (FlushAccumulator >= Config.EventBufferFlushIntervalSeconds)
		{
			FlushAccumulator = 0.f;
			Flush();
		}
	}
}

void FFlockAnalyticsProvider::HandleBackgroundChanged(bool bBackgrounded)
{
	if (!IsCollecting())
	{
		return;
	}

	if (Deps.TerminationTracker.IsValid())
	{
		Deps.TerminationTracker->SetBackgrounded(bBackgrounded);
	}

	if (bBackgrounded)
	{
		if (Deps.Session.IsValid())
		{
			Deps.Session->Pause();
		}
		if (UFlockEvents* EventsPtr = Events.Get())
		{
			EventsPtr->InvokeSessionPaused();
		}
		// Last chance to get anything queued off the device before the OS may evict us.
		Flush();
		return;
	}

	if (!Deps.Session.IsValid())
	{
		return;
	}

	const FString PlayerId = Deps.Session->GetPlayerId();
	if (Deps.Session->Resume())
	{
		// Away past the timeout: this is a new play session, not a continuation.
		EndSession(EFlockSessionEndReason::Timeout);
		StartSession(PlayerId);
		return;
	}

	if (UFlockEvents* EventsPtr = Events.Get())
	{
		EventsPtr->InvokeSessionResumed();
	}
}

void FFlockAnalyticsProvider::HandleQuit()
{
	if (Config.bAutoEndSessionOnQuit && HasActiveSession())
	{
		EndSession(EFlockSessionEndReason::Quit);
	}
	// Whatever is still spooled stays on disk and goes out on the next launch.
	if (Deps.TerminationTracker.IsValid())
	{
		Deps.TerminationTracker->StopTracking();
	}
}

void FFlockAnalyticsProvider::TickForTesting(float DeltaSeconds)
{
	HandleTick(DeltaSeconds);
}
