// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/**
 * Where generated code goes, and the guard on getting there.
 *
 * This matters more than a path helper usually does: the generated folder is Flock-owned, and a regen or
 * a clean **deletes its contents**. A setting of `../../..` would therefore not be a broken build, it
 * would be someone's disk. So the configured path is validated to be project-relative and to land
 * strictly inside the project, and the project root itself is rejected outright.
 */
class FLOCKEDITOR_API FFlockCodegenPaths
{
public:
	/** Where UFlockConfig::GeneratedCodePath points when it is left empty. */
	static const TCHAR* const DefaultGeneratedPath;

	/** The manifest's file name inside the generated root. */
	static const TCHAR* const ManifestFileName;

	/**
	 * Turns the project-relative setting into an absolute, normalized directory. False with OutError when
	 * the setting is absolute, escapes the project, or resolves to the project root.
	 */
	static bool TryResolveGeneratedRoot(const FString& Configured, FString& OutAbsoluteRoot, FString& OutError);

	/** TryResolveGeneratedRoot against the project's Flock settings. */
	static bool TryResolveGeneratedRootFromSettings(FString& OutAbsoluteRoot, FString& OutError);

	/** The manifest path inside a resolved generated root. */
	static FString ManifestPath(const FString& GeneratedRoot);
};
