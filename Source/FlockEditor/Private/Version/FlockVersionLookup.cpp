// Copyright 2022, Qwacks. All Rights Reserved.

#include "Version/FlockVersionLookup.h"

void FFlockStubVersionLookup::Resolve(const FString& ApiUrl, const FString& ApiKey, const FString& GameVersion, FFlockResolveComplete OnComplete)
{
	OnComplete.ExecuteIfBound(FFlockResolveResult::Fail(
		TEXT("No Flock version lookup is registered, so the Game Version cannot be resolved. ")
		TEXT("Ensure the FlockEditor module is loaded.")));
}

namespace
{
	// Function-local static so the default stub is constructed on first use, not at static-init time.
	TSharedRef<IFlockVersionLookup>& ActiveLookup()
	{
		static TSharedRef<IFlockVersionLookup> Lookup = MakeShared<FFlockStubVersionLookup>();
		return Lookup;
	}
}

IFlockVersionLookup& FFlockVersionLookupRegistry::Get()
{
	return ActiveLookup().Get();
}

void FFlockVersionLookupRegistry::Set(const TSharedRef<IFlockVersionLookup>& InLookup)
{
	ActiveLookup() = InLookup;
}

void FFlockVersionLookupRegistry::Reset()
{
	ActiveLookup() = MakeShared<FFlockStubVersionLookup>();
}
