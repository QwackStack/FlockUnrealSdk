// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Codegen/FlockSchemaSnapshot.h"

/**
 * A fingerprint of everything in a snapshot that generated code depends on.
 *
 * The contract is narrow and worth stating exactly: the hash **changes if and only if the generated
 * output would change**. That makes it the answer to a question the version id cannot answer — whether
 * the backend was edited *within* the same game version, which is the common case (a designer adds a
 * field) and is invisible to an id comparison.
 *
 * So it covers each template's and config's id, name, tag, and schema, and each shop's id, name, and its
 * items' ids, names, and currencies — the inputs to the typed structs, the typed id enums, and the baked
 * template ids. It deliberately does **not** cover anything generated code only reads at runtime (shop
 * prices, a config's current values): those change constantly and would report drift for a regen that
 * produced byte-identical output.
 *
 * Entities are sorted by id before hashing, so a backend that returns the same set in a different order
 * is not drift. Field *arrays* keep their order, because that order becomes member order in the generated
 * struct — a reorder is a real output change. Object keys inside the schema are sorted, because their
 * textual order is an artifact of serialization and changes nothing.
 *
 * SHA-1, not SHA-256: the engine ships SHA-1 in Core, and this is change detection rather than a security
 * property — the same use git puts it to. The stored value is opaque, so the algorithm can change later
 * without touching the contract (a differing hash is drift, which is exactly the right outcome).
 */
class FLOCKEDITOR_API FFlockSchemaHasher
{
public:
	/** The snapshot's content fingerprint as lowercase hex. */
	static FString ComputeContentHash(const FFlockSchemaSnapshot& Snapshot);

	/**
	 * Canonical form of a JSON document: object keys sorted, array order preserved, no whitespace.
	 * Exposed because it is the interesting half of the hash and is worth testing directly.
	 */
	static FString Canonicalize(const FString& Json);
};
