// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Codegen/FlockContentCatalog.h"

namespace
{
	template <typename T>
	TArray<FString> NamesOf(const TArray<T>& Items)
	{
		TArray<FString> Names;
		Names.Reserve(Items.Num());
		for (const T& Item : Items)
		{
			if (!Item.Name.IsEmpty())
			{
				Names.Add(Item.Name);
			}
		}
		return Names;
	}

	TArray<FString> FieldNamesOf(const TArray<FFlockCatalogField>& Fields)
	{
		TArray<FString> Names;
		Names.Reserve(Fields.Num());
		for (const FFlockCatalogField& Field : Fields)
		{
			if (!Field.Name.IsEmpty())
			{
				Names.Add(Field.Name);
			}
		}
		return Names;
	}
}

TArray<FString> UFlockContentCatalog::GetTemplateNames() const
{
	return NamesOf(PlayerTemplates);
}

TArray<FString> UFlockContentCatalog::GetConfigNames() const
{
	return NamesOf(GameConfigs);
}

TArray<FString> UFlockContentCatalog::GetShopNames() const
{
	return NamesOf(Shops);
}

TArray<FString> UFlockContentCatalog::GetShopItemNames() const
{
	TArray<FString> Names;
	for (const FFlockCatalogShop& Shop : Shops)
	{
		for (const FFlockCatalogShopItem& Item : Shop.Items)
		{
			if (!Item.Name.IsEmpty())
			{
				Names.AddUnique(Item.Name);
			}
		}
	}
	return Names;
}

TArray<FString> UFlockContentCatalog::GetTemplateFieldNames(const FString& TemplateName) const
{
	const FFlockCatalogTemplate* Template = FindTemplateByName(TemplateName);
	return Template ? FieldNamesOf(Template->Fields) : TArray<FString>();
}

TArray<FString> UFlockContentCatalog::GetConfigFieldNames(const FString& ConfigName) const
{
	const FFlockCatalogConfig* Config = FindConfigByName(ConfigName);
	return Config ? FieldNamesOf(Config->Fields) : TArray<FString>();
}

const FFlockCatalogTemplate* UFlockContentCatalog::FindTemplateByTag(const FString& Tag) const
{
	if (Tag.IsEmpty())
	{
		return nullptr;
	}
	// Case-insensitive: a tag is author-typed on the dashboard, and "Currency" means the same thing the
	// SDK's own tag lookups treat it as.
	return PlayerTemplates.FindByPredicate([&Tag](const FFlockCatalogTemplate& Template)
	{
		return Template.Tag.Equals(Tag, ESearchCase::IgnoreCase);
	});
}

const FFlockCatalogTemplate* UFlockContentCatalog::FindTemplateByName(const FString& Name) const
{
	return PlayerTemplates.FindByPredicate([&Name](const FFlockCatalogTemplate& Template)
	{
		return Template.Name == Name;
	});
}

const FFlockCatalogConfig* UFlockContentCatalog::FindConfigByName(const FString& Name) const
{
	return GameConfigs.FindByPredicate([&Name](const FFlockCatalogConfig& Config)
	{
		return Config.Name == Name;
	});
}

FString UFlockContentCatalog::FindShopItemId(const FString& ItemName) const
{
	for (const FFlockCatalogShop& Shop : Shops)
	{
		for (const FFlockCatalogShopItem& Item : Shop.Items)
		{
			if (Item.Name == ItemName)
			{
				return Item.Id;
			}
		}
	}
	return FString();
}

bool UFlockContentCatalog::IsEmptyCatalog() const
{
	return PlayerTemplates.IsEmpty() && GameConfigs.IsEmpty() && Shops.IsEmpty();
}
