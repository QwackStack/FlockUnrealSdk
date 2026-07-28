// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Codegen/FlockEnumEmitter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Codegen/FlockCatalogBuilder.h"
#include "Codegen/FlockContentCatalog.h"
#include "Engine/UserDefinedEnum.h"
#include "FileHelpers.h"
#include "Kismet2/EnumEditorUtils.h"
#include "UObject/Package.h"

const TCHAR* const FFlockEnumEmitter::ShopItemEnumName = TEXT("FlockShopItemId");
const TCHAR* const FFlockEnumEmitter::CurrencyEnumName = TEXT("FlockCurrencyId");
const TCHAR* const FFlockEnumEmitter::AchievementEnumName = TEXT("FlockAchievementId");

namespace
{
	const TCHAR* const AchievementTag = TEXT("achievement");

	/** Sorts by id then name, matching the hasher, the catalog, and the struct emitter. */
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

	/** Appends a member, giving a name that collides a numeric suffix rather than dropping it. */
	void AddUnique(TArray<TPair<FString, FString>>& Members, TSet<FString>& UsedNames,
		const FString& DisplayName, const FString& WireValue)
	{
		FString Candidate = DisplayName;
		int32 Suffix = 2;
		while (UsedNames.Contains(Candidate))
		{
			Candidate = FString::Printf(TEXT("%s_%d"), *DisplayName, Suffix++);
		}
		UsedNames.Add(Candidate);
		Members.Emplace(Candidate, WireValue);
	}
}

FString FFlockEnumEmitter::MakeMemberName(const FString& SourceName)
{
	FString Pascal;
	bool bUpperNext = true;
	for (const TCHAR Character : SourceName)
	{
		if (FChar::IsAlnum(Character))
		{
			Pascal.AppendChar(bUpperNext ? FChar::ToUpper(Character) : Character);
			bUpperNext = false;
		}
		else
		{
			bUpperNext = true;
		}
	}
	if (Pascal.IsEmpty())
	{
		return TEXT("Unnamed");
	}
	// A leading digit cannot start an identifier — the same problem the canonical SDK solves with its
	// `_100` currency members.
	return FChar::IsDigit(Pascal[0]) ? TEXT("_") + Pascal : Pascal;
}

TArray<TPair<FString, FString>> FFlockEnumEmitter::CollectShopItems(const FFlockSchemaSnapshot& Snapshot)
{
	TArray<TPair<FString, FString>> Members;
	TSet<FString> UsedNames;
	TSet<FString> SeenIds;

	for (const FFlockShop* Shop : SortedById(Snapshot.Shops))
	{
		for (const FFlockShopItem* Item : SortedById(Shop->ShopItems))
		{
			// Ids are unique game-wide, so the same item listed in two shops is one member.
			if (Item->Id.IsEmpty() || Item->Name.IsEmpty() || SeenIds.Contains(Item->Id))
			{
				continue;
			}
			SeenIds.Add(Item->Id);
			AddUnique(Members, UsedNames, MakeMemberName(Item->Name), Item->Id);
		}
	}
	return Members;
}

TArray<TPair<FString, FString>> FFlockEnumEmitter::CollectCurrencies(const FFlockSchemaSnapshot& Snapshot)
{
	// Sorted and de-duplicated first, so member order does not follow shop order.
	TArray<FString> Names;
	for (const FFlockShop& Shop : Snapshot.Shops)
	{
		for (const FFlockShopItem& Item : Shop.ShopItems)
		{
			if (!Item.Currency.IsEmpty())
			{
				Names.AddUnique(Item.Currency);
			}
		}
	}
	Names.Sort();

	TArray<TPair<FString, FString>> Members;
	TSet<FString> UsedNames;
	for (const FString& Name : Names)
	{
		// The currency's own name is what AddGameFunds sends, so display and wire differ only in casing.
		AddUnique(Members, UsedNames, MakeMemberName(Name), Name);
	}
	return Members;
}

TArray<TPair<FString, FString>> FFlockEnumEmitter::CollectAchievements(const FFlockSchemaSnapshot& Snapshot)
{
	TArray<TPair<FString, FString>> Members;
	TSet<FString> UsedNames;

	for (const FFlockPlayerTemplateSchema* Template : SortedById(Snapshot.PlayerTemplates))
	{
		if (!Template->Tag.Equals(AchievementTag, ESearchCase::IgnoreCase))
		{
			continue;
		}
		// The tagged template's declared fields are the unlockable achievement names.
		for (const FFlockCatalogField& Field : FFlockCatalogBuilder::ReadFields(Template->SchemaJson))
		{
			AddUnique(Members, UsedNames, MakeMemberName(Field.Name), Field.Name);
		}
		break; // tags are unique per the provider contract
	}
	return Members;
}

FFlockEnumEmitter::FEnumResult FFlockEnumEmitter::BuildEnum(UObject* Outer, const FString& EnumName,
	const TArray<TPair<FString, FString>>& Members, TArray<FString>& OutWarnings)
{
	FEnumResult Result;
	if (Members.IsEmpty())
	{
		// Not a warning: a game with no shops legitimately has no shop-item enum, and emitting an empty
		// one would leave a confusing asset behind.
		return Result;
	}

	UEnum* Created = FEnumEditorUtils::CreateUserDefinedEnum(Outer, FName(*EnumName), RF_Public | RF_Standalone);
	UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(Created);
	if (!Enum)
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not create enum '%s'."), *EnumName));
		return Result;
	}

	// One enumerator per member: unlike CreateUserDefinedStruct, which seeds a placeholder variable,
	// CreateUserDefinedEnum returns an enum with no members at all (NumEnums() counts only the implicit
	// _MAX). Assuming symmetry between the two APIs costs exactly one member on every enum.
	for (int32 Index = 0; Index < Members.Num(); ++Index)
	{
		FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
	}

	for (int32 Index = 0; Index < Members.Num(); ++Index)
	{
		// The display name is what a Blueprint dropdown shows and what a graph author reads; the
		// underlying enumerator names stay engine-generated, which is why the wire mapping travels
		// separately rather than being recovered from the enum later.
		if (!FEnumEditorUtils::SetEnumeratorDisplayName(Enum, Index, FText::FromString(Members[Index].Key)))
		{
			OutWarnings.Add(FString::Printf(
				TEXT("'%s': could not name member %d ('%s')."), *EnumName, Index, *Members[Index].Key));
		}
	}

	Result.Enum = Enum;
	Result.WireValueByDisplayName = Members;
	return Result;
}

FFlockEnumEmitter::FEmitResult FFlockEnumEmitter::BuildAll(const FFlockSchemaSnapshot& Snapshot, UObject* Outer)
{
	FEmitResult Result;
	Result.ShopItems = BuildEnum(Outer, ShopItemEnumName, CollectShopItems(Snapshot), Result.Warnings);
	Result.Currencies = BuildEnum(Outer, CurrencyEnumName, CollectCurrencies(Snapshot), Result.Warnings);
	Result.Achievements = BuildEnum(Outer, AchievementEnumName, CollectAchievements(Snapshot), Result.Warnings);
	return Result;
}

FFlockEnumEmitter::FEmitResult FFlockEnumEmitter::Emit(const FFlockSchemaSnapshot& Snapshot,
	const FString& ContentPath, FString& OutError)
{
	FEmitResult Result;

	FString Root = ContentPath.TrimStartAndEnd();
	Root.RemoveFromEnd(TEXT("/"));
	if (Root.IsEmpty() || !Root.StartsWith(TEXT("/")))
	{
		OutError = FString::Printf(
			TEXT("Generated Content Path must be a package path such as /Game/Flock/Generated. Got '%s'."), *ContentPath);
		return Result;
	}

	TArray<UPackage*> Packages;
	auto BuildInto = [&](const FString& EnumName, const TArray<TPair<FString, FString>>& Members) -> FEnumResult
	{
		if (Members.IsEmpty())
		{
			return FEnumResult();
		}
		const FString PackageName = FString::Printf(TEXT("%s/%s"), *Root, *EnumName);
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			Result.Warnings.Add(FString::Printf(TEXT("Could not create package '%s'."), *PackageName));
			return FEnumResult();
		}
		Package->FullyLoad();

		FEnumResult Built = BuildEnum(Package, EnumName, Members, Result.Warnings);
		if (Built.IsValid())
		{
			FAssetRegistryModule::AssetCreated(Built.Enum);
			Built.Enum->MarkPackageDirty();
			Packages.Add(Package);
		}
		return Built;
	};

	Result.ShopItems = BuildInto(ShopItemEnumName, CollectShopItems(Snapshot));
	Result.Currencies = BuildInto(CurrencyEnumName, CollectCurrencies(Snapshot));
	Result.Achievements = BuildInto(AchievementEnumName, CollectAchievements(Snapshot));

	if (!Packages.IsEmpty() && !UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty*/ false))
	{
		OutError = TEXT("Could not save one or more generated enum assets.");
		return Result;
	}

	OutError.Reset();
	return Result;
}
