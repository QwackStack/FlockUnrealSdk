// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "FlockContentCatalog.generated.h"

/**
 * One declared field of a template or config: the name a write must use, and the type the dashboard
 * declared it as.
 *
 * Name is the **declared** spelling, not the flattened one a read hands back — this is the side of that
 * split that the server validates against, so it is the side worth recording.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockCatalogField
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	FString Name;

	/** Structural type: int, string, bool, object, list, dict, … */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	FString Type;

	/** The dashboard's own name for the type, when it supplied one. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	FString TypeName;
};

/** A player template: the shape of one kind of per-player record. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockCatalogTemplate
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	FString Name;

	/** "currency", "achievement", … — how the SDK resolves a row without an id. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	FString Tag;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	TArray<FFlockCatalogField> Fields;
};

/** A game config: game-wide values, read-only to a client. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockCatalogConfig
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	FString Name;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	FString Tag;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	TArray<FFlockCatalogField> Fields;
};

/**
 * A purchasable item. Price is recorded for the designer's benefit only — it is a snapshot of what the
 * backend said at sync time, and the SDK always fetches it live, so nothing should read it as truth.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockCatalogShopItem
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	FString Name;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	FString Currency;

	/** As of the last sync. Fetch the shop for a live price. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	int32 PriceAtSync = 0;
};

/** A shop and the items it sold at sync time. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockCatalogShop
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	FString Name;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock")
	TArray<FFlockCatalogShopItem> Items;
};

/**
 * A browsable, offline picture of everything the backend declares for this game version: templates and
 * their fields, configs and their fields, shops and their items, the currencies in circulation, and the
 * achievements that can be unlocked.
 *
 * Two jobs, both editor-side. It is the **designer's window** onto the dashboard — select it in the
 * Content Browser and read the whole content model in the Details panel, no code and no dashboard login.
 * And it is the **single source of truth for codegen's own emitters and for editor dropdowns**, so ids
 * and declared field names come from one place rather than being re-derived per feature.
 *
 * Nothing references it at runtime, which is what keeps it out of a packaged build: with no reference,
 * it is never cooked. It lives in the runtime module regardless, so an editor picker on a runtime
 * Blueprint node can reach it.
 *
 * Regenerated wholesale on every sync — edits are lost, and its accessors are read-only for that reason.
 */
UCLASS(BlueprintType)
class FLOCK_API UFlockContentCatalog : public UDataAsset
{
	GENERATED_BODY()

public:
	/** The game version this was synced for. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock|Sync")
	FString GameVersionId;

	/** ISO-8601, UTC. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock|Sync")
	FString GeneratedAtUtc;

	/** Matches the manifest's; a difference means this asset is from a different sync. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock|Sync")
	FString ContentHash;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock|Player")
	TArray<FFlockCatalogTemplate> PlayerTemplates;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock|Config")
	TArray<FFlockCatalogConfig> GameConfigs;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock|Shop")
	TArray<FFlockCatalogShop> Shops;

	/** Every distinct currency any shop item is priced in. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock|Shop")
	TArray<FString> Currencies;

	/** The fields of the "achievement"-tagged template — the names Unlock Achievement accepts. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flock|Player")
	TArray<FString> Achievements;

	// ── Lookups. Editor pickers and emitters read through these rather than walking the arrays. ──

	UFUNCTION(BlueprintPure, Category = "Flock|Catalog")
	TArray<FString> GetTemplateNames() const;

	UFUNCTION(BlueprintPure, Category = "Flock|Catalog")
	TArray<FString> GetConfigNames() const;

	UFUNCTION(BlueprintPure, Category = "Flock|Catalog")
	TArray<FString> GetShopNames() const;

	/** Every item across every shop; ids are unique game-wide. */
	UFUNCTION(BlueprintPure, Category = "Flock|Catalog")
	TArray<FString> GetShopItemNames() const;

	/** The declared field names of a template, by template name. Empty when there is no such template. */
	UFUNCTION(BlueprintPure, Category = "Flock|Catalog")
	TArray<FString> GetTemplateFieldNames(const FString& TemplateName) const;

	UFUNCTION(BlueprintPure, Category = "Flock|Catalog")
	TArray<FString> GetConfigFieldNames(const FString& ConfigName) const;

	/** The template carrying a tag ("currency", "achievement"); null when none does. */
	const FFlockCatalogTemplate* FindTemplateByTag(const FString& Tag) const;
	const FFlockCatalogTemplate* FindTemplateByName(const FString& Name) const;
	const FFlockCatalogConfig* FindConfigByName(const FString& Name) const;

	/** The id of a shop item by name; empty when unknown. */
	UFUNCTION(BlueprintPure, Category = "Flock|Catalog")
	FString FindShopItemId(const FString& ItemName) const;

	/** True when the catalog holds nothing — a sync against an empty game, or a stale placeholder. */
	UFUNCTION(BlueprintPure, Category = "Flock|Catalog")
	bool IsEmptyCatalog() const;
};
