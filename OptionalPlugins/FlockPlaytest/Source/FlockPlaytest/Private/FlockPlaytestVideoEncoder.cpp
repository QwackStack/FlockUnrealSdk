// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestVideoEncoder.h"

#include "HAL/PlatformMisc.h"

#if WITH_FLOCK_PLAYTEST_VIDEO
THIRD_PARTY_INCLUDES_START
#include "vpx/vp8cx.h"
#include "vpx/vpx_encoder.h"
THIRD_PARTY_INCLUDES_END
#endif

#if WITH_FLOCK_PLAYTEST_VIDEO
struct FFlockPlaytestVideoEncoder::FLibVpxState
{
	vpx_codec_ctx_t Codec;
	vpx_image_t Image;
	TArray<uint8> Planes;
	bool bInitialized = false;
};
#else
struct FFlockPlaytestVideoEncoder::FLibVpxState
{
};
#endif

namespace
{
	int32 HalfRoundedUp(int32 Value)
	{
		return (Value + 1) / 2;
	}

#if WITH_FLOCK_PLAYTEST_VIDEO
	void CollectEncodedFrames(vpx_codec_ctx_t& Codec, TArray<FFlockPlaytestEncodedFrame>& OutFrames)
	{
		vpx_codec_iter_t Iterator = nullptr;
		while (const vpx_codec_cx_pkt_t* Packet = vpx_codec_get_cx_data(&Codec, &Iterator))
		{
			if (Packet->kind != VPX_CODEC_CX_FRAME_PKT)
			{
				continue;
			}
			FFlockPlaytestEncodedFrame& Frame = OutFrames.AddDefaulted_GetRef();
			Frame.Bytes.Append(static_cast<const uint8*>(Packet->data.frame.buf), static_cast<int32>(Packet->data.frame.sz));
			Frame.TimestampMs = Packet->data.frame.pts;
			Frame.bKeyFrame = (Packet->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
		}
	}
#endif
}

FFlockPlaytestVideoEncoder::FFlockPlaytestVideoEncoder()
	: State(MakeUnique<FLibVpxState>())
{
}

FFlockPlaytestVideoEncoder::~FFlockPlaytestVideoEncoder()
{
#if WITH_FLOCK_PLAYTEST_VIDEO
	if (State->bInitialized)
	{
		vpx_codec_destroy(&State->Codec);
	}
#endif
}

bool FFlockPlaytestVideoEncoder::IsBuiltWithVideo()
{
	return WITH_FLOCK_PLAYTEST_VIDEO != 0;
}

bool FFlockPlaytestVideoEncoder::Initialize(FIntPoint InFrameSize, int32 FramesPerSecond, int32 BitrateKbps, FString& OutError)
{
#if WITH_FLOCK_PLAYTEST_VIDEO
	if (State->bInitialized)
	{
		OutError = TEXT("the encoder is already set up");
		return false;
	}
	if (InFrameSize.X <= 0 || InFrameSize.Y <= 0 || InFrameSize.X % 2 != 0 || InFrameSize.Y % 2 != 0)
	{
		OutError = FString::Printf(TEXT("a frame size of %dx%d cannot be encoded; each side must be even"), InFrameSize.X, InFrameSize.Y);
		return false;
	}

	vpx_codec_iface_t* Interface = vpx_codec_vp9_cx();
	vpx_codec_enc_cfg_t Config;
	vpx_codec_err_t Result = vpx_codec_enc_config_default(Interface, &Config, 0);
	if (Result != VPX_CODEC_OK)
	{
		OutError = FString::Printf(TEXT("libvpx gave no VP9 settings: %hs"), vpx_codec_err_to_string(Result));
		return false;
	}
	Config.g_w = static_cast<unsigned int>(InFrameSize.X);
	Config.g_h = static_cast<unsigned int>(InFrameSize.Y);
	Config.g_timebase.num = 1;
	Config.g_timebase.den = 1000;
	Config.g_pass = VPX_RC_ONE_PASS;
	Config.g_lag_in_frames = 0;
	Config.g_threads = static_cast<unsigned int>(FMath::Clamp(FPlatformMisc::NumberOfCores() / 2, 1, 4));
	Config.rc_end_usage = VPX_CBR;
	Config.rc_target_bitrate = static_cast<unsigned int>(BitrateKbps);
	// A key frame at least every ten seconds, so a player can start from the middle of the recording.
	Config.kf_mode = VPX_KF_AUTO;
	Config.kf_min_dist = 0;
	Config.kf_max_dist = static_cast<unsigned int>(FMath::Max(1, FramesPerSecond) * 10);

	Result = vpx_codec_enc_init(&State->Codec, Interface, &Config, 0);
	if (Result != VPX_CODEC_OK)
	{
		OutError = FString::Printf(TEXT("libvpx could not set up a VP9 encoder: %hs"), vpx_codec_err_to_string(Result));
		return false;
	}
	State->bInitialized = true;
	FrameSize = InFrameSize;

	// Fast enough to keep up with a game on one worker thread (measured in the playtesting spike at 720p).
	vpx_codec_control(&State->Codec, VP8E_SET_CPUUSED, 8);
	vpx_codec_control(&State->Codec, VP9E_SET_ROW_MT, 1);
	vpx_codec_control(&State->Codec, VP9E_SET_TILE_COLUMNS, 1);
	vpx_codec_control(&State->Codec, VP9E_SET_AQ_MODE, 3);
	return true;
#else
	OutError = TEXT("this build has no video encoder: video recording is built for 64-bit Windows only");
	return false;
#endif
}

bool FFlockPlaytestVideoEncoder::Encode(const TArray<FColor>& Pixels, int64 TimestampMs, int64 DurationMs,
	TArray<FFlockPlaytestEncodedFrame>& OutFrames, FString& OutError)
{
#if WITH_FLOCK_PLAYTEST_VIDEO
	if (!State->bInitialized)
	{
		OutError = TEXT("the encoder is not set up");
		return false;
	}
	if (Pixels.Num() != FrameSize.X * FrameSize.Y)
	{
		OutError = FString::Printf(TEXT("a frame of %d pixels does not match the video's %dx%d"), Pixels.Num(), FrameSize.X, FrameSize.Y);
		return false;
	}

	ConvertToVideoColours(Pixels, FrameSize, State->Planes);
	vpx_img_wrap(&State->Image, VPX_IMG_FMT_I420, static_cast<unsigned int>(FrameSize.X), static_cast<unsigned int>(FrameSize.Y),
		1, State->Planes.GetData());

	const vpx_codec_err_t Result = vpx_codec_encode(&State->Codec, &State->Image, TimestampMs,
		static_cast<unsigned long>(FMath::Max<int64>(1, DurationMs)), 0, VPX_DL_REALTIME);
	if (Result != VPX_CODEC_OK)
	{
		const char* Detail = vpx_codec_error_detail(&State->Codec);
		OutError = FString::Printf(TEXT("libvpx could not encode a frame: %hs %hs"), vpx_codec_error(&State->Codec), Detail != nullptr ? Detail : "");
		return false;
	}
	CollectEncodedFrames(State->Codec, OutFrames);
	return true;
#else
	OutError = TEXT("this build has no video encoder");
	return false;
#endif
}

bool FFlockPlaytestVideoEncoder::Finish(TArray<FFlockPlaytestEncodedFrame>& OutFrames, FString& OutError)
{
#if WITH_FLOCK_PLAYTEST_VIDEO
	if (!State->bInitialized)
	{
		OutError = TEXT("the encoder is not set up");
		return false;
	}
	const vpx_codec_err_t Result = vpx_codec_encode(&State->Codec, nullptr, -1, 1, 0, VPX_DL_REALTIME);
	if (Result != VPX_CODEC_OK)
	{
		OutError = FString::Printf(TEXT("libvpx could not finish the video: %hs"), vpx_codec_error(&State->Codec));
		return false;
	}
	CollectEncodedFrames(State->Codec, OutFrames);
	return true;
#else
	OutError = TEXT("this build has no video encoder");
	return false;
#endif
}

void FFlockPlaytestVideoEncoder::ConvertToVideoColours(const TArray<FColor>& Pixels, FIntPoint InFrameSize, TArray<uint8>& OutPlanes)
{
	const int32 Width = InFrameSize.X;
	const int32 Height = InFrameSize.Y;
	const int32 ColourWidth = HalfRoundedUp(Width);
	const int32 ColourHeight = HalfRoundedUp(Height);
	OutPlanes.SetNumUninitialized(Width * Height + 2 * ColourWidth * ColourHeight);
	if (Pixels.Num() < Width * Height)
	{
		FMemory::Memzero(OutPlanes.GetData(), OutPlanes.Num());
		return;
	}

	uint8* Brightness = OutPlanes.GetData();
	uint8* Blue = Brightness + Width * Height;
	uint8* Red = Blue + ColourWidth * ColourHeight;
	const FColor* Source = Pixels.GetData();

	for (int32 Row = 0; Row < Height; ++Row)
	{
		for (int32 Column = 0; Column < Width; ++Column)
		{
			const FColor& Pixel = Source[Row * Width + Column];
			Brightness[Row * Width + Column] = static_cast<uint8>(((66 * Pixel.R + 129 * Pixel.G + 25 * Pixel.B + 128) >> 8) + 16);
		}
	}

	for (int32 ColourRow = 0; ColourRow < ColourHeight; ++ColourRow)
	{
		for (int32 ColourColumn = 0; ColourColumn < ColourWidth; ++ColourColumn)
		{
			int32 SumR = 0;
			int32 SumG = 0;
			int32 SumB = 0;
			int32 Count = 0;
			for (int32 Row = ColourRow * 2; Row < FMath::Min(ColourRow * 2 + 2, Height); ++Row)
			{
				for (int32 Column = ColourColumn * 2; Column < FMath::Min(ColourColumn * 2 + 2, Width); ++Column)
				{
					const FColor& Pixel = Source[Row * Width + Column];
					SumR += Pixel.R;
					SumG += Pixel.G;
					SumB += Pixel.B;
					++Count;
				}
			}
			const int32 R = SumR / Count;
			const int32 G = SumG / Count;
			const int32 B = SumB / Count;
			Blue[ColourRow * ColourWidth + ColourColumn] = static_cast<uint8>(((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128);
			Red[ColourRow * ColourWidth + ColourColumn] = static_cast<uint8>(((112 * R - 94 * G - 18 * B + 128) >> 8) + 128);
		}
	}
}
