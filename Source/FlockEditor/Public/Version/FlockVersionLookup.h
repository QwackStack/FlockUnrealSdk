// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"

/** Result of a Game Version name-to-ID resolve. */
struct FFlockResolveResult
{
	bool bSuccess = false;
	FString GameVersionId;

	/** Flattened message. Kept because the toast and log paths only ever wanted a sentence. */
	FString Error;

	/**
	 * The typed failure, carrying error type, HTTP status, and the server's coded message.
	 *
	 * Flattening this to Error alone was what made every setup failure — unreachable host, rejected key,
	 * misspelled version — arrive as undifferentiated text. The connection probe classifies from here.
	 */
	FFlockError Detail;

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
		Result.Detail = FFlockError::Make(EFlockErrorType::Network, InError);
		return Result;
	}

	/** Failure that keeps the diagnosis. Preferred wherever a typed error is in hand. */
	static FFlockResolveResult FailWith(const FFlockError& InError)
	{
		FFlockResolveResult Result;
		Result.bSuccess = false;
		Result.Error = InError.Message;
		Result.Detail = InError;
		return Result;
	}
};

/** Completion callback for an async resolve. Runs on the game thread. */
DECLARE_DELEGATE_OneParam(FFlockResolveComplete, const FFlockResolveResult&);

/**
 * Transport seam for the edit-time Game Version lookup. Kept behind an interface so the HTTP-backed
 * FFlockHttpVersionLookup can be registered without touching the resolver.
 */
class FLOCKEDITOR_API IFlockVersionLookup
{
public:
	virtual ~IFlockVersionLookup() = default;

	/** Resolves the Game Version name to its ID, invoking OnComplete when finished. */
	virtual void Resolve(const FString& ApiUrl, const FString& ApiKey, const FString& GameVersion, FFlockResolveComplete OnComplete) = 0;

	/**
	 * True when this lookup can actually contact the backend. The stub returns false, which keeps
	 * the build guard inert until a real lookup is registered.
	 */
	virtual bool CanResolve() const = 0;
};

/** Default lookup used until a real HTTP-backed lookup is registered. Fails cleanly. */
class FLOCKEDITOR_API FFlockStubVersionLookup : public IFlockVersionLookup
{
public:
	virtual void Resolve(const FString& ApiUrl, const FString& ApiKey, const FString& GameVersion, FFlockResolveComplete OnComplete) override;
	virtual bool CanResolve() const override { return false; }
};

/**
 * Process-wide holder for the active version lookup. Stub by default; the FlockEditor module calls Set()
 * to register the real HTTP-backed lookup on startup. The resolver and the build guard both read this.
 */
class FLOCKEDITOR_API FFlockVersionLookupRegistry
{
public:
	/** The active lookup (the stub until a real one is registered). */
	static IFlockVersionLookup& Get();

	/** Register a real lookup (the HTTP-backed FFlockHttpVersionLookup). */
	static void Set(const TSharedRef<IFlockVersionLookup>& InLookup);

	/** Revert to the stub lookup. */
	static void Reset();
};
