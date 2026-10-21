// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Codegen/FlockCodegenPaths.h"

#include "Config/FlockConfig.h"
#include "Misc/PackageName.h"
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

bool FFlockCodegenPaths::TryResolveContentRoot(const FString& Configured, FString& OutRoot, FString& OutError)
{
	OutRoot.Reset();
	const TCHAR* const Fix = TEXT("Set Generated Content Path in Project Settings > Flock SDK > Codegen to a folder of "
		"its own, such as /Game/Flock/Generated.");

	FString Root = Configured.TrimStartAndEnd();
	while (Root.RemoveFromEnd(TEXT("/")))
	{
	}
	if (Root.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Generated Content Path is empty. %s"), Fix);
		return false;
	}
	if (!Root.StartsWith(TEXT("/")))
	{
		OutError = FString::Printf(
			TEXT("Generated Content Path must be a package path such as /Game/Flock/Generated, but it is '%s'. %s"),
			*Configured, Fix);
		return false;
	}

	// Each folder name checked on its own, so `/Game/Flock/..` cannot climb back to the whole content folder.
	TArray<FString> Folders;
	Root.RightChop(1).ParseIntoArray(Folders, TEXT("/"), /*InCullEmpty*/ false);
	for (const FString& Folder : Folders)
	{
		if (Folder.IsEmpty() || Folder == TEXT(".") || Folder == TEXT(".."))
		{
			OutError = FString::Printf(
				TEXT("Generated Content Path '%s' has an empty folder name, '.' or '..' in it. %s"), *Configured, Fix);
			return false;
		}
	}
	// A character a package name cannot hold (a space, a full stop inside a name) would reach CreatePackage otherwise.
	FText Invalid;
	if (!FPackageName::IsValidTextForLongPackageName(Root, &Invalid))
	{
		OutError = FString::Printf(TEXT("Generated Content Path '%s' is not a valid package path: %s %s"),
			*Configured, *Invalid.ToString(), Fix);
		return false;
	}

	const FName MountPoint = FPackageName::GetPackageMountPoint(Root, /*InWithoutSlashes*/ true);
	if (MountPoint.IsNone())
	{
		OutError = FString::Printf(
			TEXT("Generated Content Path '%s' is not inside a content folder this project has, such as /Game. %s"),
			*Configured, Fix);
		return false;
	}
	if (Root.Equals(TEXT("/") + MountPoint.ToString(), ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(
			TEXT("Generated Content Path '%s' is a whole content folder, which holds the project's own assets. "
				"Codegen writes and removes assets in the folder it is given. %s"), *Configured, Fix);
		return false;
	}

	// The folder on disk, which for an engine mount or a plugin installed into the engine is not the project's.
	FString Folder;
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectDir);
	if (!FPackageName::TryConvertLongPackageNameToFilename(Root, Folder))
	{
		OutError = FString::Printf(
			TEXT("Generated Content Path '%s' has no folder on disk, so its assets could not be saved. %s"),
			*Configured, Fix);
		return false;
	}
	Folder = FPaths::ConvertRelativePathToFull(Folder);
	FPaths::NormalizeDirectoryName(Folder);
	if (!Folder.StartsWith(ProjectDir + TEXT("/"), ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(
			TEXT("Generated Content Path '%s' is saved to '%s', which is outside this project's folder. %s"),
			*Configured, *Folder, Fix);
		return false;
	}

	OutRoot = Root;
	OutError.Reset();
	return true;
}

FString FFlockCodegenPaths::ManifestPath(const FString& GeneratedRoot)
{
	return FPaths::Combine(GeneratedRoot, ManifestFileName);
}
