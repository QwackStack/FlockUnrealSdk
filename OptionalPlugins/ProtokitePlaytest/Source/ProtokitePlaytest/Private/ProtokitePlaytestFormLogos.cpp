// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "ProtokitePlaytestFormLogos.h"

#include "Engine/Texture2D.h"
#include "ProtokitePlaytestLog.h"
#include "ProtokitePlaytestSettings.h"

#if WITH_EDITOR
#include "TextureCompiler.h"
#endif

const TCHAR* const FProtokitePlaytestFormLogos::PoweredByQwacksPath = TEXT("/ProtokitePlaytest/ProtokitePlaytestPoweredByQwacks.ProtokitePlaytestPoweredByQwacks");

namespace
{
	/** How long a form's images are kept at full detail after it opens: longer than anyone fills in a form. */
	constexpr float ImagesKeptSharpForSeconds = 30.f * 60.f;

	/**
	 * The image a setting names, loaded and ready to measure, or null with the reason said. An empty setting says nothing:
	 * an unset slot is a choice, not a mistake.
	 */
	UTexture2D* LoadForTheForm(const TSoftObjectPtr<UTexture2D>& Setting, const TCHAR* SettingName)
	{
		if (Setting.IsNull())
		{
			return nullptr;
		}
		UTexture2D* Image = Setting.LoadSynchronous();
		if (Image == nullptr)
		{
#if WITH_EDITOR
			UE_LOG(LogProtokitePlaytest, Warning,
				TEXT("The feedback form leaves out %s from %s: it could not be loaded. Check the image still exists at that path, or take it out of %s in Project Settings > Plugins > Protokite Playtest Settings."),
				*Setting.ToString(), SettingName, SettingName);
#else
			// In a packaged game the image was not packaged: named only in a platform's own config, or set while the game runs.
			UE_LOG(LogProtokitePlaytest, Warning,
				TEXT("The feedback form leaves out %s from %s: it was not packaged with the game. Name it in %s in Project Settings > Plugins > Protokite Playtest Settings before packaging, or refer to it from something the game packages."),
				*Setting.ToString(), SettingName, SettingName);
#endif
			return nullptr;
		}
#if WITH_EDITOR
		// In the editor a texture may still be building, and its size is then the placeholder's, not its own.
		FTextureCompilingManager::Get().FinishCompilation({ Image });
#endif
		if (!FProtokitePlaytestFormLogos::CanBeDrawn(Image))
		{
			UE_LOG(LogProtokitePlaytest, Warning,
				TEXT("The feedback form leaves out %s from %s: the image has no size to draw. Import it again from its file, or take it out of %s."),
				*Setting.ToString(), SettingName, SettingName);
			return nullptr;
		}
		// A game's image in a streamed texture group could otherwise be drawn at a low detail until it streams in. An image
		// that never streams is left as the game set it.
		if (!Image->NeverStream)
		{
			Image->SetForceMipLevelsToBeResident(ImagesKeptSharpForSeconds);
		}
		return Image;
	}

	float KeepInRange(float Height, float Shortest, float Tallest, float Default)
	{
		return FMath::IsFinite(Height) ? FMath::Clamp(Height, Shortest, Tallest) : Default;
	}
}

FProtokitePlaytestFormLogos FProtokitePlaytestFormLogos::FromSettings(const UProtokitePlaytestSettings& Settings)
{
	FProtokitePlaytestFormLogos Result;
	for (const TSoftObjectPtr<UTexture2D>& Setting : Settings.FeedbackFormLogos)
	{
		if (UTexture2D* Image = LoadForTheForm(Setting, TEXT("Feedback Form Logos")))
		{
			Result.Logos.Emplace(Image);
		}
	}
	Result.LogoHeight = KeepLogoHeightInRange(Settings.FeedbackFormLogoHeight);

	// The game's own image replaces the mark; one that cannot be drawn leaves the mark in its place, not a gap.
	UTexture2D* Footer = LoadForTheForm(Settings.FeedbackFormFooterLogo, TEXT("Feedback Form Footer Logo"));
	if (Footer == nullptr)
	{
		Footer = LoadPoweredByQwacks();
	}
	if (Footer != nullptr)
	{
		Result.FooterLogo.Reset(Footer);
	}
	Result.FooterLogoHeight = KeepFooterLogoHeightInRange(Settings.FeedbackFormFooterLogoHeight);
	return Result;
}

UTexture2D* FProtokitePlaytestFormLogos::LoadPoweredByQwacks()
{
	UTexture2D* Mark = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(PoweredByQwacksPath)).LoadSynchronous();
#if WITH_EDITOR
	if (Mark != nullptr)
	{
		FTextureCompilingManager::Get().FinishCompilation({ Mark });
	}
#endif
	if (!CanBeDrawn(Mark))
	{
#if WITH_EDITOR
		UE_LOG(LogProtokitePlaytest, Warning,
			TEXT("The feedback form shows no Powered by Qwacks mark: %s is missing from the plugin. Install the Protokite Playtest plugin again with its Content folder."),
			PoweredByQwacksPath);
#else
		UE_LOG(LogProtokitePlaytest, Warning,
			TEXT("The feedback form shows no Powered by Qwacks mark: %s was not packaged with the game. Package the game again with the Protokite Playtest plugin enabled and its Content folder installed."),
			PoweredByQwacksPath);
#endif
		return nullptr;
	}
	return Mark;
}

FIntPoint FProtokitePlaytestFormLogos::ShapeOf(const UTexture2D& Image)
{
	// Measured: in an editor that draws nothing (-nullrhi) a texture never builds what it draws from, and its own size
	// reads 0 by 0, while the imported size is there in the editor and in a packaged game alike.
	const FIntPoint Imported = Image.GetImportedSize();
	if (Imported.X > 0 && Imported.Y > 0)
	{
		return Imported;
	}
	return FIntPoint(Image.GetSizeX(), Image.GetSizeY());
}

bool FProtokitePlaytestFormLogos::CanBeDrawn(const UTexture2D* Image)
{
	if (Image == nullptr)
	{
		return false;
	}
	const FIntPoint Shape = ShapeOf(*Image);
	return Shape.X > 0 && Shape.Y > 0;
}

FVector2D FProtokitePlaytestFormLogos::DrawnSize(const UTexture2D& Image, float Height)
{
	if (!CanBeDrawn(&Image) || !(Height > 0.f))
	{
		return FVector2D::ZeroVector;
	}
	const FIntPoint Shape = ShapeOf(Image);
	return FVector2D(Height * Shape.X / Shape.Y, Height);
}

float FProtokitePlaytestFormLogos::KeepLogoHeightInRange(float Height)
{
	return KeepInRange(Height, UProtokitePlaytestSettings::ShortestFeedbackFormLogoHeight,
		UProtokitePlaytestSettings::TallestFeedbackFormLogoHeight, UProtokitePlaytestSettings::DefaultFeedbackFormLogoHeight);
}

float FProtokitePlaytestFormLogos::KeepFooterLogoHeightInRange(float Height)
{
	return KeepInRange(Height, UProtokitePlaytestSettings::ShortestFeedbackFormFooterLogoHeight,
		UProtokitePlaytestSettings::TallestFeedbackFormFooterLogoHeight, UProtokitePlaytestSettings::DefaultFeedbackFormFooterLogoHeight);
}

TArray<FSoftObjectPath> FProtokitePlaytestFormLogos::ImagesToPackage(const UProtokitePlaytestSettings& Settings)
{
	TArray<FSoftObjectPath> Images;
	Images.AddUnique(FSoftObjectPath(PoweredByQwacksPath));
	for (const TSoftObjectPtr<UTexture2D>& Setting : Settings.FeedbackFormLogos)
	{
		if (!Setting.IsNull())
		{
			Images.AddUnique(Setting.ToSoftObjectPath());
		}
	}
	if (!Settings.FeedbackFormFooterLogo.IsNull())
	{
		Images.AddUnique(Settings.FeedbackFormFooterLogo.ToSoftObjectPath());
	}
	return Images;
}
