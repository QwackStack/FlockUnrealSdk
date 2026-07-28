// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Codegen/FlockCodegenPaths.h"

#include "Config/FlockConfig.h"
#include "Misc/Paths.h"

const TCHAR* const FFlockCodegenPaths::DefaultGeneratedPath = TEXT("Source/FlockGenerated");
const TCHAR* const FFlockCodegenPaths::ManifestFileName = TEXT("flock_schema_manifest.json");

bool FFlockCodegenPaths::TryResolveGeneratedRoot(const FString& Configured, FString& OutAbsoluteRoot, FString& OutError)
{
	OutAbsoluteRoot.Reset();

	FString Relative = Configured.TrimStartAndEnd();
	if (Relative.IsEmpty())
	{
		Relative = DefaultGeneratedPath;
	}
	Relative.ReplaceInline(TEXT("\\"), TEXT("/"));

	// An absolute path is rejected outright rather than normalized: it cannot be checked against the
	// project in a way that is meaningful across machines, and this folder gets deleted.
	if (FPaths::IsRelative(Relative) == false)
	{
		OutError = FString::Printf(
			TEXT("Generated Code Path must be relative to the project, but '%s' is absolute."), *Configured);
		return false;
	}

	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	// Trailing slash before collapsing, deliberately: CollapseRelativeDirectories matches the `/./` and
	// `/../` forms, so a path *ending* in `.` or `..` ("." on its own, or "Source/..") is left intact and
	// then sails through the containment check below while actually pointing at the project root — the one
	// case this guard exists to stop. Appending the slash turns those endings into the form it collapses.
	FString Candidate = FPaths::Combine(ProjectRoot, Relative) + TEXT("/");
	FPaths::CollapseRelativeDirectories(Candidate);
	FPaths::RemoveDuplicateSlashes(Candidate);
	FPaths::NormalizeDirectoryName(Candidate);
	Candidate = FPaths::ConvertRelativePathToFull(Candidate);
	FPaths::NormalizeDirectoryName(Candidate);

	FString NormalizedProjectRoot = ProjectRoot;
	FPaths::NormalizeDirectoryName(NormalizedProjectRoot);

	if (Candidate.Equals(NormalizedProjectRoot, ESearchCase::IgnoreCase))
	{
		OutError = TEXT("Generated Code Path resolves to the project root. Point it at a subfolder — "
			"regenerating deletes everything inside it.");
		return false;
	}
	// StartsWith on the root plus a separator, so a sibling folder whose name merely begins with the
	// project's ("MyGame2" next to "MyGame") cannot pass as being inside it.
	if (!Candidate.StartsWith(NormalizedProjectRoot + TEXT("/"), ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(
			TEXT("Generated Code Path must stay inside the project. '%s' resolves to '%s'."), *Configured, *Candidate);
		return false;
	}

	OutAbsoluteRoot = Candidate;
	OutError.Reset();
	return true;
}

bool FFlockCodegenPaths::TryResolveGeneratedRootFromSettings(FString& OutAbsoluteRoot, FString& OutError)
{
	const UFlockConfig* Config = GetDefault<UFlockConfig>();
	// A missing settings object is not a reason to fail: the default path is what an unconfigured project
	// would get anyway, and this keeps a fresh clone working before anyone opens the settings page.
	return TryResolveGeneratedRoot(Config ? Config->GeneratedCodePath : FString(), OutAbsoluteRoot, OutError);
}

FString FFlockCodegenPaths::ManifestPath(const FString& GeneratedRoot)
{
	return FPaths::Combine(GeneratedRoot, ManifestFileName);
}
