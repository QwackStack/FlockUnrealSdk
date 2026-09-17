// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Analytics/FlockLifecyclePump.h"
#include "FlockPlaytestConfig.h"
#include "FlockPlaytestFormAnswers.h"
#include "FlockPlaytestIdentity.h"
#include "FlockPlaytestPerformanceTimeline.h"
#include "FlockPlaytestSession.h"
#include "FlockPlaytestStatus.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockRetryPolicy.h"
#include "Models/FlockCommandModels.h"
#include "Async/Future.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "FlockPlaytestSubsystem.generated.h"

class FFlockPlaytestRecordingRun;
class FFlockPlaytestVideoRecording;
class FFlockPlaytestFormKeyWatcher;
class FFlockPlaytestRecordingUploads;
class SFlockPlaytestFormWidget;
class FFlockProtokiteClient;
class IFlockFileUploader;
class IFlockPlaytestVideoFrameSource;
class UFlockSubsystem;
enum class EFlockPlaytestVideoStopReason : uint8;
class UGameInstance;
class UWorld;
struct FWorldContext;

/**
 * The playtest plugin's runtime home, one per game instance.
 *
 * Keeps one answer current: whether this build may do playtest work (GetStatus). It decides when it starts,
 * and again whenever the Flock SDK initializes or shuts down. Once the settings allow it and the Flock SDK is
 * initialized, it fetches this build's playtest config from Protokite, once per Flock initialization, and
 * forgets it when the Flock SDK shuts down. It moves to Stopped when its game instance shuts down.
 *
 * It also runs the launch's one Protokite session. The session starts once the playtest config is loaded and the
 * current Flock initialization's first session has reached the server, whichever of the two happens last, and it
 * names that Flock session. A Flock session only exists after a player signs in, with the Flock SDK's analytics on,
 * Analytics Auto Start Session on (or a Start Session call) and consent granted when Analytics Require Explicit
 * Consent is on. A later Flock session, a sign-out or the Flock SDK shutting down neither ends nor restarts it, and
 * neither does the Flock SDK initializing again with another API key or Game Version ID: the session keeps the
 * address and headers it started with. It ends when the game instance shuts down, or when EndPlaytestSession is
 * called.
 *
 * When the playtest turns heavy analytics on, it also sends the Flock SDK a performance window for every ten seconds
 * of play and an event for every level this game instance loads, under the playtest category, and takes the game's
 * own playtest events (RecordPlaytestEvent). None of it runs unless the status is Ready and the Flock SDK's analytics
 * is on, and it pauses while the game is in the background.
 *
 * When the playtest turns video recording on, it records the game's screen from the moment the config is loaded, to a
 * VP9 file under Saved/FlockPlaytest/Recordings/Playtest, with the Video Recording settings. One recording is made per
 * launch: once it stops, at its length or size limit, when the game calls StopVideoRecording, when playtesting stops or
 * when the game instance shuts down, no other starts. Background time is not recorded. Only 64-bit Windows builds that
 * draw something record video; elsewhere one warning says so and the rest of the playtest carries on. The same recording
 * can be tried without a playtest, saved under Recordings/TestVideos: Record Video In Play In Editor (Flock Playtest Local
 * Settings), or the console command FlockPlaytest.RecordTestVideo <seconds> in builds that are not Shipping.
 *
 * A recording belongs to its game instance until it shuts down, and nothing else touches it meanwhile. A playtest
 * recording is saved with the Protokite session it belongs to and is kept after its launch ends, waiting to be uploaded.
 * When a launch starts, a recording an earlier one was still writing (the game was closed or crashed) is finished with
 * every whole frame it holds, and a playtest recording that no Protokite session started for is deleted, since it can
 * never be uploaded. Before a recording starts, the oldest recordings of launches that have ended are deleted until it
 * fits inside Recordings Disk Budget.
 *
 * Each change is logged once: a setting or a refusal that stops playtesting is a warning, waiting, fetching
 * and being ready are logged, and playtesting turned off stays quiet, because that is the chosen state of
 * every build that is not a playtest build.
 */
UCLASS()
class FLOCKPLAYTEST_API UFlockPlaytestSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem interface

	/** Whether playtest work may run right now, and if not, why. Reading it changes nothing. */
	EFlockPlaytestStatus GetStatus() const { return Status; }

	/** This build's playtest config. Empty unless it has been fetched for the current Flock initialization. */
	const FFlockPlaytestConfig& GetPlaytestConfig() const { return PlaytestConfig; }

	/**
	 * True only while GetStatus() is Ready and the playtest's config turns FeatureName on (see
	 * FlockPlaytestFeatures). A feature the config does not mention is off.
	 */
	bool IsPlaytestFeatureEnabled(const FString& FeatureName) const;

	/** Where this launch's Protokite session is. Reading it changes nothing. */
	EFlockPlaytestSessionState GetPlaytestSessionState() const { return SessionState; }

	/** The id Protokite gave this launch's session. Empty until the session has started, and kept once it has ended. */
	const FString& GetPlaytestSessionId() const { return PlaytestSessionId; }

	/** The identity this launch's session start sent. Empty until a start has resolved it. */
	const FFlockPlaytestIdentity& GetPlaytestIdentity() const { return PlaytestIdentity; }

	/**
	 * Ends this launch's Protokite session now, for a game that quits on its own schedule. Protokite ignores an end
	 * for a session that has already ended, so calling this twice sends two ends. Returns false, and sends nothing,
	 * when no session has started. The session also ends by itself when the game instance shuts down.
	 */
	bool EndPlaytestSession();

	/**
	 * Records one of the game's own events for this playtest, sent through the Flock SDK's analytics under the playtest
	 * category. Recorded only while GetStatus() is Ready and the playtest turns heavy analytics on; otherwise it returns
	 * false and records nothing, so the call is safe in every build. Also false for a name the plugin sends itself
	 * (FlockPlaytestEvents), and when the Flock SDK refuses the event: its analytics off (Analytics Enabled), consent
	 * withheld, an empty name, session_started, or a name longer than 200 characters. It never waits on the network. A
	 * call from another thread is handed to the game thread and answers true.
	 */
	bool RecordPlaytestEvent(const FString& EventName, const FFlockCommandData& Properties = FFlockCommandData());

	/**
	 * True while performance windows and level loads are being recorded: the status is Ready, the playtest turns heavy
	 * analytics on, and the Flock SDK's analytics is on. Reading it changes nothing.
	 */
	bool IsMeasuringPerformance() const { return PerformancePump.IsRunning(); }

	/** True while a video recording is capturing the screen. Reading it changes nothing. */
	bool IsRecordingVideo() const;

	/**
	 * Stops this launch's video recording for good, for a game that quits on its own schedule. The file is finished on a
	 * worker thread, and no other recording starts this launch. Returns false, and changes nothing, when no recording is
	 * capturing.
	 */
	bool StopVideoRecording();

	/**
	 * Whether this launch's recording has somewhere to go: one is running, it belongs to the playtest rather than being
	 * a test video, and a Protokite session has started for it to be uploaded to.
	 *
	 * **A recording with no session cannot be uploaded at all** -- the next launch deletes it -- so anything offering
	 * the player a way to send it asks this first. Offering it on "a recording is running" alone puts a button in front
	 * of a player that stops their recording and sends nothing.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest")
	bool CanSendTheRecording() const;

	/**
	 * Stops this launch's recording and uploads it straight away, while the player is still in the game -- what the
	 * feedback form's "upload what I recorded" button calls.
	 *
	 * **Opening the form does not do this on its own** (owner, 2026-09-16): capture runs until the player asks for it to
	 * stop, or a length or size limit ends it. Returns false, and changes nothing, when no recording is capturing.
	 *
	 * Uploading is not waited for. A recording that does not make it stays on disk and a later launch pushes it, so
	 * nothing is lost by the game closing meanwhile.
	 */
	bool StopVideoRecordingAndUploadIt();

	/**
	 * Whether there is a feedback form to show: the playtest is ready and published one.
	 *
	 * A playtest with no form offers nothing to open, and a game reads this to leave its "give feedback" entry out
	 * rather than offering something that does nothing.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest")
	bool CanOpenFeedbackForm() const;

	/**
	 * Opens the playtest's feedback form over the game. Returns false, and changes nothing, when there is no form to
	 * show or one is already open.
	 *
	 * While it is open the mouse is shown and input goes to the form; closing or sending it puts both back exactly as
	 * they were, so a game that never used a cursor does not end up with one.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Playtest")
	bool OpenFeedbackForm();

	/** Closes the form without sending it. Returns false when none is open. */
	UFUNCTION(BlueprintCallable, Category = "Flock|Playtest")
	bool CloseFeedbackForm();

	UFUNCTION(BlueprintPure, Category = "Flock|Playtest")
	bool IsFeedbackFormOpen() const { return FormWidget.IsValid(); }

	/**
	 * Sends a filled-in form, keeping it on disk when it cannot go now.
	 *
	 * The form calls this once its answers are ones the server would take, and **a game with a form of its own calls it
	 * too** -- the answers are the whole contract, so a studio that would rather draw its own questions still gets the
	 * checking, the keeping and the sending.
	 *
	 * The player is never made to wait: anything that does not get through is sent by a later launch.
	 */
	void SendFilledInForm(const FFlockPlaytestFormAnswers& Answers);

	/** The finished file of this launch's video recording. Empty until one has been saved. */
	const FString& GetFinishedVideoRecordingPath() const { return FinishedVideoRecordingPath; }

	/**
	 * Records the screen for Seconds with no playtest needed, to try video recording out. Saved like any recording, and
	 * never uploaded. Returns false, and logs why, when a video is already recording or was already recorded this launch,
	 * when the playtest records video itself, or when video cannot be recorded here. A Shipping build always returns false
	 * and logs nothing. It starts as soon as the game instance has a viewport to record.
	 */
	bool StartTestVideoRecording(double Seconds);

	/**
	 * Starts following a Flock subsystem exactly as Initialize does, for tests that build both subsystems by
	 * hand. Call it once per subsystem.
	 */
	void FollowFlockLifecycleForTesting(UFlockSubsystem* InFlock) { FollowFlockLifecycle(InFlock); }

	/** The Flock subsystem this one follows; null before it starts following one and after it shuts down. */
	UFlockSubsystem* GetFollowedFlockForTesting() const;

	/** Sends Protokite requests through the given transport instead of the engine's HTTP module. Call before following. */
	void SetHttpAdapterForTesting(const TSharedPtr<IFlockHttpAdapter>& InAdapter) { TestHttpAdapter = InAdapter; }

	/** Uses the given retry policy instead of the Flock SDK's HTTP settings. Call before following. */
	void SetRetryPolicyForTesting(const FFlockRetryPolicy& InPolicy) { TestRetryPolicy = InPolicy; }

	/** Reads the Steam account through Reader instead of the engine's Steam subsystem. Call before following. */
	void SetSteamAccountReaderForTesting(TFunction<FFlockRunningSteamAccount()> Reader) { TestSteamAccountReader = MoveTemp(Reader); }

	/** Keeps the device id in the given file instead of the default one under Saved. Call before following. */
	void SetDeviceIdFilePathForTesting(const FString& Path) { TestDeviceIdFilePath = Path; }

	/** Hands the performance timeline one frame through the same ticker path the engine drives, background pause included. */
	void TickPerformanceTimelineForTesting(float FrameSeconds) { PerformancePump.TickForTesting(FrameSeconds); }

	/** Sends the game to the background, or brings it back, the way the engine announces it: to every ticker it follows. */
	void SetBackgroundedForTesting(bool bBackgrounded)
	{
		PerformancePump.SetBackgroundedForTesting(bBackgrounded);
		VideoPump.SetBackgroundedForTesting(bBackgrounded);
	}

	/** Hands over the engine's announcement that a map load started, as it passes it: the loading world's context. */
	void HandlePreLoadMapForTesting(const FWorldContext& WorldContext, const FString& MapName) { HandlePreLoadMap(WorldContext, MapName); }

	/** Hands over the engine's announcement that a map load finished: the loaded world, or none when the load failed. */
	void HandlePostLoadMapForTesting(UWorld* LoadedWorld) { HandlePostLoadMap(LoadedWorld); }

	/** Reads the engine frame number through Reader instead of the engine's frame counter. Call before following. */
	void SetEngineFrameNumberReaderForTesting(TFunction<uint64()> Reader) { TestEngineFrameNumberReader = MoveTemp(Reader); }

	/** The engine frame number the performance timeline is handed: the engine's frame counter, unless a test gave a reader. */
	uint64 GetEngineFrameNumberForTesting() const { return GetEngineFrameNumber(); }

	/**
	 * Takes video frames from Factory instead of the game viewport. Factory is handed the largest video size and returns
	 * a source, or none: with OutWhyNot empty to be asked again on the next frame, or saying why video can never be
	 * recorded. Call before following.
	 */
	void SetVideoFrameSourceFactoryForTesting(TFunction<TSharedPtr<IFlockPlaytestVideoFrameSource>(FIntPoint MaxVideoSize, FString& OutWhyNot)> Factory)
	{
		TestVideoFrameSourceFactory = MoveTemp(Factory);
	}

	/**
	 * Keeps video recordings in Folder instead of Saved/FlockPlaytest/Recordings, and finishes what ended runs left there
	 * when following starts. Call before following.
	 */
	void SetVideoRecordingFolderForTesting(const FString& Folder) { TestVideoRecordingFolder = Folder; }

	/** Runs Hook on the worker thread before each video frame is encoded, so a test can hold the worker. Call before following. */
	void SetBeforeEachVideoEncodeForTesting(TFunction<void()> Hook) { TestBeforeEachVideoEncode = MoveTemp(Hook); }

	/**
	 * Runs Hook on the writing thread before each video frame is written. Returning false makes that write fail, the way a
	 * full disk does. Call before following.
	 */
	void SetBeforeEachVideoWriteForTesting(TFunction<bool()> Hook) { TestBeforeEachVideoWrite = MoveTemp(Hook); }

	/** Runs Hook on the worker thread before it goes through what earlier launches left in the recordings folder. Call before following. */
	void SetBeforeFinishingWhatEndedRunsLeftForTesting(TFunction<void()> Hook) { TestBeforeFinishingWhatEndedRunsLeft = MoveTemp(Hook); }

	/** Waits until what earlier launches left in the recordings folder has been gone through. */
	void WaitUntilRecordingsFolderFinishedForTesting();

	/** Keeps filled-in forms here instead of under Saved. Set before a form is sent. */
	void SetFormSpoolFolderForTesting(const FString& InFolder) { FormSpoolFolderForTesting = InFolder; }

	/** Sends recordings through this instead of the network. Set before a recording finishes. */
	void SetFileUploaderForTesting(const TSharedPtr<IFlockFileUploader>& InUploader) { TestFileUploader = InUploader; }

	/** Hands the video recording one frame through the same ticker path the engine drives, background pause included. */
	void TickVideoRecordingForTesting(float FrameSeconds) { VideoPump.TickForTesting(FrameSeconds); }

	/** Waits until the worker has finished what it was handed. The next video tick then applies a finished file. */
	void WaitUntilVideoWrittenForTesting();

	/** Whether the video recording's ticker is running. */
	bool IsVideoTickerRunningForTesting() const { return VideoPump.IsRunning(); }

private:
	/**
	 * Starts following the Flock SDK's initialize, shut-down and session events, and decides the status straight
	 * away, so a Flock SDK that initialized before this was called is seen without waiting for an event that has
	 * already fired.
	 */
	void FollowFlockLifecycle(UFlockSubsystem* InFlock);

	UFUNCTION()
	void HandleFlockLifecycleChanged();

	/**
	 * Asks Protokite again when a Flock session starts, but only when the last attempt could not reach it. A
	 * refusal would get the same answer twice, and a config already loaded or on its way needs nothing.
	 */
	UFUNCTION()
	void HandleFlockSessionStarted(const FString& SessionId);

	/** Remembers the current Flock initialization's first session to reach the server, and starts the Protokite session when it can. */
	UFUNCTION()
	void HandleFlockSessionRegistered(const FString& SessionId, const FString& ServerSessionId);

	/**
	 * Starts recording performance windows and level loads when they should run, and stops when they should not,
	 * dropping the window in progress. The one place they start or stop.
	 */
	void UpdatePerformanceTimeline();

	void HandlePerformanceFrame(float FrameSeconds);
	void HandleBackgroundChanged(bool bBackgrounded);
	void HandlePreLoadMap(const FWorldContext& WorldContext, const FString& MapName);
	void HandlePostLoadMap(UWorld* LoadedWorld);

	/** Notes when this game instance started loading a map, for the load time and to leave out the frame the load stretched. */
	void NoteLevelLoadStarted(const UGameInstance* LoadingGameInstance);

	/** Sends level_loaded for a map this game instance finished loading. Another game instance's load is ignored. */
	void NoteLevelLoaded(const UGameInstance* LoadingGameInstance, const FString& MapName);

	/**
	 * Sends one playtest event through the Flock SDK's analytics, under the playtest category, while heavy analytics is
	 * on. The one place playtest events are sent, and the one place heavy analytics is checked before sending.
	 */
	bool SendPlaytestEvent(const FString& EventName, const FFlockCommandData& Properties);

	/** The engine's frame counter, or the testing reader's answer. */
	uint64 GetEngineFrameNumber() const;

	/** Whether anything asks for a video recording now: the playtest, a test video, or Record Video In Play In Editor. */
	bool IsVideoRecordingWanted() const;

	/** Whether Record Video In Play In Editor is on and this game instance is playing in the editor. */
	bool IsRecordVideoInPlayInEditorOn() const;

	/** Whether a recording is wanted and may still start this launch: none has started, and nothing rules video out. */
	bool IsWaitingToStartVideoRecording() const;

	/**
	 * Starts the launch's video recording when it is wanted and none has started, and stops a capturing one that nothing
	 * wants any more. The one place a recording starts or stops, apart from its own limits and the calls that stop it.
	 */
	void UpdateVideoRecording();

	/** Starts the recording, or waits for the game viewport, or logs once why video cannot be recorded. */
	void StartVideoRecordingWhenPossible();

	/** The folder recordings are kept in: Saved/FlockPlaytest/Recordings, or the testing folder. */
	FString GetVideoRecordingsFolder() const;

	/**
	 * Starts finishing, keeping or deleting what the runs that ended before this one left in the recordings folder, on a
	 * worker thread, which logs what it did. Called when following starts.
	 */
	void FinishWhatEndedRunsLeftInRecordingsFolder();

	/** Sends every form kept from an earlier launch, once Flock has initialized. Runs whether or not a form is open. */
	void SendFormsKeptFromEarlierLaunches();

	/** Starts or stops watching for the form key, following whether there is a form to open. */
	void UpdateFeedbackFormKeyWatcher();

	/** The uploader, built on first use so a launch that never records never makes one. */
	TSharedRef<FFlockPlaytestRecordingUploads> GetOrCreateRecordingUploads();

	/** Sends this launch's own finished recording, which this launch still holds the run of. */
	void UploadThisLaunchsRecording();

	/**
	 * Pushes recordings earlier launches left, once Flock has initialized and the launch pass has finished with them.
	 *
	 * **This runs whether or not playtesting is on** (owner, 2026-09-16): a launch with playtesting switched off still
	 * pushes what is waiting, and does nothing else -- it records nothing and starts no session. Otherwise turning
	 * playtesting off would strand every recording an earlier launch could not send.
	 */
	void StartUploadingWhatEarlierLaunchesLeft();

	/**
	 * Saves this launch's Protokite session beside its playtest recording once both exist, so a later launch can upload the
	 * recording to it. Does nothing for a test video or before the session has started.
	 */
	void SaveVideoRecordingSession();

	/** Runs the video ticker while a recording captures or finishes, or waits for a viewport; stops it otherwise. */
	void UpdateVideoPump();

	void HandleVideoFrame(float FrameSeconds);
	void HandleVideoBackgroundChanged(bool bBackgrounded);

	/** Logs what became of the finished recording and forgets it. The one writer of FinishedVideoRecordingPath. */
	void ApplyFinishedVideoRecording();

	/** Stops a recording and waits for its file, for a subsystem shutting down. */
	void FinishVideoRecordingNow(EFlockPlaytestVideoStopReason Reason);

	/**
	 * Forgets a config that belongs to a Flock initialization which has ended, starts a fetch when the status
	 * calls for one, applies the status, and starts the Protokite session when it can.
	 */
	void RefreshStatus();

	/** The status the current settings, Flock SDK and config state add up to. Changes nothing. */
	EFlockPlaytestStatus DecideCurrentStatus() const;

	/** The Protokite client, created on first use with the Flock SDK's timeout and retry settings. */
	TSharedRef<FFlockProtokiteClient> GetOrCreateProtokiteClient();

	/** Sends the playtest-config request for the current Flock initialization. */
	void StartPlaytestConfigFetch();

	/**
	 * Drops the config, stops a request that is still being sent or retried, and makes any reply still on its
	 * way stale. The one place a config is forgotten.
	 */
	void ForgetPlaytestConfig();

	/**
	 * Sends this launch's session start once the status is Ready and a Flock session has reached the server, and
	 * does nothing otherwise. The one place a start is sent, and it sends one at most per launch.
	 */
	void StartPlaytestSessionWhenAllowed();

	/**
	 * The Steam id when a Steam subsystem is running, otherwise this install's device id. When neither can be had,
	 * the identity is empty and OutWhyNone says why.
	 */
	FFlockPlaytestIdentity ResolvePlaytestIdentity(FString& OutWhyNone) const;

	/** The only writer of Status. Logs when the status changes. */
	void ApplyStatus(EFlockPlaytestStatus NewStatus, const FString& ProtokiteApiUrl, const FString& GameVersionId);

	TWeakObjectPtr<UFlockSubsystem> Flock;

	EFlockPlaytestStatus Status = EFlockPlaytestStatus::TurnedOff;

	/** False until the first decision, so that decision is logged even when it matches the initial value. */
	bool bStatusDecided = false;

	FFlockPlaytestConfig PlaytestConfig;
	EFlockPlaytestConfigState ConfigState = EFlockPlaytestConfigState::NotFetched;

	/** Why the last fetch did not load a config, for the warning that reports it; empty otherwise. */
	FString ConfigFailureMessage;

	/** How many times a config has been forgotten. A reply to a request sent before the latest time is ignored. */
	int32 TimesConfigForgotten = 0;

	/** The playtest-config request for the current Flock initialization, kept so forgetting the config can stop it. */
	FFlockRequestHandle ConfigFetchRequest;

	/** Set when Protokite refused the session because the playtest has closed. Keeps playtesting off for the launch. */
	bool bPlaytestNoLongerCollecting = false;

	/** The server id of the current Flock initialization's first session to reach the server; empty until one has. */
	FString FirstFlockServerSessionId;

	EFlockPlaytestSessionState SessionState = EFlockPlaytestSessionState::NotStarted;
	FString PlaytestSessionId;
	FFlockPlaytestIdentity PlaytestIdentity;

	/** The Protokite API URL and headers the session start used. The end is sent with the same ones. */
	FString PlaytestSessionApiUrl;
	TMap<FString, FString> PlaytestSessionHeaders;

	/** Whether waiting for a Flock session has been logged this launch. */
	bool bLoggedWaitingForFlockSession = false;

	/** Frames for the performance timeline. Its own ticker, which stops while the game is in the background. */
	FFlockLifecyclePump PerformancePump;
	FFlockPlaytestPerformanceTimeline PerformanceTimeline;
	FDelegateHandle PerformanceFrameHandle;
	FDelegateHandle BackgroundChangedHandle;
	FDelegateHandle PreLoadMapHandle;
	FDelegateHandle PostLoadMapHandle;

	/** The map this game instance is on, as the last level load, or the start of recording, found it. */
	FString CurrentMapName;

	/** When this game instance's level load started, in platform seconds; negative when none is under way. */
	double LevelLoadStartSeconds = -1.0;

	/** Whether heavy analytics being on while the Flock SDK's analytics is off has been logged this launch. */
	bool bLoggedHeavyAnalyticsWithoutFlockAnalytics = false;

	TSharedPtr<FFlockProtokiteClient> ProtokiteClient;
	TSharedPtr<IFlockHttpAdapter> TestHttpAdapter;
	TOptional<FFlockRetryPolicy> TestRetryPolicy;
	TFunction<FFlockRunningSteamAccount()> TestSteamAccountReader;
	FString TestDeviceIdFilePath;
	TFunction<uint64()> TestEngineFrameNumberReader;

	/** Frames for the video recording. Its own ticker, so video runs whether or not heavy analytics does. */
	FFlockLifecyclePump VideoPump;
	FDelegateHandle VideoFrameHandle;
	FDelegateHandle VideoBackgroundChangedHandle;

	/** The recording capturing, or finishing its file. Null otherwise. */
	TSharedPtr<FFlockPlaytestVideoRecording> VideoRecording;

	/**
	 * This launch's hold on the folder its recording is in, from the moment the recording starts until the game instance
	 * shuts down: a finished recording stays this launch's until then, so no other launch touches it.
	 */
	TSharedPtr<FFlockPlaytestRecordingRun> VideoRecordingRun;

	/** Going through what earlier launches left in the recordings folder, on its worker thread. */
	TFuture<void> RecordingsFolderFinished;

	/** Set once a recording has been started this launch, whatever became of it. */
	bool bVideoRecordingStartedThisLaunch = false;

	/** Set once this launch has begun sending forms earlier launches kept, so it is begun exactly once. */
	bool bStartedSendingKeptForms = false;

	/** Where filled-in forms wait when they cannot be sent; a test may point this somewhere of its own. */
	TOptional<FString> FormSpoolFolderForTesting;

	/** The feedback form while it is open; null when it is not. */
	TSharedPtr<SFlockPlaytestFormWidget> FormWidget;

	/** Watches for the key that opens the form. Only alive while there is a form to open. */
	TSharedPtr<FFlockPlaytestFormKeyWatcher> FormKeyWatcher;

	/** Whether the mouse was already being shown before the form opened, so closing it can put that back. */
	bool bCursorWasShownBeforeTheForm = false;

	/** Whether this subsystem is the one that paused the game, so it only unpauses a pause of its own. */
	bool bPausedForTheForm = false;

	/** Sends finished recordings to Protokite. Built when the first one is ready to go. */
	TSharedPtr<FFlockPlaytestRecordingUploads> RecordingUploads;

	/** Stands in for the network in tests. */
	TSharedPtr<IFlockFileUploader> TestFileUploader;

	/**
	 * Set as teardown starts, before anything is stopped. Uploading reads it: a whole recording cannot be sent inside a
	 * shutdown, so one finished by teardown is left for a later launch instead of started and abandoned.
	 */
	bool bDeinitializing = false;

	/** Set once this launch has begun pushing what earlier launches left, so it is begun exactly once. */
	bool bStartedUploadingWhatEarlierLaunchesLeft = false;

	/** Waits for Flock to initialize and the launch pass to finish before pushing what earlier launches left. */
	FTSTicker::FDelegateHandle WaitingToUploadEarlierRecordings;

	/** Why video can never be recorded in this process, once found; empty otherwise. */
	FString VideoRecordingUnavailableReason;

	FString FinishedVideoRecordingPath;

	/** Set once StartTestVideoRecording has asked for a test video this launch, and how long it may be. Only it writes these. */
	bool bTestVideoRequested = false;
	double TestVideoSeconds = 0.0;

	TFunction<TSharedPtr<IFlockPlaytestVideoFrameSource>(FIntPoint, FString&)> TestVideoFrameSourceFactory;
	FString TestVideoRecordingFolder;
	TFunction<void()> TestBeforeEachVideoEncode;
	TFunction<bool()> TestBeforeEachVideoWrite;
	TFunction<void()> TestBeforeFinishingWhatEndedRunsLeft;
};
