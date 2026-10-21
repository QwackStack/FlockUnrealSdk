// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/**
 * The one owner of which C++ files Flock codegen wrote, and so of which it may replace or remove.
 *
 * Every generated file opens with Banner, and that banner is the mark: a file under Generated Code Path that
 * does not open with it is the game's, whatever its name, and codegen neither overwrites nor deletes it. So a
 * Generated Code Path pointed at the game's own module (`Source/MyGame`) changes nothing there: its
 * `MyGame.Build.cs` is refused rather than replaced, and its headers and sources stay.
 */
class FLOCKEDITOR_API FFlockGeneratedFiles
{
public:
	/**
	 * The comment every generated C++ file opens with. No copyright line: these files are generated into the
	 * *user's* project from their own schema, so asserting the SDK's copyright over their content model would be
	 * wrong, and pointing at a LICENSE.md that lives in the plugin would dangle.
	 */
	static const TCHAR* const Banner;

	/** True when the file exists and opens with the banner. */
	static bool IsGenerated(const FString& Path);

	/** Deletes the file when codegen wrote it. True when it went. */
	static bool DeleteIfGenerated(const FString& Path);

	/**
	 * Writes the file only when its content differs, so a re-sync does not churn timestamps and force a rebuild.
	 * False, with OutError saying what is wrong and how to fix it, when a file codegen did not write is already
	 * there, or the write fails.
	 */
	static bool WriteIfChanged(const FString& Path, const FString& Content, FString& OutError);
};
