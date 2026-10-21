// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/**
 * Where generated code and generated assets go, and the guard on getting there.
 *
 * This matters more than a path helper usually does: a regen or a clean **deletes** in these folders.
 * What it deletes is only what codegen made (FFlockGeneratedFiles, FFlockGeneratedAssets), but it also
 * writes there, so a setting of `../../..` or `/Game` would put generated output among someone's own work.
 * So the code path is validated to be project-relative and to land strictly inside the project, and the
 * content path to be a folder below a content root; the project root and a whole content folder are
 * rejected outright.
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

	/**
	 * Turns UFlockConfig::GeneratedContentPath into the package path codegen writes its assets under, with
	 * no trailing slash. The one check on that setting: every emitter and every delete goes through it.
	 *
	 * False, with OutError saying what is wrong and how to fix it, when the setting is empty, is not a
	 * package path, climbs with `.` or `..`, is not inside a content folder the project mounts, is that
	 * content folder itself (`/Game`, or a plugin's), or is content outside the project's own folder (the
	 * engine's, or a plugin installed into the engine). Codegen writes and removes assets in this folder,
	 * so the whole of `/Game` is never an answer.
	 */
	static bool TryResolveContentRoot(const FString& Configured, FString& OutRoot, FString& OutError);

	/** The manifest path inside a resolved generated root. */
	static FString ManifestPath(const FString& GeneratedRoot);
};
