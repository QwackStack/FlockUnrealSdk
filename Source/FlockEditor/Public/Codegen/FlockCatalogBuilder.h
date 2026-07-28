// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Codegen/FlockSchemaSnapshot.h"

class UFlockContentCatalog;

/**
 * Turns a schema snapshot into the content catalog.
 *
 * Split deliberately in two: Populate() is a pure transformation over an object the caller owns, and
 * Save() is the asset plumbing. Everything interesting — how a schema's fields are read, which template
 * supplies the achievements, how currencies are gathered — lives in the pure half and is testable without
 * touching the asset system.
 *
 * Ordering is stable (entities by id, matching the hasher and the emitters) so a re-sync of unchanged
 * content produces an identical asset rather than a spurious diff for whoever commits it.
 */
class FLOCKEDITOR_API FFlockCatalogBuilder
{
public:
	/** The catalog asset's name inside the generated content folder. */
	static const TCHAR* const AssetName;

	/** Fills a catalog from a snapshot, replacing whatever it held. */
	static void Populate(const FFlockSchemaSnapshot& Snapshot, UFlockContentCatalog& OutCatalog);

	/**
	 * Creates or overwrites the catalog asset under the configured generated content path and saves it to
	 * disk. False with OutError on any step; the package is left untouched on failure.
	 */
	static bool Save(const FFlockSchemaSnapshot& Snapshot, const FString& ContentPath, FString& OutError);

	/**
	 * The top-level declared fields of a template's or config's verbatim `schema`. Nested children are not
	 * flattened in: the catalog answers "what may I write, and what is it called", and a write targets a
	 * top-level field. The struct emitter walks the tree properly when it needs the nested shape.
	 */
	static TArray<struct FFlockCatalogField> ReadFields(const FString& SchemaJson);
};
