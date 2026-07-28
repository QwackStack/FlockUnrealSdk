// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Codegen/FlockSchemaSnapshot.h"

class UBlueprint;
class UUserDefinedStruct;
class UEnum;

/** One generated enum and the wire values behind its members, as the enum emitter produced them. */
struct FLOCKEDITOR_API FFlockEnumLookupSpec
{
	/** The generated function's name, e.g. "ShopItemId". */
	FString FunctionName;
	UEnum* Enum = nullptr;
	TArray<TPair<FString, FString>> Members;
};

/**
 * Emits a Blueprint function library asset holding one function per backend entity, so a graph names
 * things instead of typing them.
 *
 * **What this deliberately is not.** The obvious design — a single `Get Player Progress` node that
 * fetches, waits, and hands back a typed struct — is not expressible. The SDK's provider calls are async
 * action nodes, and `UK2Node_BaseAsyncTask::IsCompatibleWithGraph` permits those only in an event graph
 * or a macro, never in a function. So a generated *function* cannot wrap a fetch. What it can do is
 * remove every hand-typed string around one: the ids, the struct conversions, and the enum→wire lookups.
 *
 * A graph therefore reads:
 *
 *     Flock Get My Data By Template (Wallet Template Id) → To Wallet → Set members → From Wallet
 *         → Flock Update Player Data
 *
 * — the async nodes stay the SDK's, and everything between them is generated and typed. Wrapping the
 * async half into one node per entity is possible as a *macro* library (macros are permitted), which is
 * the natural follow-on; functions come first because the glue is needed either way.
 */
class FLOCKEDITOR_API FFlockFunctionLibraryEmitter
{
public:
	/** The generated library's asset name. */
	static const TCHAR* const LibraryAssetName;

	struct FEmitResult
	{
		UBlueprint* Library = nullptr;

		/** Functions written. */
		int32 FunctionCount = 0;

		TArray<FString> Warnings;

		/**
		 * Source entity id → the `Read…` function emitted for it. The macro emitter consumes this rather
		 * than re-deriving names, so a rename here cannot silently desynchronise the two libraries.
		 */
		TMap<FString, FString> ReadFunctionByEntityId;

		/**
		 * Source entity id → the `Make…Update` function emitted for it, same contract as the read map.
		 * Only player templates appear here; a config has no write side (see MakeFromStructFunctionName).
		 */
		TMap<FString, FString> WriteFunctionByEntityId;

		/**
		 * Lookup function name → the generated enum it takes. The command macros need both — the function
		 * to call and the enum to type their input pin with — and neither is derivable from the other.
		 */
		TMap<FString, UEnum*> LookupEnumByFunctionName;

		bool IsValid() const { return Library != nullptr; }
	};

	/**
	 * Builds the library into Outer and compiles it, without saving. The testable half — a compiled
	 * Blueprint can be invoked by reflection, so the generated graphs are checked by running them rather
	 * than by inspecting their nodes.
	 *
	 * StructsById comes from the struct emitter. Where an entity has a generated struct, the library also
	 * gets its `To…`/`From…` conversions; without it, only the id constants are emitted.
	 */
	static FEmitResult BuildLibrary(const FFlockSchemaSnapshot& Snapshot, UObject* Outer,
		const TMap<FString, UUserDefinedStruct*>& StructsById = TMap<FString, UUserDefinedStruct*>(),
		const TArray<struct FFlockEnumLookupSpec>& EnumLookups = TArray<struct FFlockEnumLookupSpec>());

	/** Builds, compiles, and saves the library under a package path. */
	static FEmitResult Emit(const FFlockSchemaSnapshot& Snapshot, const FString& ContentPath,
		const TMap<FString, UUserDefinedStruct*>& StructsById, const TArray<FFlockEnumLookupSpec>& EnumLookups,
		FString& OutError);

	/** "WalletTemplate" -> "ReadWalletTemplate": data handle in, typed struct out. */
	static FString MakeToStructFunctionName(const FString& EntityName);

	/**
	 * "WalletTemplate" -> "MakeWalletTemplateUpdate": typed struct in, command body out. Emitted only for
	 * player templates — a game config is admin-only, so a client that "wrote" one would just be rejected.
	 */
	static FString MakeFromStructFunctionName(const FString& EntityName);

	/** "Wallet" -> "WalletTemplateId": the function that returns a template's baked id. */
	static FString MakeTemplateIdFunctionName(const FString& TemplateName);

	/** "Gameplay" -> "GameplayConfigId". */
	static FString MakeConfigIdFunctionName(const FString& ConfigName);
};
