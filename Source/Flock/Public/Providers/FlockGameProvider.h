// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockGameModels.h"

/**
 * Game + version info. Three reads, each memoized in-process and snapshot-backed: the game record, this
 * build's game version, and a version looked up by name. All enveloped, no sign-in gate.
 *
 * The by-name lookup is what runs *before* the version id is known, so it cannot be scoped under it — it
 * lives on the snapshot's BootstrapScope, keyed by API URL + name so a dev and a prod backend don't share
 * an entry. (The editor's edit-time version bake does its own by-name call with no subsystem or store; that
 * one stays separate — not a duplicate to unify.)
 *
 * Completion-lambda rule: capture shared refs / weak self / values only — never `this`.
 */
class FLOCK_API FFlockGameProvider
	: public FFlockProviderBase
	, public TSharedFromThis<FFlockGameProvider>
{
public:
	FFlockGameProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
		const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
		const FString& InVersionedApiUrl, const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore,
		const FString& InGameVersionId);

	void GetGame(TFunction<void(TFlockResult<FFlockGameSchema>)> OnComplete);
	void GetGameVersion(TFunction<void(TFlockResult<FFlockGameVersionSchema>)> OnComplete);
	void GetGameVersionByName(const FString& Name, TFunction<void(TFlockResult<FFlockGameVersionSchema>)> OnComplete);

	/** Drops the in-process caches and the game snapshot category. */
	void ClearCache();

private:
	FString MakeUrl(const FString& Path) const { return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Path); }
	TMap<FString, FString> HeadersNow() const { return Session->GetAuthHeaders(); }

	TSharedRef<FFlockAuthSession> Session;
	FString VersionedApiUrl;

	bool bGameFetched = false;
	FFlockGameSchema Game;
	bool bVersionFetched = false;
	FFlockGameVersionSchema GameVersion;
	TMap<FString, FFlockGameVersionSchema> VersionsByName;

	static const TCHAR* const SnapshotCategory;
};
