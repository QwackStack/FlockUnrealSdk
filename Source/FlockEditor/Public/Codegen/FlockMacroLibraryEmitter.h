// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Codegen/FlockFunctionLibraryEmitter.h"
#include "Codegen/FlockSchemaSnapshot.h"

class UBlueprint;
class UUserDefinedStruct;

/**
 * Emits the one-node-per-entity surface: a Blueprint **macro** library where `Get Gameplay` fetches,
 * waits, and hands back a typed struct.
 *
 * This is the point of codegen, and it is why the emitter exists at all. The generated *function* library
 * can only supply the pieces around a fetch — the id, the conversion — leaving a graph to wire the fetch
 * itself and pass an id in, which is barely better than reading by dotted path. A macro can hold the
 * fetch, so the whole thing collapses to one node.
 *
 * Why a macro and not a function: `UK2Node_BaseAsyncTask::IsCompatibleWithGraph` permits an async action
 * node only in an event graph or a macro. Every SDK fetch is one, so a function structurally cannot
 * contain a fetch. That single engine rule is what shapes this whole tier.
 *
 * **Parented to `AActor`.** The SDK's async nodes take a world context that Blueprint fills from `self`,
 * which only resolves when the macro's context is an actor. That covers Actors, ActorComponents, and the
 * Level Blueprint — i.e. essentially all gameplay graphs — but it does mean these macros cannot be used
 * from a plain `UObject` Blueprint. The function library remains available there.
 *
 * ## What gets emitted
 *
 * | Macro | Per | Shape |
 * |---|---|---|
 * | `Get <Config>` | game config | fetch → typed struct |
 * | `Get <Template>` | player template | fetch → typed struct **+ the row id** |
 * | `Save <Template>` | player template | typed struct + row id → update |
 * | `Purchase` / `Unlock Achievement` / `Add Funds` | once each | generated enum in, command out |
 *
 * **`Get <Template>` outputs the row id because `Save` needs it and nothing else can supply it** — the id
 * is per-player, and only the fetch knows it. It is deliberately *not* a member of the generated struct:
 * there it would surface in a Break as though it were part of the template's data, and the write-side
 * conversion would have to learn to exclude it.
 *
 * The command macros are one per *family* keyed by a generated enum, not one per shop item. A per-entity
 * macro (`Purchase Gem Pack`) reads better in the action menu but emits one macro per catalog row and
 * leaves the generated enums with nothing to do; one typed pin is the same number of clicks and does not
 * grow with the catalog.
 */
class FLOCKEDITOR_API FFlockMacroLibraryEmitter
{
public:
	static const TCHAR* const LibraryAssetName;

	struct FEmitResult
	{
		UBlueprint* Library = nullptr;
		int32 MacroCount = 0;
		TArray<FString> Warnings;

		bool IsValid() const { return Library != nullptr; }
	};

	/**
	 * Builds the macro library into Outer and compiles it, without saving.
	 *
	 * Takes the function library emitter's whole result rather than picking pieces out of it: every macro
	 * is a wrapper around functions that emitter produced, and it owns their names, their enums, and which
	 * entities got one. Re-deriving any of that here is how the two libraries would silently drift apart.
	 */
	static FEmitResult BuildLibrary(const FFlockSchemaSnapshot& Snapshot, UObject* Outer,
		const FFlockFunctionLibraryEmitter::FEmitResult& Functions);

	/** Builds, compiles, and saves the macro library under a package path. */
	static FEmitResult Emit(const FFlockSchemaSnapshot& Snapshot, const FString& ContentPath,
		const FFlockFunctionLibraryEmitter::FEmitResult& Functions, FString& OutError);

	/** "Gameplay" -> "GetGameplay". */
	static FString MakeGetMacroName(const FString& EntityName);

	/** "Wallet" -> "SaveWallet". */
	static FString MakeSaveMacroName(const FString& EntityName);

	/** Fixed names for the command macros. Unprefixed — the SDK's own nodes all carry a "Flock " prefix,
	 *  so these do not collide, and they sit under Flock > Generated in the action menu regardless. */
	static const TCHAR* const PurchaseMacroName;
	static const TCHAR* const UnlockAchievementMacroName;
	static const TCHAR* const AddFundsMacroName;
};
