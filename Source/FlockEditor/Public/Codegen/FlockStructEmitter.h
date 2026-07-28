// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Codegen/FlockSchemaSnapshot.h"

class UUserDefinedStruct;
struct FEdGraphPinType;

/**
 * Emits a Blueprint struct asset per player template and game config, so a graph reads a row through
 * typed pins instead of string paths — and writes one back the same way.
 *
 * A `UUserDefinedStruct` is authored, not compiled, which is the whole point: a Blueprint-only project
 * gets the typed surface with no C++ toolchain, no project-file surgery, and no editor restart. The
 * mechanism was proven before any of this was written (`Flock.Editor.CodegenSpike.*`).
 *
 * **Members carry the template's declared names**, not the flattened spellings a read hands back. That is
 * what lets the update path send a body the server accepts without carrying a member→wire map, which a
 * Blueprint struct has nowhere to keep (property metadata is stripped from packaged builds). A declared
 * name that is not a legal member name is *skipped with a warning* rather than sanitized: a renamed
 * member would silently produce writes the server rejects, and a missing field a designer can see beats a
 * broken one they cannot.
 */
class FLOCKEDITOR_API FFlockStructEmitter
{
public:
	/** Suffixes matching the canonical SDK's generated class names. */
	static const TCHAR* const TemplateSuffix;
	static const TCHAR* const ConfigSuffix;

	struct FEmitResult
	{
		/** Structs written, including the nested ones an `object` field produces. */
		int32 StructCount = 0;

		/** Fields that could not be emitted, each naming what and why. Never fatal. */
		TArray<FString> Warnings;

		/** Source entity id → generated struct asset name, for the emitters that follow. */
		TMap<FString, FString> StructNameById;

		/**
		 * Source entity id → the struct itself. The function-library emitter needs the type, not just the
		 * name: it bakes it into a wildcard pin, which cannot be resolved from a string.
		 */
		TMap<FString, UUserDefinedStruct*> StructById;
	};

	/**
	 * Builds one struct from a verbatim `schema`. Nested `object` fields become their own structs inside
	 * the same Outer, named `<StructName><FieldName>`. Null only when the struct itself cannot be created.
	 */
	static UUserDefinedStruct* BuildStruct(UObject* Outer, const FString& StructName, const FString& SchemaJson,
		TArray<FString>& OutWarnings);

	/** Builds every template and config struct into Outer, without saving. The testable half. */
	static FEmitResult BuildAll(const FFlockSchemaSnapshot& Snapshot, UObject* Outer);

	/** Builds and saves the struct assets under a package path. */
	static FEmitResult Emit(const FFlockSchemaSnapshot& Snapshot, const FString& ContentPath, FString& OutError);

	/** "Player Progress" + "Template" -> "PlayerProgressTemplate". Empty name yields "Unnamed<Suffix>". */
	static FString MakeStructName(const FString& EntityName, const FString& Suffix);

	/**
	 * True when a declared field name can be a struct member as-is. Anything else is skipped, because
	 * altering it would break the write path this whole tier exists to get right.
	 */
	static bool IsUsableMemberName(const FString& DeclaredName);
};
