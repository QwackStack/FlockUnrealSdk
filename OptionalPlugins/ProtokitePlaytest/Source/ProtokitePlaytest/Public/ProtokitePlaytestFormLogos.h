// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "ProtokitePlaytestSettings.h"
#include "UObject/StrongObjectPtr.h"

class UTexture2D;

/**
 * The images a feedback form draws: the game's own logos in a row above the title, and one image at the bottom left, on
 * the row of Close and Send -- Powered by Qwacks unless the game set its own.
 *
 * Read from the settings each time a form opens, and held by the form while it is open: a brush does not keep its
 * texture loaded, so without these references a texture the game let go of could be collected under an open form.
 */
struct PROTOKITEPLAYTEST_API FProtokitePlaytestFormLogos
{
	/** The Powered by Qwacks mark this plugin carries, 512 by 80 with a transparent background. */
	static const TCHAR* const PoweredByQwacksPath;

	/** The game's own logos that can be drawn, in their order. Empty slots and images with no size are left out. */
	TArray<TStrongObjectPtr<UTexture2D>> Logos;

	/** How tall each logo is drawn, kept to the range the settings page offers. */
	float LogoHeight = UProtokitePlaytestSettings::DefaultFeedbackFormLogoHeight;

	/** The image at the bottom left: the game's own, or Powered by Qwacks. Unset only when even the mark cannot be loaded. */
	TStrongObjectPtr<UTexture2D> FooterLogo;

	/** How tall the footer image is drawn, kept to the range the settings page offers. */
	float FooterLogoHeight = UProtokitePlaytestSettings::DefaultFeedbackFormFooterLogoHeight;

	/** What a form opened now draws: the settings as they are, the textures loaded. */
	static FProtokitePlaytestFormLogos FromSettings(const UProtokitePlaytestSettings& Settings);

	/** The mark, loaded. Null when the plugin's content is missing. */
	static UTexture2D* LoadPoweredByQwacks();

	/**
	 * The shape an image is drawn in: the size it was imported at, so an image rounded to a power of two on import keeps
	 * its own shape. An image made while the game runs has no imported size, and its own size is used.
	 */
	static FIntPoint ShapeOf(const UTexture2D& Image);

	/** Whether an image has a shape to draw: loaded, and wider and taller than nothing. */
	static bool CanBeDrawn(const UTexture2D* Image);

	/** The size an image is drawn at before it is fitted to its row: Height tall in its own shape. Zero for an image that cannot be drawn. */
	static FVector2D DrawnSize(const UTexture2D& Image, float Height);

	/** A logo height kept to 16 to 96; one that is not a number is the default. */
	static float KeepLogoHeightInRange(float Height);

	/** A footer logo height kept to 12 to 48; one that is not a number is the default. */
	static float KeepFooterLogoHeightInRange(float Height);

	/**
	 * Every image a form can draw, for packaging: the mark, which only this plugin's code names, and the images the
	 * settings name. The playtest editor module records each as the engine records a project setting's references, and
	 * the cook packages what was recorded.
	 */
	static TArray<FSoftObjectPath> ImagesToPackage(const UProtokitePlaytestSettings& Settings);
};
