// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Setup/FlockEnginePurchaseNodes.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "UObject/UnrealType.h"

namespace
{
	/** The script package the engine's in-app purchase nodes live in. */
	const TCHAR* const OnlineSubsystemUtilsPackage = TEXT("/Script/OnlineSubsystemUtils");
}

bool FFlockEnginePurchaseNodes::IsAnEnginePurchaseClass(const UClass* Class)
{
	for (const UClass* Each = Class; Each != nullptr; Each = Each->GetSuperClass())
	{
		if (Each->GetName().StartsWith(TEXT("InAppPurchase"), ESearchCase::CaseSensitive)
			&& Each->GetOutermost()->GetName().Equals(OnlineSubsystemUtilsPackage, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

bool FFlockEnginePurchaseNodes::UsesThem(const UBlueprint& Blueprint)
{
	TArray<UEdGraph*> Graphs;
	Blueprint.GetAllGraphs(Graphs);
	for (const UEdGraph* Graph : Graphs)
	{
		if (Graph == nullptr)
		{
			continue;
		}
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node == nullptr)
			{
				continue;
			}
			// An async node names the object that makes its call (its proxy factory): the engine's purchase nodes are such
			// nodes over its InAppPurchase proxy classes. Read by reflection, so this needs no Blueprint-graph module.
			const FObjectPropertyBase* Factory = FindFProperty<FObjectPropertyBase>(Node->GetClass(), TEXT("ProxyFactoryClass"));
			if (Factory != nullptr && IsAnEnginePurchaseClass(Cast<UClass>(Factory->GetObjectPropertyValue_InContainer(Node))))
			{
				return true;
			}
		}
	}
	return false;
}

TArray<FString> FFlockEnginePurchaseNodes::FindBlueprintsUsingThem()
{
	TArray<FString> Found;
	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	TArray<FName> Referencers;
	Registry.GetReferencers(FName(OnlineSubsystemUtilsPackage), Referencers, UE::AssetRegistry::EDependencyCategory::Package);
	for (const FName& PackageName : Referencers)
	{
		if (!PackageName.ToString().StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
		{
			continue;
		}
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(PackageName, Assets);
		for (const FAssetData& Asset : Assets)
		{
			if (const UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset()))
			{
				if (UsesThem(*Blueprint))
				{
					Found.Add(Asset.GetObjectPathString());
				}
			}
		}
	}
	return Found;
}
