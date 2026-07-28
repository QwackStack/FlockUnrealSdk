// Copyright 2022, Qwacks. All Rights Reserved.

#include "Codegen/FlockCatalogBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Codegen/FlockContentCatalog.h"
#include "Codegen/FlockSchemaHasher.h"
#include "FileHelpers.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

const TCHAR* const FFlockCatalogBuilder::AssetName = TEXT("FlockContentCatalog");

namespace
{
	/** The tag whose template supplies the unlockable achievement names. */
	const TCHAR* const AchievementTag = TEXT("achievement");

	/** Sorts by id then name — the ordering the hasher and the emitters both use. */
	template <typename T>
	TArray<const T*> SortedById(const TArray<T>& Items)
	{
		TArray<const T*> Sorted;
		Sorted.Reserve(Items.Num());
		for (const T& Item : Items)
		{
			Sorted.Add(&Item);
		}
		Sorted.Sort([](const T& A, const T& B)
		{
			return A.Id == B.Id ? A.Name < B.Name : A.Id < B.Id;
		});
		return Sorted;
	}
}

TArray<FFlockCatalogField> FFlockCatalogBuilder::ReadFields(const FString& SchemaJson)
{
	TArray<FFlockCatalogField> Fields;
	if (SchemaJson.IsEmpty())
	{
		return Fields;
	}

	TArray<TSharedPtr<FJsonValue>> Entries;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(SchemaJson);
	if (!FJsonSerializer::Deserialize(Reader, Entries))
	{
		// An unreadable schema yields no fields rather than a failure: one malformed template must not
		// stop a designer browsing the rest of the game.
		return Fields;
	}

	for (const TSharedPtr<FJsonValue>& Entry : Entries)
	{
		const TSharedPtr<FJsonObject> Object = Entry.IsValid() ? Entry->AsObject() : nullptr;
		if (!Object.IsValid())
		{
			continue;
		}

		FFlockCatalogField Field;
		// field_name is the declared spelling — the one a write has to use. Skipping an entry without one
		// is right: it could not be written to anyway.
		if (!Object->TryGetStringField(TEXT("field_name"), Field.Name) || Field.Name.IsEmpty())
		{
			continue;
		}
		Object->TryGetStringField(TEXT("type"), Field.Type);
		Object->TryGetStringField(TEXT("type_name"), Field.TypeName);
		Fields.Add(MoveTemp(Field));
	}
	return Fields;
}

void FFlockCatalogBuilder::Populate(const FFlockSchemaSnapshot& Snapshot, UFlockContentCatalog& OutCatalog)
{
	OutCatalog.GameVersionId = Snapshot.GameVersionId;
	OutCatalog.GeneratedAtUtc = Snapshot.FetchedAtUtc.ToIso8601();
	OutCatalog.ContentHash = FFlockSchemaHasher::ComputeContentHash(Snapshot);

	OutCatalog.PlayerTemplates.Reset();
	OutCatalog.GameConfigs.Reset();
	OutCatalog.Shops.Reset();
	OutCatalog.Currencies.Reset();
	OutCatalog.Achievements.Reset();

	for (const FFlockPlayerTemplateSchema* Template : SortedById(Snapshot.PlayerTemplates))
	{
		FFlockCatalogTemplate Entry;
		Entry.Id = Template->Id;
		Entry.Name = Template->Name;
		Entry.Tag = Template->Tag;
		Entry.Fields = ReadFields(Template->SchemaJson);

		// The achievement-tagged template's fields *are* the achievement names. Taken from the first
		// match: tags are unique per the provider contract.
		if (OutCatalog.Achievements.IsEmpty() && Entry.Tag.Equals(AchievementTag, ESearchCase::IgnoreCase))
		{
			for (const FFlockCatalogField& Field : Entry.Fields)
			{
				OutCatalog.Achievements.AddUnique(Field.Name);
			}
		}
		OutCatalog.PlayerTemplates.Add(MoveTemp(Entry));
	}

	for (const FFlockGameConfigSchema* Config : SortedById(Snapshot.GameConfigs))
	{
		FFlockCatalogConfig Entry;
		Entry.Id = Config->Id;
		Entry.Name = Config->Name;
		Entry.Tag = Config->Tag;
		Entry.Fields = ReadFields(Config->SchemaJson);
		OutCatalog.GameConfigs.Add(MoveTemp(Entry));
	}

	for (const FFlockShop* Shop : SortedById(Snapshot.Shops))
	{
		FFlockCatalogShop Entry;
		Entry.Id = Shop->Id;
		Entry.Name = Shop->Name;
		for (const FFlockShopItem* Item : SortedById(Shop->ShopItems))
		{
			FFlockCatalogShopItem ItemEntry;
			ItemEntry.Id = Item->Id;
			ItemEntry.Name = Item->Name;
			ItemEntry.Currency = Item->Currency;
			ItemEntry.PriceAtSync = Item->Price;
			Entry.Items.Add(MoveTemp(ItemEntry));

			if (!Item->Currency.IsEmpty())
			{
				OutCatalog.Currencies.AddUnique(Item->Currency);
			}
		}
		OutCatalog.Shops.Add(MoveTemp(Entry));
	}

	// Sorted rather than in first-seen order, so the asset does not churn when the backend reorders shops.
	OutCatalog.Currencies.Sort();
	OutCatalog.Achievements.Sort();
}

bool FFlockCatalogBuilder::Save(const FFlockSchemaSnapshot& Snapshot, const FString& ContentPath, FString& OutError)
{
	FString Root = ContentPath.TrimStartAndEnd();
	if (Root.IsEmpty())
	{
		OutError = TEXT("Generated Content Path is empty in Project Settings > Flock SDK.");
		return false;
	}
	if (!Root.StartsWith(TEXT("/")))
	{
		OutError = FString::Printf(
			TEXT("Generated Content Path must be a package path such as /Game/Flock/Generated. Got '%s'."), *Root);
		return false;
	}
	Root.RemoveFromEnd(TEXT("/"));

	const FString PackageName = FString::Printf(TEXT("%s/%s"), *Root, AssetName);
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Could not create package '%s'."), *PackageName);
		return false;
	}
	Package->FullyLoad();

	// Reuse the existing asset when there is one, so references from other assets survive a re-sync.
	UFlockContentCatalog* Catalog = FindObject<UFlockContentCatalog>(Package, AssetName);
	const bool bIsNew = Catalog == nullptr;
	if (bIsNew)
	{
		Catalog = NewObject<UFlockContentCatalog>(Package, UFlockContentCatalog::StaticClass(), AssetName,
			RF_Public | RF_Standalone);
	}
	if (!Catalog)
	{
		OutError = FString::Printf(TEXT("Could not create the catalog asset in '%s'."), *PackageName);
		return false;
	}

	Populate(Snapshot, *Catalog);
	Catalog->MarkPackageDirty();
	if (bIsNew)
	{
		FAssetRegistryModule::AssetCreated(Catalog);
	}

	if (!UEditorLoadingAndSavingUtils::SavePackages({ Package }, /*bOnlyDirty*/ false))
	{
		OutError = FString::Printf(TEXT("Could not save '%s'."), *PackageName);
		return false;
	}

	OutError.Reset();
	return true;
}
