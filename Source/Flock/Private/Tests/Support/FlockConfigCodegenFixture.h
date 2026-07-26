// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FlockConfigCodegenFixture.generated.h"

/**
 * Stand-ins for what codegen will emit from a config schema: reflected USTRUCTs with PascalCase fields.
 * They exist so a test can prove FFlockGameConfigData::GetDataAs<T> binds the shapes a real generated
 * config has — a nested struct, a list, and a dictionary — end to end. Defining them here (not in the
 * test .cpp) is required: UHT only processes reflected types declared in headers. Not guarded by
 * WITH_AUTOMATION_TESTS on purpose — UHT generates their registration unconditionally.
 */
USTRUCT()
struct FFlockCodegenStatsFixture
{
	GENERATED_BODY()

	UPROPERTY()
	int32 MaxHealth = 0;

	UPROPERTY()
	float CritChance = 0.f;
};

USTRUCT()
struct FFlockCodegenConfigFixture
{
	GENERATED_BODY()

	UPROPERTY()
	int32 MaxHealth = 0;

	UPROPERTY()
	FString BossName;

	UPROPERTY()
	bool Hardcore = false;

	/** Bound from an `object` node — field names are Pascal, so this nested struct binds by reflection. */
	UPROPERTY()
	FFlockCodegenStatsFixture Stats;

	/** Bound from a `list` node. */
	UPROPERTY()
	TArray<FString> Tiers;

	/** Bound from a `dict` node — author keys stay verbatim, which is exactly what a map key should be. */
	UPROPERTY()
	TMap<FString, int32> LootTable;
};
