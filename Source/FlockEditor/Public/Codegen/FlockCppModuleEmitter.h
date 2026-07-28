// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Codegen/FlockCodegenManifest.h"

/**
 * The C++ target's skeleton: the module that generated headers live in, and the project registration
 * that makes it build.
 *
 * Separate from the type emitters on purpose. The skeleton has to be correct *before* a single `USTRUCT`
 * exists — a module that does not compile breaks the whole project's build, not one feature — so it is
 * emitted, registered, and verified as an empty module first, and types are added into something already
 * known to work.
 *
 * ## What it will not do
 *
 * **A Blueprint-only project is refused, not converted.** With no `Source/` there are no `*.Target.cs`
 * files either, so "just add a module" means generating a project's whole C++ skeleton and requiring a
 * compiler on the machine. That silently changes what kind of project someone has. The Blueprint target
 * exists precisely so those projects are never forced through it, and the error says so.
 *
 * **It does not run the build.** The editor cannot adopt new reflection data — Live Coding patches
 * function bodies, it does not add `USTRUCT`/`UENUM` types — so a restart is unavoidable however the
 * build is started. Given that, driving UnrealBuildTool would buy nothing and add a platform-specific
 * failure mode to own; the sync writes sources, registers the module, and says what to do next.
 */
class FLOCKEDITOR_API FFlockCppModuleEmitter
{
public:
	struct FEmitResult
	{
		bool bSucceeded = false;

		/** Empty on success; why the skeleton could not be written otherwise. */
		FString Error;

		/** The module's name, taken from the generated root's folder (Source/FlockGenerated -> FlockGenerated). */
		FString ModuleName;

		/** True when the .uproject gained the module entry on this run — i.e. a rebuild is newly required. */
		bool bProjectFilePatched = false;

		TArray<FString> Warnings;
	};

	/**
	 * Writes the .Build.cs, the module implementation, and the manifest header into an already-resolved
	 * generated root, then registers the module in the project file.
	 *
	 * `ProjectFilePath` is injectable so a test can patch a throwaway .uproject rather than the running
	 * project's own.
	 */
	static FEmitResult Emit(const FString& GeneratedRoot, const FFlockCodegenManifest& Manifest,
		const FString& ProjectFilePath);

	/** Emit against the running project's .uproject. */
	static FEmitResult EmitForProject(const FString& GeneratedRoot, const FFlockCodegenManifest& Manifest);

	/**
	 * Adds the module to a .uproject if absent. Idempotent and additive — an already-registered module is
	 * a no-op that reports success, so a re-sync never rewrites the project file.
	 */
	static bool RegisterModule(const FString& ProjectFilePath, const FString& ModuleName, bool& bOutPatched,
		FString& OutError);

	/** Removes the module entry. Kept for completeness; a target switch does not use it — see below. */
	static bool UnregisterModule(const FString& ProjectFilePath, const FString& ModuleName, bool& bOutPatched,
		FString& OutError);

	/**
	 * Deletes the generated types and accessors, **keeping the module skeleton**.
	 *
	 * The skeleton stays because removing it is what would break the project: deleting the `.Build.cs` or
	 * the `.uproject` entry leaves a registered module with no sources, or user code including a header
	 * from a module that no longer exists. An empty module compiles and costs nothing, and it is already
	 * in place if the target is switched back.
	 *
	 * `bResetManifest` rewrites the manifest header with empty values rather than deleting it — anything
	 * including it must keep compiling, and it must stop claiming a version nothing was generated for.
	 */
	static bool RemoveGeneratedTypes(const FString& GeneratedRoot, const FString& ModuleName,
		bool bResetManifest, FString& OutError);

	/**
	 * False when this project has no C++ at all. Checked by looking for `*.Target.cs` under `Source/`,
	 * which is what actually distinguishes a C++ project — a `Source/` folder alone can exist without one.
	 */
	static bool ProjectHasCppTargets(const FString& ProjectDir);

	/** "Source/FlockGenerated" -> "FlockGenerated". A module's name is its .Build.cs basename. */
	static FString ModuleNameFromRoot(const FString& GeneratedRoot);

	/** The C++ target needs the generated root under Source/, or UnrealBuildTool never sees the module. */
	static bool IsUnderSourceDir(const FString& GeneratedRoot, const FString& ProjectDir);
};
