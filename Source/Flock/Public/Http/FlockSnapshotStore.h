// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FlockLogger.h"

/**
 * Read-side offline cache: one JSON file per (scope, key), holding the last successful response payload so
 * a later fetch can serve it when the network is down. Payloads are opaque JSON strings (object or array) —
 * the store never interprets them; the provider base owns turning a model into a payload and back.
 *
 * On-disk layout: <root>/<scope>/<sanitized-key>_<hash8>.json, where the scope is normally
 * "<GameVersionId>/<category>" so a version switch parks stale trees under a different top-level folder
 * (pruned by PruneOtherVersions). BootstrapScope is reserved for lookups that necessarily run before the
 * version id is known (a by-name version resolve) and is exempt from pruning.
 *
 * Each file is a versioned envelope {v, sdk, stored_at_utc, data}. A version bump, a null payload, or a
 * corrupt file reads as a miss and deletes the file — a bad cache must degrade to a cache miss, never to a
 * failed fetch. There is no TTL: entries live until overwritten by a fresh success, dropped by a scope
 * delete, or pruned by a version change.
 */
class FLOCK_API FFlockSnapshotStore
{
public:
	/** Bumped only when the envelope format changes; a mismatch invalidates every existing entry. */
	static constexpr int32 EnvelopeVersion = 1;

	/** Reserved scope for pre-version lookups; never pruned by PruneOtherVersions. */
	static const TCHAR* const BootstrapScope;

	/** Empty root defaults to <ProjectSavedDir>/Flock/snapshots. SdkVersion is stamped into the envelope. */
	FFlockSnapshotStore(const FString& InRootDirectory, const TSharedRef<IFlockLogger>& InLogger, const FString& InSdkVersion);

	/** Default root, exposed so the subsystem and tests can reason about the location. */
	static FString DefaultRoot();

	/**
	 * Wraps Payload (a JSON object or array string) in a fresh envelope and writes it under scope/key,
	 * replacing any prior entry. A payload that does not parse as JSON is dropped with a warning rather
	 * than stored — a genuine response is always valid JSON. Best-effort: a write failure is logged, never
	 * thrown.
	 */
	void Write(const FString& Scope, const FString& Key, const FString& Payload);

	/**
	 * Reads the stored payload back into OutPayload. Returns false on any miss — absent file, wrong
	 * envelope version, or corrupt content — and deletes the file in the corrupt/stale cases.
	 */
	bool TryRead(const FString& Scope, const FString& Key, FString& OutPayload) const;

	/** Removes an entire scope directory (e.g. one category under the current version). */
	void DeleteScope(const FString& Scope);

	/** Drops every top-level scope directory except KeepGameVersionId and BootstrapScope. */
	void PruneOtherVersions(const FString& KeepGameVersionId);

private:
	FString BuildPath(const FString& Scope, const FString& Key) const;
	static FString SanitizeScope(const FString& Scope);
	static FString Sanitize(const FString& Id);
	static FString Hash8(const FString& Key);
	static void TryDelete(const FString& Path);

	FString Root;
	TSharedRef<IFlockLogger> Logger;
	FString SdkVersion;
};
