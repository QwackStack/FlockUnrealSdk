// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Codegen/FlockSchemaSnapshot.h"

/**
 * The C++ target's types: a `USTRUCT` header per template and config, and a `UENUM` header for the shop
 * items, currencies, and achievements.
 *
 * Text generation, not asset authoring — which makes this the *simpler* of the two targets to emit and
 * the harder one to be wrong in safely. A Blueprint asset that comes out malformed affects one graph; a
 * header that does not compile stops the whole project building. So every rule below fails toward a
 * field that is missing rather than a file that is broken.
 *
 * ## Member names are Pascal-cased, with the declared name registered alongside
 *
 * A field declared `game_currencies` becomes a member spelled `GameCurrencies`, so C++ reads idiomatically.
 * The declared name is emitted into a table the generated module installs into `FFlockStructBinder` on
 * startup, and the binder consults it while recursing — so a write goes out as `game_currencies` at
 * *every* depth, not just the top level.
 *
 * That depth is the whole point. A first cut shipped verbatim member names precisely because the map then
 * available only reached the top level, and a nested member would have gone out under its C++ spelling
 * and been rejected — the same bug the Blueprint tier had already paid for once. Recursing on both sides
 * is what makes the idiomatic naming safe.
 *
 * The mapping is **data, not `UPROPERTY` metadata**: `FField`'s metadata map is compiled out by
 * `WITH_METADATA`, so a meta-driven mapping would resolve in the editor and silently write C++ spellings
 * in a packaged build.
 *
 * ## What degrades rather than guessing
 *
 * - A name that cannot be *made* into a legal C++ identifier is skipped with a warning. One that can —
 *   `200`, `class` — is sanitized to `_200` / `Class`, which the registry makes safe. The Blueprint
 *   tier's stricter "never sanitize" rule still holds there, because it has no map to carry the
 *   declared name.
 * - A shape UHT cannot express — a container whose element is itself a container — becomes an
 *   `FFlockJsonData` handle. A near-miss type here does not fail at runtime; it fails the user's build.
 * - Nested objects are emitted as **separate top-level structs**, because UHT does not accept a `USTRUCT`
 *   declared inside another, and in dependency order, because C++ needs the definition first.
 */
class FLOCKEDITOR_API FFlockCppTypeEmitter
{
public:
	struct FEmitResult
	{
		bool bSucceeded = false;
		FString Error;

		int32 StructCount = 0;
		int32 EnumCount = 0;

		/** Header file names written, relative to the module's Public folder. */
		TArray<FString> Headers;

		TArray<FString> Warnings;
	};

	/**
	 * Writes every generated header into `<GeneratedRoot>/Public/`, replacing what a previous sync put
	 * there. `ModuleName` supplies the `<MODULE>_API` export macro the structs need.
	 */
	static FEmitResult Emit(const FFlockSchemaSnapshot& Snapshot, const FString& GeneratedRoot,
		const FString& ModuleName);

	/** "Player Level" + "Template" -> "FPlayerLevelTemplate". */
	static FString MakeStructName(const FString& EntityName, const TCHAR* Suffix);

	/**
	 * True when a declared field name can be a C++ member: an identifier, and not a reserved word.
	 *
	 * Stricter than the Blueprint tier's check because C++ has keywords and Blueprint does not — a field
	 * legitimately declared `class` or `operator` is emittable there and not here.
	 */
	static bool IsUsableMemberName(const FString& DeclaredName);
};
