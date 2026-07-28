// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockStructuredData.h"

/**
 * Binds a provider's flattened data to an arbitrary struct, and back again — the runtime half of codegen.
 *
 * Generated types come in two flavours and this serves both: a C++ `USTRUCT` from the generated module,
 * and a `UUserDefinedStruct` authored as a Blueprint asset (which is how a Blueprint-only project gets
 * typed pins without compiling anything). Neither is known to the SDK at compile time, so everything here
 * goes through reflection on a `UStruct` the caller supplies.
 *
 * **Why not FJsonObjectConverter::JsonObjectToUStruct.** It gets most of the way — it resolves a Blueprint
 * struct's mangled property names (a member authored `Level` is stored as `Level_2_<GUID>`) and matches
 * case-insensitively, so `{"Level":…}` and `{"level":…}` both bind. What it cannot do is bridge a
 * *spelling* difference: a source key of `max_health` leaves a `MaxHealth` member untouched. That case is
 * real — the DataField flatten Pascal-cases field names, but a legacy flat row or a dict-typed field keeps
 * author keys verbatim — so binding here indexes both spellings from both directions.
 *
 * **Names on the way out are the wire's, not the member's.** A write is matched against the player
 * template exactly, so `ToCommandData` emits the declared name. For a Blueprint struct the members are
 * *named* with the declared names, so no mapping is needed; the generated C++ tier uses PascalCase members
 * and passes the map its emitter baked. Either way the caller never has to know the difference — which is
 * the whole reason codegen exists (see the commands feature notes on `template_validation_failed`).
 *
 * Pinned by `Flock.Codegen.Binder.*` for the C++ path and `Flock.Editor.CodegenSpike.*` for the
 * Blueprint-struct path.
 */
class FLOCK_API FFlockStructBinder
{
public:
	/** Fills a struct instance from a row's/config's flattened data. Returns the number of members bound. */
	static int32 FillStruct(const UStruct* Struct, void* StructMemory, const FFlockStructuredData& Data);

	/** FillStruct against an already-parsed object, for a caller that has one in hand. */
	static int32 FillStructFromObject(const UStruct* Struct, void* StructMemory, const TSharedRef<FJsonObject>& Source);

	/**
	 * Reads a struct instance into a command bag. Keys are the members' own names — correct when the
	 * struct's members are named as the template declares them, which is how the Blueprint tier emits.
	 */
	static FFlockCommandData ToCommandData(const UStruct* Struct, const void* StructMemory);

	/**
	 * ToCommandData with an explicit member → declared-name map, for generated C++ structs whose members
	 * are PascalCase. A member absent from the map falls back to its own name.
	 */
	static FFlockCommandData ToCommandData(const UStruct* Struct, const void* StructMemory,
		const TMap<FString, FString>& DeclaredNameByMember);

	/**
	 * A member's authored name: the display name for a Blueprint struct (stripping the GUID suffix the
	 * property carries), the plain field name for a C++ one.
	 */
	static FString GetMemberName(const UStruct* Struct, const FProperty* Property);
};
