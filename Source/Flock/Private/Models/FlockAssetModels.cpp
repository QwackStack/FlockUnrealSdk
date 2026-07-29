// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Models/FlockAssetModels.h"

FString FFlockAsset::VersionToken() const
{
	// An empty UpdatedAt still has to produce a stable token, or every fetch would look like a new
	// version and re-download bytes that are already on disk.
	return FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*UpdatedAt));
}
