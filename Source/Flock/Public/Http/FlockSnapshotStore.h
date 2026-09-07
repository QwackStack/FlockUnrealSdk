// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

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

	/**
	 * Reserved first scope segment for STATE, as opposed to cache. Never pruned.
	 *
	 * The layout puts the game version first and every other version is deleted at startup, which is right
	 * for a cached response and catastrophic for anything that is not re-fetchable. A queued offline write
	 * exists nowhere else — the server has never seen it — so deleting it does not cost a request, it loses
	 * the player's data.
	 *
	 * Compose a state scope with this first and the owning player last: "_state/command/<PlayerId>". Cache
	 * keeps the version first, unchanged: "<GameVersionId>/leaderboard".
	 *
	 * Safe as a reserved name because a game version id is a ULID ([0-9A-Z]) and can never sanitize to it,
	 * and an unset version sanitizes to "_" rather than to it.
	 */
	static const TCHAR* const StateScope;

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

	/**
	 * Removes an entire scope directory (e.g. one category under the current version).
	 *
	 * This is a **recursive tree delete**, and a scope is the only unit of deletion the store offers, so
	 * everything sharing a scope shares its fate. Anything that must outlive a delete belongs in a scope of
	 * its own — that is why a per-player scope suffix exists and why state lives in a separate category from
	 * cache. Keys are not enumerable back from disk (a name is sanitized, capped at 64 chars, and suffixed
	 * with a hash), so "delete the scope but keep these entries" cannot be done by inspection.
	 */
	void DeleteScope(const FString& Scope);

	/** Removes one entry. Silent when it was not there — this is a delete, not an assertion it existed. */
	void DeleteKey(const FString& Scope, const FString& Key);

	/** Drops every top-level scope directory except KeepGameVersionId, BootstrapScope and StateScope. */
	void PruneOtherVersions(const FString& KeepGameVersionId);

	/**
	 * Moves pre-1.9.0 state out of the version-scoped tree and under StateScope. Returns how many files moved.
	 *
	 * One-time, legacy-only, and deliberately not a general mechanism. The offline write queue used to live
	 * at "<GameVersionId>/command/<PlayerId>", so shipping a build with a new game version had
	 * PruneOtherVersions delete the player's unsent writes — silently, with no error and no log. State now
	 * lives outside that tree by construction, so nothing written from this version on needs rescuing.
	 *
	 * It has to run BEFORE the prune, which is why the subsystem calls it rather than a provider rescuing
	 * its own queue when it loads: by then the directory is already gone.
	 *
	 * Delete this once no install predating 1.9.0 can still be upgraded.
	 */
	int32 MigrateLegacyState(const TArray<FString>& Leaves);

private:
	int32 MoveTree(const FString& From, const FString& To);

	FString BuildPath(const FString& Scope, const FString& Key) const;
	static FString SanitizeScope(const FString& Scope);
	static FString Sanitize(const FString& Id);
	static FString Hash8(const FString& Key);
	static void TryDelete(const FString& Path);

	FString Root;
	TSharedRef<IFlockLogger> Logger;
	FString SdkVersion;
};
