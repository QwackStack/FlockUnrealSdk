// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Result of a Game Version name-to-ID resolve. */
struct FFlockResolveResult
{
	bool bSuccess = false;
	FString GameVersionId;
	FString Error;

	static FFlockResolveResult Ok(const FString& InGameVersionId)
	{
		FFlockResolveResult Result;
		Result.bSuccess = true;
		Result.GameVersionId = InGameVersionId;
		return Result;
	}

	static FFlockResolveResult Fail(const FString& InError)
	{
		FFlockResolveResult Result;
		Result.bSuccess = false;
		Result.Error = InError;
		return Result;
	}
};

/** Completion callback for an async resolve. Runs on the game thread. */
DECLARE_DELEGATE_OneParam(FFlockResolveComplete, const FFlockResolveResult&);

/**
 * Transport seam for the edit-time Game Version lookup. Kept behind an interface so the general
 * HTTP layer (QWA-978) can drop in a real FFlockHttpVersionLookup without touching the resolver.
 */
class FLOCKEDITOR_API IFlockVersionLookup
{
public:
	virtual ~IFlockVersionLookup() = default;

	/** Resolves the Game Version name to its ID, invoking OnComplete when finished. */
	virtual void Resolve(const FString& ApiUrl, const FString& ApiKey, const FString& GameVersion, FFlockResolveComplete OnComplete) = 0;

	/**
	 * True when this lookup can actually contact the backend. The stub returns false, which keeps
	 * the build guard inert until a real lookup (QWA-978) is registered.
	 */
	virtual bool CanResolve() const = 0;
};

/** Default lookup used until the HTTP layer (QWA-978) registers a real one. Fails cleanly. */
class FLOCKEDITOR_API FFlockStubVersionLookup : public IFlockVersionLookup
{
public:
	virtual void Resolve(const FString& ApiUrl, const FString& ApiKey, const FString& GameVersion, FFlockResolveComplete OnComplete) override;
	virtual bool CanResolve() const override { return false; }
};

/**
 * Process-wide holder for the active version lookup. Stub by default; QWA-978 calls Set() to swap in
 * the real HTTP-backed lookup. The resolver and the build guard both read this.
 */
class FLOCKEDITOR_API FFlockVersionLookupRegistry
{
public:
	/** The active lookup (the stub until a real one is registered). */
	static IFlockVersionLookup& Get();

	/** Register a real lookup (e.g. FFlockHttpVersionLookup from QWA-978). */
	static void Set(const TSharedRef<IFlockVersionLookup>& InLookup);

	/** Revert to the stub lookup. */
	static void Reset();
};
