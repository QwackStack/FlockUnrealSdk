// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS && WITH_FLOCK_PLAYTEST_VIDEO

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "FlockPlaytestSubsystem.h"
#include "FlockPlaytestVideoFrameSource.h"
#include "FlockPlaytestVideoRecording.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Tests/FlockPlaytestVideoTestSupport.h"
#include "UnrealClient.h"

using namespace FlockPlaytestVideoTesting;

namespace
{
	/** Waits, while the engine draws its frames, for the playtest subsystem to save its video, then checks the file. */
	class FCheckRecordedGameViewportCommand : public IAutomationLatentCommand
	{
	public:
		FCheckRecordedGameViewportCommand(FAutomationTestBase* InTest, UFlockPlaytestSubsystem* InPlaytest)
			: Test(InTest)
			, Playtest(InPlaytest)
			, StartSeconds(FPlatformTime::Seconds())
		{
		}

		virtual bool Update() override
		{
			const UFlockPlaytestSubsystem* Subsystem = Playtest.Get();
			if (Subsystem == nullptr)
			{
				Test->AddError(TEXT("The playtest subsystem went away while recording"));
				return true;
			}
			const FString Path = Subsystem->GetFinishedVideoRecordingPath();
			if (Path.IsEmpty())
			{
				if (FPlatformTime::Seconds() - StartSeconds > 60.0)
				{
					Test->AddError(TEXT("No video was saved within a minute"));
					return true;
				}
				return false;
			}

			FVideoFileRead File;
			if (Test->TestTrue(TEXT("The saved file reads back"), ReadVideoFile(Path, File)))
			{
				Test->TestTrue(FString::Printf(TEXT("Frames were recorded (%d)"), File.Frames.Num()), File.Frames.Num() >= 10);
				Test->TestTrue(FString::Printf(TEXT("The video, %dx%d, fits inside 1280 by 720 with even sides"), File.Width, File.Height),
					File.Width > 0 && File.Width <= 1280 && File.Height > 0 && File.Height <= 720 && File.Width % 2 == 0 && File.Height % 2 == 0);
				const int64 LastTimestampMs = File.Frames.Num() > 0 ? File.Frames.Last().TimestampMs : -1;
				// A slow machine draws fewer frames, so the last one can start well before the two seconds are up.
				Test->TestTrue(FString::Printf(TEXT("About two seconds long (last frame at %lld ms)"), LastTimestampMs),
					LastTimestampMs >= 1500 && LastTimestampMs < 2000);

				FIntPoint PictureSize;
				double Brightness = -1.0;
				Test->TestEqual(TEXT("Every frame decodes"), DecodeVideoFile(File, PictureSize, Brightness), File.Frames.Num());
				Test->TestTrue(FString::Printf(TEXT("The picture is not blank (average brightness %.1f)"), Brightness), Brightness > 17.0);
			}
			Test->TestTrue(FString::Printf(TEXT("Saved as a test video under the project's recordings folder (%s)"), *Path),
				FPaths::IsUnderDirectory(Path, FFlockPlaytestRecordingsFolder::GetKindFolder(FFlockPlaytestRecordingsFolder::GetDefaultPath(),
					EFlockPlaytestRecordingKind::TestVideo)));
			IFileManager::Get().Delete(*Path, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
			return true;
		}

	private:
		FAutomationTestBase* Test;
		TWeakObjectPtr<UFlockPlaytestSubsystem> Playtest;
		double StartSeconds;
	};

	void SetFrameGrabberLatency(int32 Latency)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("framegrabber.framelatency")))
		{
			Variable->Set(Latency, ECVF_SetByCode);
		}
	}

	/**
	 * Records the game viewport for three seconds while the engine runs: the window is resized after one second and the
	 * frame grabber latency set after two. Then the file is checked, and the window size and latency are put back.
	 */
	class FRecordThroughAResizeCommand : public IAutomationLatentCommand
	{
	public:
		FRecordThroughAResizeCommand(FAutomationTestBase* InTest, UWorld* InWorld, const TSharedPtr<FFlockPlaytestVideoRecording>& InRecording,
			const FString& InFolder, FIntPoint InViewportSizeAtStart)
			: Test(InTest)
			, World(InWorld)
			, Recording(InRecording)
			, Folder(InFolder)
			, ViewportSizeAtStart(InViewportSizeAtStart)
			, StartSeconds(FPlatformTime::Seconds())
		{
		}

		virtual bool Update() override
		{
			const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;
			// For ten frames before the latency changes no frame is asked for, so each one asked for earlier has reached the
			// grabber. When the latency changes while a frame is on its way, the engine's grabber flushes rendering commands from
			// the render thread, which stops the engine on a check: measured in this test, three runs out of three.
			const bool bLettingFramesArrive = !bLatencySet && ElapsedSeconds > 2.0;
			if (!bLettingFramesArrive)
			{
				Recording->AddFrame(FApp::GetDeltaTime());
			}
			if (!bResized && ElapsedSeconds > 1.0)
			{
				bResized = true;
				GEngine->Exec(World.Get(), TEXT("r.SetRes 960x540w"));
			}
			if (bLettingFramesArrive && ++FramesLetArrive >= 10)
			{
				bLatencySet = true;
				NotReadyBeforeLatency = Recording->GetSummary().FramesNotReadyInTime;
				SetFrameGrabberLatency(1);
			}
			if (ElapsedSeconds < 3.0)
			{
				return false;
			}

			const FIntPoint ViewportSizeAtStop = GEngine->GameViewport != nullptr && GEngine->GameViewport->Viewport != nullptr
				? GEngine->GameViewport->Viewport->GetSizeXY() : FIntPoint::ZeroValue;
			// With the latency set, stopping returns only because no frame was asked for meanwhile.
			Recording->StopCapturing(EFlockPlaytestVideoStopReason::StoppedByGame);
			Recording->WaitUntilWritten();
			SetFrameGrabberLatency(0);
			GEngine->Exec(World.Get(), TEXT("r.SetRes 1280x720w"));

			Test->TestNotEqual(TEXT("Precondition: the viewport changed size"), ViewportSizeAtStop, ViewportSizeAtStart);
			const FFlockPlaytestVideoRecordingSummary Summary = Recording->GetSummary();
			Test->TestTrue(FString::Printf(TEXT("The file was written (error: '%s')"), *Summary.Error),
				Summary.Error.IsEmpty() && !Summary.FilePath.IsEmpty());
			Test->TestTrue(FString::Printf(TEXT("With the latency set, frames stopped being asked for (%d capture times passed, %d before it)"),
				Summary.FramesNotReadyInTime, NotReadyBeforeLatency), Summary.FramesNotReadyInTime - NotReadyBeforeLatency >= 10);

			FVideoFileRead File;
			if (Test->TestTrue(TEXT("The file reads back"), ReadVideoFile(Summary.FilePath, File)))
			{
				int32 FramesAfterResize = 0;
				int32 FramesWithLatencySet = 0;
				for (const FVideoFileFrame& Frame : File.Frames)
				{
					FramesAfterResize += Frame.TimestampMs > 1300 && Frame.TimestampMs < 2000 ? 1 : 0;
					FramesWithLatencySet += Frame.TimestampMs > 2100 ? 1 : 0;
				}
				Test->TestTrue(FString::Printf(TEXT("Frames kept coming after the resize (%d)"), FramesAfterResize), FramesAfterResize >= 5);
				Test->TestTrue(FString::Printf(TEXT("And stopped while the latency was set (%d)"), FramesWithLatencySet), FramesWithLatencySet <= 2);
				FIntPoint PictureSize;
				double Brightness = -1.0;
				Test->TestEqual(TEXT("Every frame decodes"), DecodeVideoFile(File, PictureSize, Brightness), File.Frames.Num());
			}
			IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
			return true;
		}

	private:
		FAutomationTestBase* Test;
		TWeakObjectPtr<UWorld> World;
		TSharedPtr<FFlockPlaytestVideoRecording> Recording;
		FString Folder;
		FIntPoint ViewportSizeAtStart;
		double StartSeconds;
		bool bResized = false;
		bool bLatencySet = false;
		int32 FramesLetArrive = 0;
		int32 NotReadyBeforeLatency = 0;
	};
}

// Needs a real renderer and a game viewport, like the test below.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoKeepsRecordingThroughAWindowResizeTest,
	"Flock.Playtest.Video.KeepsRecordingThroughAWindowResize",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoKeepsRecordingThroughAWindowResizeTest::RunTest(const FString& Parameters)
{
	UWorld* World = GEngine != nullptr && GEngine->GameViewport != nullptr ? GEngine->GameViewport->GetWorld() : nullptr;
	UGameInstance* GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;
	if (!TestNotNull(TEXT("A running game instance"), GameInstance) || GEngine->GameViewport->Viewport == nullptr)
	{
		return true;
	}
	FString WhyNot;
	const TSharedPtr<IFlockPlaytestVideoFrameSource> Source = FFlockPlaytestGameViewportFrameSource::Create(GameInstance, FIntPoint(1280, 720), WhyNot);
	if (!TestTrue(FString::Printf(TEXT("The game viewport can be recorded ('%s')"), *WhyNot), Source.IsValid()))
	{
		return true;
	}
	const FString Folder = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockPlaytestTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FString Error;
	const TSharedPtr<FFlockPlaytestVideoRecording> Recording = FFlockPlaytestVideoRecording::Start(Source.ToSharedRef(), FFlockPlaytestVideoSettings(),
		FPaths::Combine(Folder, TEXT("resize.webm")), Error);
	if (!TestTrue(FString::Printf(TEXT("The recording starts ('%s')"), *Error), Recording.IsValid()))
	{
		return true;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FRecordThroughAResizeCommand(this, World, Recording, Folder, GEngine->GameViewport->Viewport->GetSizeXY()));
	return true;
}

// Needs a real renderer and a game viewport: run it in a -game session without -nullrhi. The engine leaves it out of a
// -nullrhi run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoRecordsTheGameViewportTest,
	"Flock.Playtest.Video.RecordsTheGameViewport",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoRecordsTheGameViewportTest::RunTest(const FString& Parameters)
{
	UWorld* World = GEngine != nullptr && GEngine->GameViewport != nullptr ? GEngine->GameViewport->GetWorld() : nullptr;
	UGameInstance* GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;
	UFlockPlaytestSubsystem* Playtest = GameInstance != nullptr ? GameInstance->GetSubsystem<UFlockPlaytestSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("The running game instance has a playtest subsystem"), Playtest))
	{
		return true;
	}

	// Through the console command, the way a developer starts one.
	TestTrue(TEXT("The console command is known"), GEngine->Exec(World, TEXT("FlockPlaytest.RecordTestVideo 2")));
	TestTrue(TEXT("It records the game viewport"), Playtest->IsRecordingVideo());
	ADD_LATENT_AUTOMATION_COMMAND(FCheckRecordedGameViewportCommand(this, Playtest));
	return true;
}

#endif
