// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Codegen/FlockSchemaSnapshot.h"

/**
 * The C++ target's call surface: `FFlockGenerated::GetWallet(...)`, `SaveWallet(...)`, `Purchase(...)`.
 *
 * One generated struct holding static functions, so a call site reads as a namespace and nothing has to
 * be constructed or kept alive. Every function resolves the SDK from a world context object, so no call
 * site passes a subsystem or a provider.
 *
 * ## The row id is a parameter, not a member
 *
 * `Get<Template>` hands back `(struct, row id)` and `Save<Template>` takes both — the row id never lives
 * on the generated struct.
 *
 * The canonical SDK does the opposite, carrying `PlayerDataId` on the generated class so `UpdateAsync()`
 * needs no arguments, and the original design here copied that. It is wrong for Unreal for two reasons
 * that only became clear once the Blueprint tier existed. Generated structs are `BlueprintType`, so a
 * member would surface in a Break node reading exactly like template data — which is why the Blueprint
 * tier made it a pin. And the write-side conversion reflects over *every* member, so a `PlayerDataId`
 * member would have to be specially excluded; a special case in the one code path whose bugs are
 * server-rejected writes is a poor trade for one fewer argument.
 *
 * ## No generated `UFUNCTION`s
 *
 * A Blueprint-callable async node is a `UCLASS` per entity, and generating those would duplicate what the
 * Blueprint target already does well. A project on the C++ target still gets its typed structs in
 * Blueprint — they are `BlueprintType` — and drives them through the SDK's existing wildcard nodes
 * (*Flock Data To Struct* / *Flock Struct To Command Data*), which work on any struct. That is the
 * function-library chain rather than the one-node macro, which is the honest cost of picking this target.
 */
class FLOCKEDITOR_API FFlockCppAccessorEmitter
{
public:
	struct FEmitResult
	{
		bool bSucceeded = false;
		FString Error;

		/** Functions written across the header. */
		int32 FunctionCount = 0;

		TArray<FString> Warnings;
	};

	/**
	 * Writes `<Module>Accessors.h` / `.cpp` into the generated root.
	 *
	 * Needs the same struct names the type emitter produced; it re-derives them through
	 * `FFlockCppTypeEmitter::MakeStructName` rather than being handed a map, because both are pure
	 * functions of the entity name and a map would be a second thing to keep in step.
	 */
	static FEmitResult Emit(const FFlockSchemaSnapshot& Snapshot, const FString& GeneratedRoot,
		const FString& ModuleName);
};
