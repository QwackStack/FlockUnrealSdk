// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/** A declared field's shape, independent of what any emitter turns it into. */
enum class EFlockSchemaKind : uint8
{
	String,
	Int,
	Int64,
	Float,
	Double,
	Bool,
	Object,
	List,
	Dict,
	/** Not a shape the SDK knows; every emitter degrades this to an opaque handle rather than guessing. */
	Unknown,
};

/**
 * The parts of reading a template/config `schema` that both targets share.
 *
 * Extracted because the Blueprint tier and the C++ tier must agree on **which wire types exist** and how
 * a schema nests. They disagree only on what a field becomes — a pin type or a line of C++ — and that
 * split is the whole of the difference between them. Two private copies of this table would drift the
 * first time the backend grew a type, and the symptom would be one target silently degrading a field the
 * other handled.
 */
struct FLOCKEDITOR_API FFlockSchemaTypes
{
	/** Guards against a schema that nests into itself; real ones are shallow. */
	static constexpr int32 MaxNestingDepth = 8;

	/**
	 * Wire type name → kind.
	 *
	 * Datetimes classify as String on purpose: the whole SDK carries ISO-8601 timestamps as strings
	 * (`CreatedAt`/`UpdatedAt` on every model), and diverging here would make generated models
	 * inconsistent with hand-written ones.
	 *
	 * A trailing `?` (the dashboard's nullable marker, "datetime?") is stripped first — it says nothing
	 * about the type, and neither target has a nullable scalar. Without that, *every* optional field
	 * degraded to an opaque handle; found by syncing a real backend, not by reading.
	 */
	static EFlockSchemaKind Classify(const FString& WireType);

	/** A template's or config's `schema` string as its top-level field list. */
	static TArray<TSharedPtr<FJsonValue>> ParseSchemaArray(const FString& SchemaJson);

	/** A field's `schema` child: a list (an object's body) or a single descriptor (a list/dict element). */
	static TArray<TSharedPtr<FJsonValue>> ChildEntries(const TSharedRef<FJsonObject>& Field);
};
