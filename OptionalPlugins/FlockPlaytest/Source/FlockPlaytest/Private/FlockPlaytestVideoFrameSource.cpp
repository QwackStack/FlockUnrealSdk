// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestVideoFrameSource.h"

#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "FlockPlaytestVideoEncoder.h"
#include "FlockPlaytestVideoFrameSchedule.h"
#include "Framework/Application/SlateApplication.h"
#include "FrameGrabber.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "RenderingThread.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"

namespace
{
	/**
	 * How many frames may be on their way at once. Each frame the window presents fills the oldest request, so requests
	 * the window never presents (a minimised window) would pile up and hand later frames old times.
	 */
	constexpr int32 MaxFramesOnTheirWay = 2;

	struct FTimestampPayload : public IFramePayload
	{
		explicit FTimestampPayload(int64 InTimestampMs) : TimestampMs(InTimestampMs) {}
		int64 TimestampMs;
	};

	int32 ReadFrameGrabberLatency()
	{
		const IConsoleVariable* Latency = IConsoleManager::Get().FindConsoleVariable(TEXT("framegrabber.framelatency"));
		return Latency != nullptr ? Latency->GetInt() : 0;
	}

	/** The shared viewport the game instance draws into, or null while it has none. */
	TSharedPtr<FSceneViewport> FindGameViewport(const UGameInstance* GameInstance)
	{
		UGameViewportClient* Client = GameInstance != nullptr ? GameInstance->GetGameViewportClient() : nullptr;
		FSceneViewport* RawViewport = Client != nullptr ? Client->GetGameViewport() : nullptr;
		if (RawViewport == nullptr)
		{
			return nullptr;
		}
		// The client holds a plain pointer; the viewport widget holds the shared one, which the frame grabber takes.
		const TSharedPtr<SViewport> Widget = RawViewport->GetViewportWidget().Pin();
		const TSharedPtr<ISlateViewport> Shared = Widget.IsValid() ? Widget->GetViewportInterface().Pin() : nullptr;
		if (!Shared.IsValid() || Shared.Get() != static_cast<ISlateViewport*>(RawViewport))
		{
			return nullptr;
		}
		return StaticCastSharedPtr<FSceneViewport>(Shared);
	}

	void MoveFrames(TArray<FCapturedFrameData>&& Captured, TArray<FFlockPlaytestVideoFrame>& OutFrames)
	{
		for (FCapturedFrameData& Data : Captured)
		{
			const FTimestampPayload* Payload = Data.GetPayload<FTimestampPayload>();
			if (Payload == nullptr)
			{
				continue;
			}
			FFlockPlaytestVideoFrame& Frame = OutFrames.AddDefaulted_GetRef();
			Frame.Pixels = MoveTemp(Data.ColorBuffer);
			Frame.Size = Data.BufferSize;
			Frame.TimestampMs = Payload->TimestampMs;
		}
	}
}

TSharedPtr<IFlockPlaytestVideoFrameSource> FFlockPlaytestGameViewportFrameSource::Create(const UGameInstance* GameInstance,
	FIntPoint MaxVideoSize, FString& OutWhyNot)
{
	OutWhyNot = DecideWhyVideoCannotBeRecorded(FFlockPlaytestVideoEncoder::IsBuiltWithVideo(), FApp::CanEverRender(),
		ReadFrameGrabberLatency());
	if (!OutWhyNot.IsEmpty() || !FSlateApplication::IsInitialized())
	{
		return nullptr;
	}
	const TSharedPtr<FSceneViewport> Viewport = FindGameViewport(GameInstance);
	if (!Viewport.IsValid())
	{
		return nullptr;
	}
	const FIntPoint FrameSize = FitVideoSizeInside(Viewport->GetSizeXY(), MaxVideoSize);
	if (FrameSize.X <= 0 || FrameSize.Y <= 0)
	{
		return nullptr;
	}
	return MakeShareable(new FFlockPlaytestGameViewportFrameSource(Viewport.ToSharedRef(), FrameSize));
}

FFlockPlaytestGameViewportFrameSource::FFlockPlaytestGameViewportFrameSource(const TSharedRef<FSceneViewport>& InViewport, FIntPoint InFrameSize)
	: Viewport(InViewport)
	, FrameSize(InFrameSize)
{
	StartGrabber(InViewport);
}

FFlockPlaytestGameViewportFrameSource::~FFlockPlaytestGameViewportFrameSource()
{
	TArray<FFlockPlaytestVideoFrame> Dropped;
	Stop(Dropped);
}

bool FFlockPlaytestGameViewportFrameSource::IsReadyForAnotherFrame() const
{
	// A latency set while recording would leave a readback waiting that stopping then blocks on forever, so no frame is
	// asked for until the console variable is back at 0.
	return !bStopped && Grabber.IsValid() && FramesOnTheirWay < MaxFramesOnTheirWay && ReadFrameGrabberLatency() == 0;
}

void FFlockPlaytestGameViewportFrameSource::CaptureFrame(int64 TimestampMs)
{
	if (!IsReadyForAnotherFrame())
	{
		return;
	}
	Grabber->CaptureThisFrame(MakeShared<FTimestampPayload, ESPMode::ThreadSafe>(TimestampMs));
	++FramesOnTheirWay;
}

void FFlockPlaytestGameViewportFrameSource::TakeCapturedFrames(TArray<FFlockPlaytestVideoFrame>& OutFrames)
{
	if (bStopped || !Grabber.IsValid())
	{
		return;
	}
	TArray<FCapturedFrameData> Captured = Grabber->GetCapturedFrames();
	FramesOnTheirWay = FMath::Max(0, FramesOnTheirWay - Captured.Num());
	MoveFrames(MoveTemp(Captured), OutFrames);

	// A readback the engine gave up on is never handed over, so the grabber's own answer wins over the count.
	if (!Grabber->HasOutstandingFrames())
	{
		FramesOnTheirWay = 0;
	}

	// The grabber reads the part of the window the viewport had when it started. After a resize a new one reads the new
	// part, at the same video size, since a video keeps one size throughout.
	const TSharedPtr<FSceneViewport> Pinned = Viewport.Pin();
	if (Pinned.IsValid())
	{
		const FIntPoint ViewportSize = Pinned->GetSizeXY();
		if (ViewportSize != ViewportSizeWhenGrabberStarted && ViewportSize.X > 0 && ViewportSize.Y > 0)
		{
			StopGrabber(OutFrames);
			StartGrabber(Pinned.ToSharedRef());
		}
	}
}

void FFlockPlaytestGameViewportFrameSource::Stop(TArray<FFlockPlaytestVideoFrame>& OutFrames)
{
	if (bStopped)
	{
		return;
	}
	bStopped = true;
	StopGrabber(OutFrames);
}

void FFlockPlaytestGameViewportFrameSource::StartGrabber(const TSharedRef<FSceneViewport>& InViewport)
{
	Grabber = MakeUnique<FFrameGrabber>(InViewport, FrameSize, PF_B8G8R8A8, /*NumSurfaces*/ 3);
	Grabber->StartCapturingFrames();
	ViewportSizeWhenGrabberStarted = InViewport->GetSizeXY();
	FramesOnTheirWay = 0;
}

void FFlockPlaytestGameViewportFrameSource::StopGrabber(TArray<FFlockPlaytestVideoFrame>& OutFrames)
{
	if (!Grabber.IsValid())
	{
		return;
	}
	if (Grabber->IsCapturingFrames())
	{
		Grabber->StopCapturingFrames();
	}
	// Readbacks already sent to the render thread finish here, so no surface is still waiting when the grabber shuts
	// down, and the render thread is not presenting while its callback is removed.
	FlushRenderingCommands();
	MoveFrames(Grabber->GetCapturedFrames(), OutFrames);
	Grabber->Shutdown();
	Grabber.Reset();
	FramesOnTheirWay = 0;
}
