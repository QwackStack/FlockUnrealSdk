// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Codegen/FlockSchemaSnapshot.h"

class UUserDefinedEnum;

/**
 * Emits a Blueprint enum per set of backend identifiers — shop items, currencies, achievements — so a
 * graph picks one from a typed dropdown instead of typing a string that is only checked at runtime.
 *
 * The point is the same as the struct emitter's, applied to ids rather than fields: a mis-typed
 * achievement name currently fails against a live backend with `game_command.achievement_not_found`,
 * which is a slow and expensive way to find a typo. An enum makes it unpickable.
 *
 * **A member's display name is not its wire value.** A shop item shows as `GemPack` but purchases by an
 * opaque id; an achievement shows as `FirstWin` but unlocks by `first_win`. So every emitted enum comes
 * back with its display→wire mapping, which the function-library emitter bakes into the generated nodes.
 * Nothing at runtime has to look the mapping up, which is what keeps the catalog asset out of cooked
 * builds.
 */
class FLOCKEDITOR_API FFlockEnumEmitter
{
public:
	/** Asset names, matching the canonical SDK's enum names. */
	static const TCHAR* const ShopItemEnumName;
	static const TCHAR* const CurrencyEnumName;
	static const TCHAR* const AchievementEnumName;

	/** One emitted enum and the wire value behind each of its members, in declaration order. */
	struct FEnumResult
	{
		UUserDefinedEnum* Enum = nullptr;

		/** Display name → the id or name the SDK actually sends. Empty when nothing was emitted. */
		TArray<TPair<FString, FString>> WireValueByDisplayName;

		bool IsValid() const { return Enum != nullptr; }
	};

	struct FEmitResult
	{
		FEnumResult ShopItems;
		FEnumResult Currencies;
		FEnumResult Achievements;

		TArray<FString> Warnings;

		int32 EnumCount() const
		{
			return (ShopItems.IsValid() ? 1 : 0) + (Currencies.IsValid() ? 1 : 0) + (Achievements.IsValid() ? 1 : 0);
		}
	};

	/**
	 * Builds one enum from ordered (display name, wire value) pairs. Returns an invalid result for an
	 * empty set — an enum with no members is not a useful dropdown, and emitting one would leave a
	 * confusing asset behind for a game that simply has no shops.
	 */
	static FEnumResult BuildEnum(UObject* Outer, const FString& EnumName,
		const TArray<TPair<FString, FString>>& Members, TArray<FString>& OutWarnings);

	/** Builds every enum into Outer, without saving. The testable half. */
	static FEmitResult BuildAll(const FFlockSchemaSnapshot& Snapshot, UObject* Outer);

	/** Builds and saves the enum assets under a package path. */
	static FEmitResult Emit(const FFlockSchemaSnapshot& Snapshot, const FString& ContentPath, FString& OutError);

	// ── The member sets, exposed so their derivation is testable without building assets ──

	/** Every shop item across every shop: display name from the item's name, wire value its id. */
	static TArray<TPair<FString, FString>> CollectShopItems(const FFlockSchemaSnapshot& Snapshot);

	/** Every distinct currency any item is priced in; the currency name is both display and wire value. */
	static TArray<TPair<FString, FString>> CollectCurrencies(const FFlockSchemaSnapshot& Snapshot);

	/** The "achievement"-tagged template's fields; the declared field name is the wire value. */
	static TArray<TPair<FString, FString>> CollectAchievements(const FFlockSchemaSnapshot& Snapshot);

	/** "gem pack" -> "GemPack". A leading digit is prefixed, since a member name cannot start with one. */
	static FString MakeMemberName(const FString& SourceName);
};
