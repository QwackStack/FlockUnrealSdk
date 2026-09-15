// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Providers/FlockAnalyticsProvider.h"

#include "Analytics/FlockAnalyticsJson.h"
#include "Analytics/FlockLogSink.h"
#include "Analytics/FlockStackTrace.h"
#include "Async/Async.h"
#include "HAL/PlatformTime.h"
#include "Http/FlockEndpoints.h"
#include "Http/FlockJsonUtils.h"
#include "Misc/App.h"
#include "Misc/CoreMisc.h"
#include "Misc/FileHelper.h"

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

	/** The diagnostics entry a build sends when it cannot see every class of fault. */
	const TCHAR* CoverageNoticeName = TEXT("exception_capture_limited");

	const TCHAR* BoolWire(bool bValue)
	{
		return bValue ? TEXT("true") : TEXT("false");
	}

	/** What every automatically captured fault carries, on either path, so a dashboard can tell where it came from. */
	FFlockLogDetails DetailsOfCapturedFault(const FFlockCapturedLog& Captured)
	{
		FFlockLogDetails Details;
		Details.ExtraData.Add(TEXT("category"), Captured.Category.ToString());
		if (!Captured.Source.IsEmpty())
		{
			Details.ExtraData.Add(TEXT("exception_source"), Captured.Source);
		}
		if (!Captured.ScriptExceptionType.IsEmpty())
		{
			Details.ExtraData.Add(TEXT("blueprint_exception_type"), Captured.ScriptExceptionType);
		}
		return Details;
	}

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
	 *
	 * None of it changes while the process lives and several reads are real OS queries, so it is
	 * gathered once. That used to be per session start; the heartbeat's registration retry and the
	 * spool drain both reach it now, which would otherwise mean an OS-version and locale lookup every
	 * minute for a run that cannot register.
	 */
	const FFlockDeviceInfo& CollectDeviceInfo(const FString& SdkVersion)
	{
		static const FFlockDeviceInfo Info = [&SdkVersion]()
		{
			FFlockDeviceInfo Collected;
			Collected.Platform = FString(FPlatformProperties::IniPlatformName());
			Collected.OperatingSystem = FPlatformMisc::GetOSVersion();
			Collected.DeviceModel = FPlatformMisc::GetDeviceMakeAndModel();
			Collected.DeviceType = PlatformDeviceType();
			Collected.AppVersion = FApp::GetBuildVersion();
			Collected.SystemLanguage = FPlatformMisc::GetDefaultLanguage();
			Collected.SystemMemoryMb = static_cast<int32>(FPlatformMemory::GetPhysicalGBRam()) * 1024;
			Collected.SdkVersion = SdkVersion;
			return Collected;
		}();
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
	, RepeatedExceptions(InConfig.ExceptionRepeatWindowSeconds)
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

int32 FFlockAnalyticsProvider::GetPendingAnalyticsEventCount() const
{
	return Deps.AnalyticsEventCache.IsValid() ? Deps.AnalyticsEventCache->PendingCount() : 0;
}

double FFlockAnalyticsProvider::NowSeconds() const
{
	return Clock ? Clock() : FPlatformTime::Seconds();
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

	if (!Config.SessionPlatform.IsEmpty() && !Config.HasUsableSessionPlatform())
	{
		// Quoted, so the stray space shows.
		Logger->LogWarning(FString::Printf(TEXT("Session Platform '%s' starts or ends with a space or line break, so sessions send the engine's platform name instead. Fix it in Project Settings > Plugins > Flock SDK Settings."),
			*Config.SessionPlatform));
	}

	// Before anything else writes a marker of its own.
	ReportSurvivingTermination();

	// Reads a different file than the tombstone and writes neither of the other's, so the order
	// between them is free.
	RecoverOrphanedSession();

	// Two switches with two owners: Deps.bEnableLogSink says whether this process may tap GLog at all (off inside
	// the automation runner), Config.bCaptureExceptions is the project's own choice.
	if (Deps.bEnableLogSink && Config.bCaptureExceptions)
	{
		LogSink = MakeShared<FFlockLogSink>();
		for (const FString& Category : Config.ExcludedExceptionCategories)
		{
			if (!Category.IsEmpty())
			{
				LogSink->AddExcludedCategory(FName(*Category));
			}
		}
		const TWeakPtr<FFlockAnalyticsProvider> WeakSelf = AsShared();
		LogSink->OnFatal.AddLambda([WeakSelf](const FFlockCapturedLog& Captured)
		{
			// Crash path: spool to disk only. There is no time for a round trip.
			const TSharedPtr<FFlockAnalyticsProvider> Self = WeakSelf.Pin();
			if (Self.IsValid())
			{
				Self->LogException(Captured.Message, Captured.StackTrace, DetailsOfCapturedFault(Captured));
			}
		});
		LogSink->Start();
	}

	ReportCaptureCoverage();

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

	// Faults captured since the last tick, and repeats still inside an open window: no tick will come for either,
	// so they are spooled now or never.
	DrainLogSink();
	QueueRepeatReports(RepeatedExceptions.CollectAll());

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
		// Opting out discards the session and drops what was queued: a player who withdraws consent
		// should not leave collected data sitting on disk waiting to be sent — and the session they
		// were in must not be reported either, which is why this is a discard and not an end.
		DiscardSession();
		if (Deps.LogEventCache.IsValid())
		{
			Deps.LogEventCache->Clear();
		}
		if (Deps.SessionEndCache.IsValid())
		{
			Deps.SessionEndCache->Clear();
		}
		if (Deps.AnalyticsEventCache.IsValid())
		{
			Deps.AnalyticsEventCache->Clear();
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

	// A spool that stores nothing answers with an empty handle — which is exactly what switching caching off
	// builds (a cap of 0). Treating that as spooled lost every entry with caching off, so an entry the spool did
	// not take is delivered straight away instead.
	if (Deps.LogEventCache.IsValid() && !Deps.LogEventCache->Enqueue(Payload).IsEmpty())
	{
		return;
	}

	// Not spooled: deliver straight away and accept that a failure loses the entry.
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

void FFlockAnalyticsProvider::LogError(const FString& Message, const FFlockLogDetails& Details)
{
	FFlockLogEventRequest Event = MakeLogEvent(EFlockLogEventType::LogicError, Message);
	Event.Data.LogicalExpression = Details.LogicalExpression;
	Event.Data.ErrorCode = Details.ErrorCode;
	Event.Data.ErrorMessage = Message;
	Event.Data.ErrorData = Details.ErrorData;
	Event.Data.ExtraData = Details.ExtraData;
	QueueLogEvent(MoveTemp(Event));
}

void FFlockAnalyticsProvider::LogException(const FString& Message, const FString& StackTrace,
	const FFlockLogDetails& Details)
{
	// No trace supplied means walk one now. 3 frames drop the two inside the walker plus this
	// function, so the first frame is the caller — verified against a printed trace, because an
	// off-by-one here is invisible except by reading one. The walk stays in this function for that
	// reason: moved one call deeper, the first frame would be LogException itself. The automatic sink
	// path always supplies its own, captured where the error was logged, so it never re-walks here.
	SpoolException(Message, StackTrace.IsEmpty() ? FFlockStackTrace::Capture(/*FramesToSkip*/ 3) : StackTrace, Details);

	// Exception pressure is context for the next launch's termination report.
	if (Deps.TerminationTracker.IsValid())
	{
		Deps.TerminationTracker->NoteException();
	}
}

void FFlockAnalyticsProvider::SpoolException(const FString& Message, const FString& StackTrace,
	const FFlockLogDetails& Details)
{
	FFlockLogEventRequest Event = MakeLogEvent(EFlockLogEventType::Exception, Message);
	Event.Data.ErrorMessage = Message;
	Event.Data.ErrorTraceback = StackTrace;
	if (!Event.Data.ErrorTraceback.IsEmpty())
	{
		Event.Data.ErrorTraceback.ParseIntoArrayLines(Event.Data.ErrorTracebackLines);
	}
	Event.Data.ErrorData = Details.ErrorData;
	Event.Data.ExtraData = Details.ExtraData;
	QueueLogEvent(MoveTemp(Event));
}

void FFlockAnalyticsProvider::RecordScreenView(const FString& ScreenName)
{
	if (!IsCollecting() || !Deps.Session.IsValid())
	{
		return;
	}
	Deps.Session->RecordScreenView(ScreenName);
}

bool FFlockAnalyticsProvider::IsEventTooLongToStore(const FString& EventName, const FString& EventCategory)
{
	return EventName.Len() > MaxEventNameLength || EventCategory.Len() > MaxEventCategoryLength;
}

bool FFlockAnalyticsProvider::TrackEvent(const FString& EventName, const FFlockCommandData& Properties,
	const FString& EventCategory)
{
	// The provider is game-thread only. A game recording an event from a worker has done nothing wrong, so the
	// call is forwarded rather than refused — which means it cannot be judged here, and answers true.
	if (!IsInGameThread())
	{
		const TWeakPtr<FFlockAnalyticsProvider> WeakSelf = AsShared();
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, EventName, Properties, EventCategory]()
		{
			if (const TSharedPtr<FFlockAnalyticsProvider> Self = WeakSelf.Pin())
			{
				Self->TrackEvent(EventName, Properties, EventCategory);
			}
		});
		return true;
	}

	if (!IsCollecting())
	{
		return false;
	}
	// The server stores an empty name as a row of its own, so the refusal has to happen here.
	if (EventName.TrimStartAndEnd().IsEmpty())
	{
		Logger->LogWarning(TEXT("Track event refused: an event needs a name."));
		return false;
	}
	// The server cannot store a longer name or category, and fails the whole request for it: every event sent alongside
	// would be held back and sent again until its attempts ran out.
	if (IsEventTooLongToStore(EventName, EventCategory))
	{
		Logger->LogWarning(FString::Printf(
			TEXT("Track event refused: its name has %d characters and its category %d, and the server stores at most %d and %d. Name: '%s'."),
			EventName.Len(), EventCategory.Len(), MaxEventNameLength, MaxEventCategoryLength, *EventName.Left(MaxEventNameLength)));
		return false;
	}
	// The server writes session_started itself when a session starts, and accepts the name from a client without
	// complaint — so recording it here would count every session twice, silently.
	if (EventName.Equals(ReservedSessionStartedEvent, ESearchCase::CaseSensitive))
	{
		Logger->LogWarning(FString::Printf(
			TEXT("Track event refused: '%s' is reserved; the server records it when a session starts."),
			ReservedSessionStartedEvent));
		return false;
	}

	FFlockSpooledAnalyticsEvent Entry;
	Entry.Event.PlayerId = AuthSessionRef->IsAuthenticated() ? AuthSessionRef->GetPlayerId() : FString();
	Entry.Event.EventName = EventName;
	Entry.Event.EventCategory = EventCategory;
	Entry.Event.Timestamp = FDateTime::UtcNow().ToIso8601();
	Entry.Event.Properties = Properties;

	// Only the session of this event's own player is attached. Its server id may not be known yet, so the local id
	// is kept alongside: the server id can still be attached when the event is sent.
	// A server id the server has already refused a batch for is not attached either: every event carrying it would
	// cost a refusal and a resend.
	if (HasActiveSession() && !Entry.Event.PlayerId.IsEmpty() && Deps.Session->GetPlayerId() == Entry.Event.PlayerId
		&& !RefusedSessionIds.Contains(Deps.Session->GetServerSessionId()))
	{
		Entry.LocalSessionId = Deps.Session->GetSessionId();
		Entry.Event.SessionId = Deps.Session->GetServerSessionId();
	}

	if (Deps.AnalyticsEventCache.IsValid()
		&& !Deps.AnalyticsEventCache->Enqueue(FFlockAnalyticsJson::SerializeSpooledAnalyticsEvent(Entry)).IsEmpty())
	{
		return true;
	}

	// Not spooled — caching is off, or the disk refused it. Without a spool an event with no player cannot be held,
	// and sent it would only be refused.
	if (Entry.Event.PlayerId.IsEmpty())
	{
		Logger->LogDebug(FString::Printf(
			TEXT("Event '%s' dropped: nobody is signed in and there is no spool to hold it."), *EventName));
		return false;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = AuthSessionRef;
	const FString Url = AnalyticsUrl(FlockEndpoints::AnalyticsEvents);
	const FString Body = FFlockAnalyticsJson::SerializeAnalyticsEvents({ Entry.Event });
	Execute<FFlockAnalyticsAck>(
		[ClientRef, SessionRef, Url, Body](TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnAttempt)
		{
			return ClientRef->PostJsonRaw<FFlockAnalyticsAck>(Url, SessionRef->GetAuthHeaders(), Body, MoveTemp(OnAttempt));
		},
		nullptr, TEXT("Track event"), /*bIdempotent*/ false);
	return true;
}

void FFlockAnalyticsProvider::HandleAuthenticated(const FString& PlayerId)
{
	if (PlayerId.IsEmpty() || !Deps.AnalyticsEventCache.IsValid())
	{
		return;
	}

	// Events recorded while nobody was signed in belong to whoever signs in next. An event that already carries a
	// player keeps it: reassigning it would credit one player's play to another.
	TArray<FString> Handles;
	TArray<FString> Payloads;
	Deps.AnalyticsEventCache->PeekBatch(Deps.AnalyticsEventCache->PendingCount(), Handles, Payloads);

	int32 Attributed = 0;
	for (int32 Index = 0; Index < Handles.Num(); ++Index)
	{
		FFlockSpooledAnalyticsEvent Entry;
		if (!FFlockAnalyticsJson::DeserializeSpooledAnalyticsEvent(Payloads[Index], Entry) || !Entry.Event.PlayerId.IsEmpty())
		{
			continue;
		}
		Entry.Event.PlayerId = PlayerId;
		Deps.AnalyticsEventCache->Replace(Handles[Index], FFlockAnalyticsJson::SerializeSpooledAnalyticsEvent(Entry));
		++Attributed;
	}

	if (Attributed > 0)
	{
		Logger->LogDebug(FString::Printf(TEXT("Attributed %d event(s) recorded before sign-in to %s"),
			Attributed, *PlayerId));
	}
	// Whether or not anything was attributed: nothing is delivered while nobody is signed in, so a previous player's
	// events have been waiting for this too.
	FlushAnalyticsEvents(nullptr);
}

FFlockExceptionCaptureCoverage FFlockAnalyticsProvider::ComputeCoverage(bool bCaptureEnabled, bool bLoggingCompiledIn,
	bool bEnsuresCompiledIn, bool bBlueprintGuardCompiledIn, const FString& BuildConfiguration)
{
	FFlockExceptionCaptureCoverage Coverage;
	Coverage.bEnabled = bCaptureEnabled;
	Coverage.bLogErrors = bCaptureEnabled && bLoggingCompiledIn;
	// An ensure reaches the SDK only as an Error line, so it needs logging compiled in as well as ensures.
	Coverage.bEnsures = bCaptureEnabled && bLoggingCompiledIn && bEnsuresCompiledIn;
	// The engine broadcasts script exceptions in every configuration; only its loop and recursion detection is
	// compiled out of Shipping and Test.
	Coverage.bBlueprintExceptions = bCaptureEnabled;
	Coverage.bBlueprintInfiniteLoops = bCaptureEnabled && bBlueprintGuardCompiledIn;
	Coverage.bCrashes = bCaptureEnabled;
	Coverage.BuildConfiguration = BuildConfiguration;
	return Coverage;
}

FFlockExceptionCaptureCoverage FFlockAnalyticsProvider::GetExceptionCaptureCoverage() const
{
	if (CoverageOverride.IsSet())
	{
		return CoverageOverride.GetValue();
	}
	// A running sink, not the setting: capture is also off when analytics is, or when this process may not tap the log
	// at all, and coverage must not claim faults nothing is listening for.
	return ComputeCoverage(LogSink.IsValid(), !NO_LOGGING, DO_ENSURE != 0, DO_BLUEPRINT_GUARD != 0,
		LexToString(FApp::GetBuildConfiguration()));
}

void FFlockAnalyticsProvider::ReportCaptureCoverage()
{
	const FFlockExceptionCaptureCoverage Coverage = GetExceptionCaptureCoverage();
	// Nothing to say when capture is off — the project chose that — or when nothing is out of reach. Nor while nothing
	// is collected: the notice would be dropped, and then recorded as sent.
	if (!Coverage.bEnabled || Coverage.IsComplete() || !IsCollecting())
	{
		return;
	}

	// Once per build, not per launch: the answer cannot change until the build does, and a shipped game would
	// otherwise send the same notice every time a player starts it.
	const FString BuildKey = FString::Printf(TEXT("%s|%s|%s"), *Coverage.BuildConfiguration, *GameVersion, *SdkVersion);
	if (!Deps.CoverageNoticeMarkerPath.IsEmpty())
	{
		FString SentFor;
		if (FFileHelper::LoadFileToString(SentFor, *Deps.CoverageNoticeMarkerPath) && SentFor == BuildKey)
		{
			return;
		}
	}

	// A Shipping or Test build compiles its own log output away, so a warning in the log would never be read. The
	// notice goes to the diagnostics dashboard instead.
	FFlockLogEventRequest Event = MakeLogEvent(EFlockLogEventType::Debug, CoverageNoticeName);
	Event.Data.ExtraData.Add(TEXT("build_configuration"), Coverage.BuildConfiguration);
	Event.Data.ExtraData.Add(TEXT("log_errors"), BoolWire(Coverage.bLogErrors));
	Event.Data.ExtraData.Add(TEXT("ensures"), BoolWire(Coverage.bEnsures));
	Event.Data.ExtraData.Add(TEXT("blueprint_exceptions"), BoolWire(Coverage.bBlueprintExceptions));
	Event.Data.ExtraData.Add(TEXT("blueprint_infinite_loops"), BoolWire(Coverage.bBlueprintInfiniteLoops));
	Event.Data.ExtraData.Add(TEXT("crashes"), BoolWire(Coverage.bCrashes));
	QueueLogEvent(MoveTemp(Event));

	if (!Deps.CoverageNoticeMarkerPath.IsEmpty())
	{
		FFileHelper::SaveStringToFile(BuildKey, *Deps.CoverageNoticeMarkerPath);
	}
}

void FFlockAnalyticsProvider::QueueRepeatReports(const TArray<FFlockRepeatedExceptionCounter::FRepeatReport>& Summaries)
{
	for (const FFlockRepeatedExceptionCounter::FRepeatReport& Summary : Summaries)
	{
		// The category, source and kind the first report carried, so the repeats are counted where it was.
		FFlockLogDetails Details = Summary.Details;
		// The first occurrence was reported when its window opened; this entry carries only what came after it.
		Details.ExtraData.Add(TEXT("repeat_count"), FString::FromInt(Summary.Repeats));
		Details.ExtraData.Add(TEXT("repeat_window_seconds"), FString::SanitizeFloat(RepeatedExceptions.GetRepeatWindowSeconds()));
		// Already counted toward the termination report when each repeat arrived, so SpoolException, not LogException.
		SpoolException(Summary.Message, Summary.StackTrace, Details);
	}
}

void FFlockAnalyticsProvider::RecordTransaction(const FFlockAnalyticsTransactionRequest& InRequest,
	TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete)
{
	// The backend requires a player id on the body, so this is gated on sign-in (a spec-driven gate,
	// not one inferred from a 401): a pre-auth call fails here rather than 401'ing on the wire.
	if (!AuthSessionRef->IsAuthenticated())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockAnalyticsAck>::Fail(FFlockError::Make(EFlockErrorType::Auth,
				TEXT("RecordTransaction requires a signed-in player"))));
		}
		return;
	}
	if (InRequest.Amount < 0.0)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockAnalyticsAck>::Fail(FFlockError::Make(EFlockErrorType::Validation,
				FString::Printf(TEXT("Transaction amount must be non-negative, got: %f"), InRequest.Amount))));
		}
		return;
	}

	FFlockAnalyticsTransactionRequest Request = InRequest;
	if (Request.PlayerId.IsEmpty())
	{
		Request.PlayerId = AuthSessionRef->GetPlayerId();
	}
	if (Request.SessionId.IsEmpty())
	{
		Request.SessionId = GetCurrentSessionId();
	}
	if (Request.CreatedAt.IsEmpty())
	{
		Request.CreatedAt = FDateTime::UtcNow().ToIso8601();
	}

	FString Body;
	if (!FFlockJsonUtils::StructToWireJson(Request, Body, /*bOmitEmptyStrings*/ true))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockAnalyticsAck>::Fail(FFlockError::Make(EFlockErrorType::Serialization,
				TEXT("Failed to serialize transaction"))));
		}
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = AuthSessionRef;
	const FString Url = AnalyticsUrl(FlockEndpoints::AnalyticsTransactions);

	// Bare route: the response is a free-form object nobody reads, so success is the 2xx.
	Execute<FFlockAnalyticsAck>(
		[ClientRef, SessionRef, Url, Body](TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnAttempt)
		{
			return ClientRef->PostJsonRaw<FFlockAnalyticsAck>(Url, SessionRef->GetAuthHeaders(), Body, MoveTemp(OnAttempt));
		},
		MoveTemp(OnComplete), TEXT("Record transaction"));
}

void FFlockAnalyticsProvider::StartSession(const FString& InPlayerIdOrEmpty,
	TFunction<void(TFlockResult<FString>)> OnComplete)
{
	// Empty means "whoever is signed in" — the SDK already knows, so callers should not have to.
	const FString InPlayerId = InPlayerIdOrEmpty.IsEmpty() ? AuthSessionRef->GetPlayerId() : InPlayerIdOrEmpty;

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
	// Replace rather than ignore: handing back the stale id and leaving the old session running was
	// silent data loss — that session's metrics never reached anyone.
	if (Deps.Session->IsActive())
	{
		EndSession(EFlockSessionEndReason::Restarted);
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

	// Previous runs' ends go out before this session registers, at a moment a bearer is definitely
	// live. Anything still queued after this drains on the ordinary flush triggers.
	FlushSessionEnds(nullptr);

	RegisterActiveSession(MoveTemp(OnComplete));
}

FFlockSessionStartRequest FFlockAnalyticsProvider::MakeStartRequest(const FFlockSessionSnapshot& Snapshot) const
{
	FFlockSessionStartRequest Request;
	Request.PlayerId = Snapshot.PlayerId;
	const FFlockDeviceInfo Device = CollectDeviceInfo(SdkVersion);
	Request.Platform = Config.GetSessionPlatform(Device.Platform);
	Request.DeviceType = Device.DeviceType;
	Request.GameVersionId = GameVersion;
	// The snapshot's own start time, so a session registered late — from the spool, days after the
	// fact — is not backdated to the moment we got around to sending it.
	Request.StartedAt = Snapshot.StartTimeUtc.IsEmpty() ? FDateTime::UtcNow().ToIso8601() : Snapshot.StartTimeUtc;
	return Request;
}

void FFlockAnalyticsProvider::RegisterActiveSession(TFunction<void(TFlockResult<FString>)> OnComplete)
{
	if (!Deps.Session.IsValid() || !Deps.Session->IsActive())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FString>::Fail(FFlockError::Make(EFlockErrorType::Validation,
				TEXT("No active session to register"))));
		}
		return;
	}
	if (bRegistrationInFlight)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FString>::Ok(Deps.Session->GetServerSessionId()));
		}
		return;
	}

	bRegistrationInFlight = true;
	const FString LocalId = Deps.Session->GetSessionId();

	FString Body;
	FFlockJsonUtils::StructToWireJson(MakeStartRequest(Deps.Session->TakeSnapshot()), Body,
		/*bOmitEmptyStrings*/ true);

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = AuthSessionRef;
	const FString Url = AnalyticsUrl(FlockEndpoints::AnalyticsSessions);
	const TWeakPtr<FFlockAnalyticsProvider> WeakSelf = AsShared();

	Execute<FFlockSessionStartResponse>(
		[ClientRef, SessionRef, Url, Body](TFunction<void(TFlockResult<FFlockSessionStartResponse>)> OnAttempt)
		{
			return ClientRef->PostJsonRaw<FFlockSessionStartResponse>(Url, SessionRef->GetAuthHeaders(), Body, MoveTemp(OnAttempt));
		},
		[WeakSelf, LocalId, OnComplete](TFlockResult<FFlockSessionStartResponse> Result)
		{
			const TSharedPtr<FFlockAnalyticsProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			Self->bRegistrationInFlight = false;

			// The local session keeps running either way; the server id only affects delivery, and a
			// failure here is healed by the heartbeat or, failing that, from the spool. A 2xx with no
			// id counts as a failure — adopting an empty id would look registered while being anything
			// but, and nothing would ever retry.
			const bool bRegistered = Result.bSuccess && !Result.Value.SessionId.IsEmpty();
			const bool bStillSameSession = bRegistered && Self->Deps.Session.IsValid()
				&& Self->Deps.Session->IsActive() && Self->Deps.Session->GetSessionId() == LocalId;
			if (bStillSameSession)
			{
				Self->Deps.Session->SetServerSessionId(Result.Value.SessionId);
				// Persist at once, so a crash cannot strand a registered session without its id and
				// leave recovery to register it a second time.
				Self->Deps.Session->PersistState();
				if (Self->Deps.TerminationTracker.IsValid())
				{
					Self->Deps.TerminationTracker->SetServerSessionId(Result.Value.SessionId);
				}
				// Only here, for the session still running. A session that ended first is registered from the spool,
				// possibly by a later launch, and announcing that one would hand listeners an id from another run.
				if (UFlockEvents* EventsPtr = Self->Events.Get())
				{
					EventsPtr->InvokeSessionRegistered(LocalId, Result.Value.SessionId);
				}
			}
			else if (bRegistered)
			{
				Self->Logger->LogWarning(FString::Printf(
					TEXT("Session '%s' ended before registration completed; server session '%s' will be closed from the spool"),
					*LocalId, *Result.Value.SessionId));
			}

			if (OnComplete)
			{
				if (bRegistered)
				{
					OnComplete(TFlockResult<FString>::Ok(Result.Value.SessionId));
				}
				else
				{
					OnComplete(TFlockResult<FString>::Fail(Result.bSuccess
						? FFlockError::Make(EFlockErrorType::Network,
							TEXT("Session registration returned no session id"))
						: Result.Error));
				}
			}
		},
		TEXT("Start session"));
}

TFunction<bool(const FFlockError&)> FFlockAnalyticsProvider::SignedOutFailurePredicate() const
{
	const TSharedRef<FFlockAuthSession> SessionRef = AuthSessionRef;
	return [SessionRef](const FFlockError& Error)
	{
		return Error.Type == EFlockErrorType::Auth && !SessionRef->IsAuthenticated();
	};
}

void FFlockAnalyticsProvider::TryHealRegistration()
{
	if (!Deps.Session.IsValid() || !Deps.Session->IsActive()
		|| !Deps.Session->GetServerSessionId().IsEmpty() || bRegistrationInFlight)
	{
		return;
	}
	// Signed out, there is no point spending a request: the end is spooled and will register itself
	// once someone is signed in again.
	if (!AuthSessionRef->IsAuthenticated())
	{
		return;
	}
	RegisterActiveSession(nullptr);
}

void FFlockAnalyticsProvider::StopSessionLocally()
{
	Deps.Session->End();
	Deps.Session->ClearPersistedSession();
	// Every path that closes a session runs this, so the tombstone stops here too — otherwise an
	// explicit EndSession() followed by a clean exit makes the next launch report a crash.
	if (Deps.TerminationTracker.IsValid())
	{
		Deps.TerminationTracker->StopTracking();
	}
}

FFlockSessionSnapshot FFlockAnalyticsProvider::FinishSession(EFlockSessionEndReason Reason, bool& bOutSpooled)
{
	Deps.Session->End();
	const FFlockSessionSnapshot Snapshot = Deps.Session->TakeSnapshot();

	// Spooled before the live record is dropped: at no instant does this end exist only in memory.
	bOutSpooled = false;
	if (Deps.SessionEndCache.IsValid())
	{
		bOutSpooled = !Deps.SessionEndCache->Enqueue(FFlockAnalyticsJson::SerializeSnapshot(Snapshot)).IsEmpty();
	}
	StopSessionLocally();

	if (UFlockEvents* EventsPtr = Events.Get())
	{
		FFlockSessionEndedArgs Args;
		Args.Snapshot = Snapshot;
		Args.Reason = Reason;
		EventsPtr->InvokeSessionEnded(Args);
	}
	return Snapshot;
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

	bool bSpooled = false;
	const FFlockSessionSnapshot Snapshot = FinishSession(Reason, bSpooled);

	if (bSpooled)
	{
		// Delivery is the drain's job from here; this end is simply the newest thing in the queue.
		FlushSessionEnds(MoveTemp(OnComplete));
		return;
	}

	// Spooling is off or the queue refused it, so now is the only chance. A session that never got a
	// server id has nothing to close out remotely.
	if (Snapshot.ServerSessionId.IsEmpty())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockAnalyticsAck>::Ok(FFlockAnalyticsAck()));
		}
		return;
	}
	PatchSessionEnd(Snapshot.ServerSessionId, Snapshot, MoveTemp(OnComplete));
}

void FFlockAnalyticsProvider::DiscardSession()
{
	if (!Deps.Session.IsValid() || !Deps.Session->IsActive())
	{
		return;
	}

	const FString DiscardedId = Deps.Session->GetSessionId();
	StopSessionLocally();

	// No spool, no send, and no OnSessionEnded: consent was withdrawn, so this session's record must
	// not leave the device, and a listener reacting to an "ended" session would be reacting to data
	// the player just asked us to forget.
	Logger->LogInfo(FString::Printf(TEXT("Analytics session discarded (consent revoked): %s"), *DiscardedId));
}

void FFlockAnalyticsProvider::HandleLoggedOut()
{
	// Ordinarily a no-op — UFlockSubsystem::Logout() already closed the session while the bearer was
	// still alive. This is the catch-all for every other way auth can end: expiry, revoke, or a
	// direct call on the auth provider.
	EndSession(EFlockSessionEndReason::Logout);
	KnownPlayerId.Reset();
}

void FFlockAnalyticsProvider::RecoverOrphanedSession()
{
	if (!Deps.Session.IsValid())
	{
		return;
	}

	FFlockSessionSnapshot Orphan;
	if (!Deps.Session->RecoverOrphanedSession(Orphan))
	{
		return;
	}

	// Same rule as the termination marker: with collection off the record is dropped rather than
	// left to be re-read on every launch from now on.
	if (!IsCollecting() || !Deps.SessionEndCache.IsValid())
	{
		Deps.Session->ClearPersistedSession();
		return;
	}

	// Spool first, clear second — clearing first loses the session if the write fails.
	if (Deps.SessionEndCache->Enqueue(FFlockAnalyticsJson::SerializeSnapshot(Orphan)).IsEmpty())
	{
		Logger->LogWarning(FString::Printf(
			TEXT("Could not spool the end of orphaned session '%s'; keeping it for the next launch"),
			*Orphan.SessionId));
		return;
	}
	Deps.Session->ClearPersistedSession();
	Logger->LogInfo(FString::Printf(
		TEXT("Recovered a session the previous run left open: %s (ended at %s)"),
		*Orphan.SessionId, *Orphan.EndTimeUtc));
}

void FFlockAnalyticsProvider::PatchSessionEnd(const FString& ServerSessionId,
	const FFlockSessionSnapshot& Snapshot, TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete,
	TFunction<bool(const FFlockError&)> IsExpectedFailure)
{
	FFlockSessionEndRequest Request;
	Request.DurationSeconds = FMath::RoundToInt(Snapshot.DurationSeconds);
	Request.ScreensViewed = Snapshot.ScreensViewed;
	Request.IsBounce = Snapshot.IsBounce;
	Request.EndedAt = Snapshot.EndTimeUtc;

	FString Body;
	FFlockJsonUtils::StructToWireJson(Request, Body, /*bOmitEmptyStrings*/ true);

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = AuthSessionRef;
	const FString Url = AnalyticsUrl(FlockEndpoints::AnalyticsSessionById(ServerSessionId));

	Execute<FFlockAnalyticsAck>(
		[ClientRef, SessionRef, Url, Body](TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnAttempt)
		{
			return ClientRef->PatchJsonRaw<FFlockAnalyticsAck>(Url, SessionRef->GetAuthHeaders(), Body, MoveTemp(OnAttempt));
		},
		MoveTemp(OnComplete), TEXT("End session"), /*bIdempotent*/ true, /*MaxRetriesOverride*/ -1,
		/*bAllowAuthRetry*/ true, MoveTemp(IsExpectedFailure));
}

void FFlockAnalyticsProvider::FlushSessionEnds(TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete)
{
	if (!Deps.SessionEndCache.IsValid() || Deps.SessionEndCache->PendingCount() == 0 || bEndFlushInFlight)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockAnalyticsAck>::Ok(FFlockAnalyticsAck()));
		}
		return;
	}

	// `POST analytics/sessions` requires `player_id`, so a pass that may have to register cannot
	// succeed with nobody signed in — left ungated it 401s on the flush interval for the whole run.
	// Gating the pass rather than just the register step costs little: an end that already has a
	// server id is normally delivered at FinishSession time, while the bearer is still live.
	// StartSession drains this the moment someone signs in, the first moment it could have worked.
	//
	// The log spool is deliberately NOT gated this way. Whether a route needs auth is read off the
	// OpenAPI spec and nothing else: `log_event` declares no security and its body carries no player,
	// so a log entry has no reason to wait for a sign-in.
	if (!AuthSessionRef->IsAuthenticated())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockAnalyticsAck>::Ok(FFlockAnalyticsAck()));
		}
		return;
	}

	bEndFlushInFlight = true;
	SendNextEnd(MakeShared<TSet<FString>>(), MoveTemp(OnComplete));
}

void FFlockAnalyticsProvider::SendNextEnd(const TSharedRef<TSet<FString>>& Delivered,
	TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete)
{
	TArray<FString> Handles;
	TArray<FString> Payloads;
	// One at a time: an end may need its own registration before it can be closed, so a batch would
	// buy nothing here.
	Deps.SessionEndCache->PeekBatch(1, Handles, Payloads);

	if (Handles.Num() == 0)
	{
		bEndFlushInFlight = false;
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockAnalyticsAck>::Ok(FFlockAnalyticsAck()));
		}
		return;
	}

	const FString Handle = Handles[0];
	FFlockSessionSnapshot Snapshot;
	if (!FFlockAnalyticsJson::DeserializeSnapshot(Payloads[0], Snapshot) || Snapshot.SessionId.IsEmpty())
	{
		// A record we can no longer read will never become deliverable; drop it rather than wedge
		// every later end behind it.
		Deps.SessionEndCache->Remove(Handle);
		SendNextEnd(Delivered, MoveTemp(OnComplete));
		return;
	}

	if (Delivered->Contains(Snapshot.SessionId))
	{
		// A quit record and a crash-recovery record can describe the same session. Entries are
		// oldest-first and the older one is the accurate one, so this later copy goes.
		Logger->LogDebug(FString::Printf(TEXT("Skipping a duplicate spooled end: %s"), *Snapshot.SessionId));
		Deps.SessionEndCache->Remove(Handle);
		SendNextEnd(Delivered, MoveTemp(OnComplete));
		return;
	}

	SendSpooledEnd(Handle, Snapshot, Delivered, MoveTemp(OnComplete));
}

void FFlockAnalyticsProvider::SendSpooledEnd(const FString& Handle, const FFlockSessionSnapshot& Snapshot,
	const TSharedRef<TSet<FString>>& Delivered, TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete)
{
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = AuthSessionRef;
	const TWeakPtr<FFlockAnalyticsProvider> WeakSelf = AsShared();

	if (Snapshot.ServerSessionId.IsEmpty())
	{
		// Never registered — signed out or offline when it started, or recovered from a run that died
		// before the POST landed. The route accepts a historical started_at, so it can be registered
		// now and closed in the same pass.
		FString Body;
		FFlockJsonUtils::StructToWireJson(MakeStartRequest(Snapshot), Body, /*bOmitEmptyStrings*/ true);
		const FString Url = AnalyticsUrl(FlockEndpoints::AnalyticsSessions);

		Execute<FFlockSessionStartResponse>(
			[ClientRef, SessionRef, Url, Body](TFunction<void(TFlockResult<FFlockSessionStartResponse>)> OnAttempt)
			{
				return ClientRef->PostJsonRaw<FFlockSessionStartResponse>(Url, SessionRef->GetAuthHeaders(), Body, MoveTemp(OnAttempt));
			},
			[WeakSelf, Handle, Snapshot, Delivered, OnComplete](TFlockResult<FFlockSessionStartResponse> Result)
			{
				const TSharedPtr<FFlockAnalyticsProvider> Self = WeakSelf.Pin();
				if (!Self.IsValid())
				{
					return;
				}
				// A 2xx carrying no id is not a registration. It has to be treated as a failure rather
				// than retried in place: the empty id is what sent this record down the register
				// branch, so re-entering it would recurse until the stack gives out. Classified
				// transient, because the record is fine — it is the answer that was not.
				if (!Result.bSuccess || Result.Value.SessionId.IsEmpty())
				{
					const FFlockError Error = Result.bSuccess
						? FFlockError::Make(EFlockErrorType::Network,
							TEXT("Session registration returned no session id"))
						: Result.Error;
					Self->HandleEndFailure(Handle, Snapshot.SessionId, Error, Delivered, OnComplete);
					return;
				}

				FFlockSessionSnapshot Registered = Snapshot;
				Registered.ServerSessionId = Result.Value.SessionId;
				// Rewritten in place before the close is attempted: if the PATCH fails, the retry must
				// pick up the id we already have rather than open a second server session for the
				// same play session.
				Self->Deps.SessionEndCache->Replace(Handle, FFlockAnalyticsJson::SerializeSnapshot(Registered));
				Self->SendSpooledEnd(Handle, Registered, Delivered, OnComplete);
			},
			TEXT("Register spooled session"), /*bIdempotent*/ true, /*MaxRetriesOverride*/ -1,
			/*bAllowAuthRetry*/ true, SignedOutFailurePredicate());
		return;
	}

	const FString SessionId = Snapshot.SessionId;
	PatchSessionEnd(Snapshot.ServerSessionId, Snapshot,
		[WeakSelf, Handle, SessionId, Delivered, OnComplete](TFlockResult<FFlockAnalyticsAck> Result)
		{
			const TSharedPtr<FFlockAnalyticsProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (!Result.bSuccess)
			{
				Self->HandleEndFailure(Handle, SessionId, Result.Error, Delivered, OnComplete);
				return;
			}

			Delivered->Add(SessionId);
			Self->Deps.SessionEndCache->Remove(Handle);
			Self->SendNextEnd(Delivered, OnComplete);
		},
		SignedOutFailurePredicate());
}

void FFlockAnalyticsProvider::HandleEndFailure(const FString& Handle, const FString& SessionId,
	const FFlockError& Error, const TSharedRef<TSet<FString>>& Delivered,
	TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete)
{
	// "The retry handler gave up" is the same question, so it is asked in one place rather than
	// restated here — a permanence table that exists twice drifts the moment an error type is added.
	// Two carve-outs, both meaning "we learned nothing, so keep the record": Auth is a wait for the
	// next sign-in, and Cancelled never reached the server at all.
	const bool bFinal = Error.Type != EFlockErrorType::Auth
		&& Error.Type != EFlockErrorType::Cancelled
		&& !FFlockRetryHandler::ShouldRetry(Error, /*bIdempotent*/ true);

	if (bFinal)
	{
		Logger->LogWarning(FString::Printf(TEXT("Session end for '%s' rejected (%s); dropping it"),
			*SessionId, *Error.Message));
		Deps.SessionEndCache->Remove(Handle);
		SendNextEnd(Delivered, MoveTemp(OnComplete));
		return;
	}

	// Transient, or simply signed out: leave it spooled and stop the pass. The next trigger retries.
	bEndFlushInFlight = false;
	if (OnComplete)
	{
		OnComplete(TFlockResult<FFlockAnalyticsAck>::Fail(Error));
	}
}

void FFlockAnalyticsProvider::Flush(TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete)
{
	const TWeakPtr<FFlockAnalyticsProvider> WeakSelf = AsShared();
	FlushSessionEnds([WeakSelf, OnComplete](TFlockResult<FFlockAnalyticsAck> EndResult)
	{
		const TSharedPtr<FFlockAnalyticsProvider> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}
		// The log spool is drained whatever the ends did — one failing queue must not hold the other
		// hostage. The reported outcome is the first failure, so a caller is never told "delivered"
		// when something was left behind.
		Self->FlushLogEvents([WeakSelf, EndResult, OnComplete](TFlockResult<FFlockAnalyticsAck> LogResult)
		{
			const TSharedPtr<FFlockAnalyticsProvider> Inner = WeakSelf.Pin();
			if (!Inner.IsValid())
			{
				return;
			}
			// Gameplay events last, on their own flag: no queue's failure holds another hostage.
			Inner->FlushAnalyticsEvents([EndResult, LogResult, OnComplete](TFlockResult<FFlockAnalyticsAck> EventResult)
			{
				if (OnComplete)
				{
					OnComplete(!EndResult.bSuccess ? EndResult : (!LogResult.bSuccess ? LogResult : EventResult));
				}
			});
		});
	});
}

void FFlockAnalyticsProvider::FlushLogEvents(TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete)
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
	SendNextBatch(MakeShared<FDeliveryPass>(), MoveTemp(OnComplete));
}

void FFlockAnalyticsProvider::SendNextBatch(const TSharedRef<FDeliveryPass>& Pass,
	TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete)
{
	TArray<FString> Handles;
	TArray<FString> Payloads;
	Deps.LogEventCache->PeekBatch(Pass->NextBatchSize(Config.CacheFlushBatchSize), Handles, Payloads);

	if (Handles.Num() == 0)
	{
		bFlushInFlight = false;
		if (OnComplete)
		{
			OnComplete(Pass->ResultToReport());
		}
		return;
	}

	TArray<FFlockLogEventRequest> Batch;
	TArray<FString> SentHandles;
	TArray<FString> SentPayloads;
	for (int32 Index = 0; Index < Handles.Num(); ++Index)
	{
		FFlockLogEventRequest Event;
		if (FFlockAnalyticsJson::DeserializeEvent(Payloads[Index], Event))
		{
			Batch.Add(MoveTemp(Event));
			SentHandles.Add(Handles[Index]);
			SentPayloads.Add(Payloads[Index]);
		}
		else
		{
			// A spool entry we can no longer parse will never become deliverable; drop it rather
			// than wedging the queue behind it forever.
			Deps.LogEventCache->Remove(Handles[Index]);
			Pass->NoteEntryDone();
		}
	}
	if (Batch.Num() == 0)
	{
		SendNextBatch(Pass, MoveTemp(OnComplete));
		return;
	}

	const FString Body = FFlockAnalyticsJson::SerializeEvents(Batch);
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = AuthSessionRef;
	const FString Url = AnalyticsUrl(FlockEndpoints::LogEvent);
	const TWeakPtr<FFlockAnalyticsProvider> WeakSelf = AsShared();

	Execute<FFlockAnalyticsAck>(
		[ClientRef, SessionRef, Url, Body](TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnAttempt)
		{
			return ClientRef->PostJsonRaw<FFlockAnalyticsAck>(Url, SessionRef->GetAuthHeaders(), Body, MoveTemp(OnAttempt));
		},
		[WeakSelf, Pass, SentHandles, SentPayloads, OnComplete](TFlockResult<FFlockAnalyticsAck> Result)
		{
			const TSharedPtr<FFlockAnalyticsProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			if (Result.bSuccess)
			{
				for (const FString& Handle : SentHandles)
				{
					Self->Deps.LogEventCache->Remove(Handle);
				}
				Pass->NoteEntryDone();
				Self->SendNextBatch(Pass, OnComplete);
				return;
			}

			switch (DecideFailedSendAction(Result.Error))
			{
			case EFlockFailedSendAction::Discard:
				if (SentHandles.Num() > 1 && IsRefusalOfContent(Result.Error))
				{
					// One entry can make the server refuse the whole body. Sent apart, only that entry goes.
					Pass->EntriesToSendOneAtATime = SentHandles.Num();
				}
				else
				{
					// Refused, and it would be refused on every flush forever, holding every later entry behind it.
					Self->Logger->LogWarning(FString::Printf(TEXT("Dropping %d log entr%s the server refused (%s)"),
						SentHandles.Num(), SentHandles.Num() == 1 ? TEXT("y") : TEXT("ies"), *Result.Error.Message));
					for (const FString& Handle : SentHandles)
					{
						Self->Deps.LogEventCache->Remove(Handle);
					}
					Pass->NoteEntryRefused(Result.Error);
				}
				Self->SendNextBatch(Pass, OnComplete);
				return;
			case EFlockFailedSendAction::RetryCountingAttempt:
				Self->CountFailedLogSends(SentHandles, SentPayloads);
				break;
			case EFlockFailedSendAction::RetryWithoutCountingAttempt:
				break;
			}

			// Kept for the next flush, and the pass stops here.
			Self->bFlushInFlight = false;
			if (OnComplete)
			{
				OnComplete(Result);
			}
		},
		TEXT("Flush log events"));
}

void FFlockAnalyticsProvider::CountFailedLogSends(const TArray<FString>& Handles, const TArray<FString>& Payloads)
{
	// The payloads the batch was built from, so charging costs no second read. Replace ignores a handle the spool no
	// longer holds, so an entry cleared while its batch was away is not written back.
	for (int32 Index = 0; Index < Handles.Num() && Index < Payloads.Num(); ++Index)
	{
		const int32 FailedSends = FFlockAnalyticsJson::ReadFailedSendCount(Payloads[Index]) + 1;
		if (FailedSends >= MaxFailedSends)
		{
			Logger->LogWarning(FString::Printf(TEXT("Dropping a log entry after %d failed sends"), FailedSends));
			Deps.LogEventCache->Remove(Handles[Index]);
			continue;
		}
		Deps.LogEventCache->Replace(Handles[Index], FFlockAnalyticsJson::WithFailedSendCount(Payloads[Index], FailedSends));
	}
}

void FFlockAnalyticsProvider::FlushAnalyticsEvents(TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete)
{
	// Nothing goes while nobody is signed in. A held event has no player to send, and a previous player's events can
	// wait for the next sign-in, which flushes — scanning the spool every interval only to skip it would not.
	if (!Deps.AnalyticsEventCache.IsValid() || Deps.AnalyticsEventCache->PendingCount() == 0 || bEventsFlushInFlight
		|| !AuthSessionRef->IsAuthenticated())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockAnalyticsAck>::Ok(FFlockAnalyticsAck()));
		}
		return;
	}

	bEventsFlushInFlight = true;
	SendNextEventBatch(MakeShared<FDeliveryPass>(), MoveTemp(OnComplete));
}

void FFlockAnalyticsProvider::SendNextEventBatch(const TSharedRef<FDeliveryPass>& Pass,
	TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete)
{
	// No consent check per batch: withdrawing consent clears this spool, so a pass that outlives it finds nothing.
	if (!Deps.AnalyticsEventCache.IsValid())
	{
		bEventsFlushInFlight = false;
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockAnalyticsAck>::Ok(FFlockAnalyticsAck()));
		}
		return;
	}

	// One player per batch. The route checks every event's player and refuses the whole batch for one it does not
	// know — measured: the good events in that batch are not stored — so mixing players would let one deleted
	// account take everyone else's events down with it.
	//
	// Held events (nobody was signed in) are skipped, not sent: an empty player is the same refusal. The scan is
	// bounded, so a long signed-out stretch does not cost a disk read per held event on every flush; held events
	// become sendable the moment someone signs in anyway.
	const int32 BatchSize = Pass->NextBatchSize(Config.CacheFlushBatchSize);
	TArray<FString> ScanHandles;
	TArray<FString> ScanPayloads;
	Deps.AnalyticsEventCache->PeekBatch(FMath::Max(BatchSize * 4, 200), ScanHandles, ScanPayloads);

	TArray<FString> Handles;
	TArray<FFlockSpooledAnalyticsEvent> Entries;
	FString BatchPlayer;
	for (int32 Index = 0; Index < ScanHandles.Num() && Handles.Num() < BatchSize; ++Index)
	{
		FFlockSpooledAnalyticsEvent Entry;
		if (!FFlockAnalyticsJson::DeserializeSpooledAnalyticsEvent(ScanPayloads[Index], Entry))
		{
			// Unreadable, or not an event: it can never become deliverable.
			Deps.AnalyticsEventCache->Remove(ScanHandles[Index]);
			continue;
		}
		if (IsEventTooLongToStore(Entry.Event.EventName, Entry.Event.EventCategory))
		{
			// Recorded by a build that did not check. The server would fail every batch carrying it, on every flush.
			Logger->LogWarning(FString::Printf(
				TEXT("Dropping analytics event '%s': its name or category is longer than the server can store."),
				*Entry.Event.EventName.Left(MaxEventNameLength)));
			Deps.AnalyticsEventCache->Remove(ScanHandles[Index]);
			continue;
		}
		if (Entry.Event.PlayerId.IsEmpty())
		{
			continue;
		}
		if (BatchPlayer.IsEmpty())
		{
			BatchPlayer = Entry.Event.PlayerId;
		}
		else if (Entry.Event.PlayerId != BatchPlayer)
		{
			continue;
		}

		// Recorded before its session registered: attach the id the server knows that session by now.
		if (Entry.Event.SessionId.IsEmpty() && !Entry.LocalSessionId.IsEmpty() && Deps.Session.IsValid()
			&& Entry.LocalSessionId == Deps.Session->GetSessionId() && !Deps.Session->GetServerSessionId().IsEmpty()
			&& !RefusedSessionIds.Contains(Deps.Session->GetServerSessionId()))
		{
			Entry.Event.SessionId = Deps.Session->GetServerSessionId();
			Deps.AnalyticsEventCache->Replace(ScanHandles[Index], FFlockAnalyticsJson::SerializeSpooledAnalyticsEvent(Entry));
		}

		Handles.Add(ScanHandles[Index]);
		Entries.Add(MoveTemp(Entry));
	}

	if (Handles.Num() == 0)
	{
		bEventsFlushInFlight = false;
		if (OnComplete)
		{
			OnComplete(Pass->ResultToReport());
		}
		return;
	}

	TArray<FFlockAnalyticsEventRequest> Batch;
	Batch.Reserve(Entries.Num());
	for (const FFlockSpooledAnalyticsEvent& Entry : Entries)
	{
		Batch.Add(Entry.Event);
	}

	const FString Body = FFlockAnalyticsJson::SerializeAnalyticsEvents(Batch);
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = AuthSessionRef;
	const FString Url = AnalyticsUrl(FlockEndpoints::AnalyticsEvents);
	const TWeakPtr<FFlockAnalyticsProvider> WeakSelf = AsShared();

	// Not idempotent: the server stores every event it is sent, so an ambiguous failure is not retried in place.
	// The batch stays spooled and the next flush sends it once more.
	Execute<FFlockAnalyticsAck>(
		[ClientRef, SessionRef, Url, Body](TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnAttempt)
		{
			return ClientRef->PostJsonRaw<FFlockAnalyticsAck>(Url, SessionRef->GetAuthHeaders(), Body, MoveTemp(OnAttempt));
		},
		[WeakSelf, Pass, Handles, Entries, BatchPlayer, OnComplete](TFlockResult<FFlockAnalyticsAck> Result)
		{
			const TSharedPtr<FFlockAnalyticsProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (Result.bSuccess)
			{
				for (const FString& Handle : Handles)
				{
					Self->Deps.AnalyticsEventCache->Remove(Handle);
				}
				Pass->bRetriedWithoutSession = false;
				Pass->NoteEntryDone();
				Self->SendNextEventBatch(Pass, OnComplete);
				return;
			}
			Self->HandleEventBatchFailure(Pass, Handles, Entries, BatchPlayer, Result.Error, OnComplete);
		},
		TEXT("Flush analytics events"), /*bIdempotent*/ false);
}

void FFlockAnalyticsProvider::HandleEventBatchFailure(const TSharedRef<FDeliveryPass>& Pass, const TArray<FString>& Handles,
	const TArray<FFlockSpooledAnalyticsEvent>& Entries, const FString& BatchPlayer, const FFlockError& Error,
	TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete)
{
	// 409 is a session id the server does not know. The events themselves are fine, so they go out once more
	// without session ids before anything is dropped.
	bool bCarriesSessionIds = false;
	for (const FFlockSpooledAnalyticsEvent& Entry : Entries)
	{
		bCarriesSessionIds |= !Entry.Event.SessionId.IsEmpty();
	}
	if (Error.StatusCode == 409 && bCarriesSessionIds && !Pass->bRetriedWithoutSession)
	{
		for (int32 Index = 0; Index < Handles.Num(); ++Index)
		{
			FFlockSpooledAnalyticsEvent Stripped = Entries[Index];
			if (!Stripped.Event.SessionId.IsEmpty())
			{
				// Remembered, so neither this resend nor any event recorded later attaches it again: each would cost a
				// refusal and a resend.
				RefusedSessionIds.Add(Stripped.Event.SessionId);
			}
			Stripped.Event.SessionId.Reset();
			Deps.AnalyticsEventCache->Replace(Handles[Index], FFlockAnalyticsJson::SerializeSpooledAnalyticsEvent(Stripped));
		}
		Pass->bRetriedWithoutSession = true;
		SendNextEventBatch(Pass, MoveTemp(OnComplete));
		return;
	}

	switch (DecideFailedSendAction(Error))
	{
	case EFlockFailedSendAction::Discard:
		if (Handles.Num() > 1 && IsRefusalOfContent(Error))
		{
			// One event can make the server refuse the whole body. Sent apart, only that event goes.
			Pass->EntriesToSendOneAtATime = Handles.Num();
		}
		else
		{
			Logger->LogWarning(FString::Printf(TEXT("Dropping %d analytics event(s) for player '%s' the server refused (%s)"),
				Handles.Num(), *BatchPlayer, *Error.Message));
			for (const FString& Handle : Handles)
			{
				Deps.AnalyticsEventCache->Remove(Handle);
			}
			Pass->NoteEntryRefused(Error);
		}
		Pass->bRetriedWithoutSession = false;
		SendNextEventBatch(Pass, MoveTemp(OnComplete));
		return;
	case EFlockFailedSendAction::RetryCountingAttempt:
		for (int32 Index = 0; Index < Handles.Num(); ++Index)
		{
			FFlockSpooledAnalyticsEvent Counted = Entries[Index];
			if (++Counted.FailedSends >= MaxFailedSends)
			{
				Logger->LogWarning(FString::Printf(TEXT("Dropping analytics event '%s' after %d failed sends"),
					*Counted.Event.EventName, Counted.FailedSends));
				Deps.AnalyticsEventCache->Remove(Handles[Index]);
				continue;
			}
			Deps.AnalyticsEventCache->Replace(Handles[Index], FFlockAnalyticsJson::SerializeSpooledAnalyticsEvent(Counted));
		}
		break;
	case EFlockFailedSendAction::RetryWithoutCountingAttempt:
		break;
	}

	// Kept for the next flush, and the pass stops here.
	bEventsFlushInFlight = false;
	if (OnComplete)
	{
		OnComplete(TFlockResult<FFlockAnalyticsAck>::Fail(Error));
	}
}

void FFlockAnalyticsProvider::EraseLocalData()
{
	if (Deps.LogEventCache.IsValid())
	{
		Deps.LogEventCache->Clear();
	}
	if (Deps.SessionEndCache.IsValid())
	{
		Deps.SessionEndCache->Clear();
	}
	if (Deps.AnalyticsEventCache.IsValid())
	{
		Deps.AnalyticsEventCache->Clear();
	}
	if (Deps.Session.IsValid())
	{
		Deps.Session->ClearPersistedSession();
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
	const double Now = NowSeconds();
	// Windows that closed since the last drain go out first, so a storm's summary is not overtaken by the next one.
	QueueRepeatReports(RepeatedExceptions.CollectFinished(Now));

	FFlockCapturedLog Captured;
	while (LogSink->Dequeue(Captured))
	{
		const FFlockLogDetails Details = DetailsOfCapturedFault(Captured);
		const FString SameFaultKey = FFlockRepeatedExceptionCounter::MakeSameFaultKey(Captured.Category, Captured.Message, Captured.StackTrace);
		if (!RepeatedExceptions.ShouldReportNow(SameFaultKey, Captured.Message, Captured.StackTrace, Details, Now))
		{
			// A repeat inside an open window: counted into its summary, and still pressure for the termination report.
			if (Deps.TerminationTracker.IsValid())
			{
				Deps.TerminationTracker->NoteException();
			}
			continue;
		}

		// The sink already walked the stack where the error happened; passing it here stops
		// LogException walking a second, useless one rooted in the drain loop.
		LogException(Captured.Message, Captured.StackTrace, Details);
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
				// Refreshes the recoverable record too, so a crash costs at most one heartbeat of
				// duration rather than the whole session.
				Deps.Session->PersistState();
			}
			if (Deps.TerminationTracker.IsValid())
			{
				Deps.TerminationTracker->HandleHeartbeat();
			}
			// A session whose registration failed at start can never be closed on the server; heal it
			// here rather than leave the end to re-register itself from the spool.
			TryHealRegistration();
		}
	}

	if (Config.EventBufferFlushIntervalSeconds > 0.f)
	{
		FlushAccumulator += DeltaSeconds;
		if (FlushAccumulator >= FMath::Max(Config.EventBufferFlushIntervalSeconds, FlushWaitSeconds))
		{
			FlushAccumulator = 0.f;
			const TWeakPtr<FFlockAnalyticsProvider> WeakSelf = AsShared();
			Flush([WeakSelf](TFlockResult<FFlockAnalyticsAck> Result)
			{
				if (const TSharedPtr<FFlockAnalyticsProvider> Self = WeakSelf.Pin())
				{
					Self->UpdateFlushWait(Result);
				}
			});
		}
	}
}

void FFlockAnalyticsProvider::UpdateFlushWait(const TFlockResult<FFlockAnalyticsAck>& Result)
{
	const bool bStillQueued = GetPendingEventCount() > 0 || GetPendingAnalyticsEventCount() > 0
		|| (Deps.SessionEndCache.IsValid() && Deps.SessionEndCache->PendingCount() > 0);
	if (Result.bSuccess || !bStillQueued)
	{
		FlushWaitSeconds = 0.f;
		return;
	}
	// An unanswered failure spends no attempt, so there is nothing to stretch the wait for.
	if (Result.Error.StatusCode == 0)
	{
		return;
	}
	const float Ceiling = FMath::Max(MaxFlushWaitSeconds, Config.EventBufferFlushIntervalSeconds);
	FlushWaitSeconds = FMath::Min(FMath::Max(FlushWaitSeconds, Config.EventBufferFlushIntervalSeconds) * 2.f, Ceiling);
}

bool FFlockAnalyticsProvider::IsRefusalOfContent(const FFlockError& Error)
{
	// Malformed, too large, or a field the server will not take: each can come from one entry in the batch. A refusal
	// about who is asking or what is addressed cannot, and taking the batch apart for one would only repeat it.
	return Error.StatusCode == 400 || Error.StatusCode == 413 || Error.StatusCode == 422;
}

void FFlockAnalyticsProvider::TrackEventAsPlayerForTesting(const FString& PlayerId, const FString& EventName)
{
	if (!Deps.AnalyticsEventCache.IsValid())
	{
		return;
	}
	FFlockSpooledAnalyticsEvent Entry;
	Entry.Event.PlayerId = PlayerId;
	Entry.Event.EventName = EventName;
	Entry.Event.Timestamp = FDateTime::UtcNow().ToIso8601();
	Deps.AnalyticsEventCache->Enqueue(FFlockAnalyticsJson::SerializeSpooledAnalyticsEvent(Entry));
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
			// Backgrounded is where an OS eviction happens, so the recoverable record is refreshed
			// before we risk not running again.
			Deps.Session->PersistState();
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
	// Same as Shutdown: faults and repeat counts with no later tick to spool them.
	DrainLogSink();
	QueueRepeatReports(RepeatedExceptions.CollectAll());

	if (Config.bAutoEndSessionOnQuit && HasActiveSession())
	{
		EndSession(EFlockSessionEndReason::Quit);
	}
	// The end is already durable, so this is only a last-chance delivery attempt — whatever does not
	// finish before the process dies stays on disk and goes out on the next launch.
	Flush();
	if (Deps.TerminationTracker.IsValid())
	{
		Deps.TerminationTracker->StopTracking();
	}
}

void FFlockAnalyticsProvider::TickForTesting(float DeltaSeconds)
{
	HandleTick(DeltaSeconds);
}
