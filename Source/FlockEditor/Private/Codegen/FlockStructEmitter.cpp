// Copyright 2022, Qwacks. All Rights Reserved.

#include "Codegen/FlockStructEmitter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraphSchema_K2.h"
#include "FileHelpers.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Models/FlockJsonData.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

const TCHAR* const FFlockStructEmitter::TemplateSuffix = TEXT("Template");
const TCHAR* const FFlockStructEmitter::ConfigSuffix = TEXT("Config");

namespace
{
	/** Guards against a schema that nests into itself; real ones are shallow. */
	constexpr int32 MaxNestingDepth = 8;

	FEdGraphPinType SimplePin(const FName& Category, const FName& SubCategory = NAME_None)
	{
		FEdGraphPinType Pin;
		Pin.PinCategory = Category;
		Pin.PinSubCategory = SubCategory;
		return Pin;
	}

	/** The opaque handle any shape we cannot express falls back to, so a field is never silently dropped. */
	FEdGraphPinType JsonHandlePin()
	{
		FEdGraphPinType Pin;
		Pin.PinCategory = UEdGraphSchema_K2::PC_Struct;
		Pin.PinSubCategoryObject = FFlockJsonData::StaticStruct();
		return Pin;
	}

	/**
	 * Scalar wire type → pin type. Datetimes stay strings: the whole SDK carries ISO-8601 timestamps as
	 * FString (CreatedAt/UpdatedAt on every model), and diverging here would make generated models
	 * inconsistent with hand-written ones.
	 */
	bool TryMapScalar(const FString& WireType, FEdGraphPinType& OutPin)
	{
		FString Type = WireType.TrimStartAndEnd().ToLower();
		// A trailing '?' marks the field as nullable on the dashboard ("datetime?"), which says nothing
		// about its type. Blueprint has no nullable scalar anyway — an unset field simply stays at its
		// default — so the marker is dropped rather than making the type unrecognizable. Found by syncing
		// a real backend: without this, every optional field degraded to an opaque JSON handle.
		Type.RemoveFromEnd(TEXT("?"));
		if (Type == TEXT("string") || Type == TEXT("str") || Type == TEXT("text")
			|| Type == TEXT("datetime") || Type == TEXT("date") || Type == TEXT("timestamp"))
		{
			OutPin = SimplePin(UEdGraphSchema_K2::PC_String);
			return true;
		}
		if (Type == TEXT("int") || Type == TEXT("integer"))
		{
			OutPin = SimplePin(UEdGraphSchema_K2::PC_Int);
			return true;
		}
		if (Type == TEXT("long") || Type == TEXT("int64"))
		{
			OutPin = SimplePin(UEdGraphSchema_K2::PC_Int64);
			return true;
		}
		if (Type == TEXT("float"))
		{
			OutPin = SimplePin(UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Float);
			return true;
		}
		if (Type == TEXT("double") || Type == TEXT("number"))
		{
			OutPin = SimplePin(UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Double);
			return true;
		}
		if (Type == TEXT("bool") || Type == TEXT("boolean"))
		{
			OutPin = SimplePin(UEdGraphSchema_K2::PC_Boolean);
			return true;
		}
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> ParseSchemaArray(const FString& SchemaJson)
	{
		TArray<TSharedPtr<FJsonValue>> Entries;
		if (!SchemaJson.IsEmpty())
		{
			const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(SchemaJson);
			FJsonSerializer::Deserialize(Reader, Entries);
		}
		return Entries;
	}

	/** A field's `schema` child, as a list (object body) or a single descriptor (list/dict element). */
	TArray<TSharedPtr<FJsonValue>> ChildEntries(const TSharedRef<FJsonObject>& Field)
	{
		TArray<TSharedPtr<FJsonValue>> Children;
		const TSharedPtr<FJsonValue> Child = Field->TryGetField(TEXT("schema"));
		if (!Child.IsValid())
		{
			return Children;
		}
		if (Child->Type == EJson::Array)
		{
			Children = Child->AsArray();
		}
		else if (Child->Type == EJson::Object)
		{
			Children.Add(Child);
		}
		return Children;
	}

	// Forward declaration: resolving a field's type can require building a nested struct, which resolves
	// more fields.
	FEdGraphPinType ResolveFieldPin(const TSharedRef<FJsonObject>& Field, UObject* Outer, const FString& NamePrefix,
		int32 Depth, TArray<FString>& OutWarnings);

	UUserDefinedStruct* BuildStructFromEntries(UObject* Outer, const FString& StructName,
		const TArray<TSharedPtr<FJsonValue>>& Entries, int32 Depth, TArray<FString>& OutWarnings);

	FEdGraphPinType ResolveFieldPin(const TSharedRef<FJsonObject>& Field, UObject* Outer, const FString& NamePrefix,
		int32 Depth, TArray<FString>& OutWarnings)
	{
		FString WireType;
		Field->TryGetStringField(TEXT("type"), WireType);
		const FString Type = WireType.TrimStartAndEnd().ToLower();

		FEdGraphPinType Scalar;
		if (TryMapScalar(Type, Scalar))
		{
			return Scalar;
		}

		if (Depth >= MaxNestingDepth)
		{
			OutWarnings.Add(FString::Printf(
				TEXT("'%s': nested more than %d deep; emitted as an opaque JSON handle."), *NamePrefix, MaxNestingDepth));
			return JsonHandlePin();
		}

		if (Type == TEXT("object"))
		{
			const TArray<TSharedPtr<FJsonValue>> Children = ChildEntries(Field);
			if (Children.IsEmpty())
			{
				OutWarnings.Add(FString::Printf(
					TEXT("'%s': object with no declared body; emitted as an opaque JSON handle."), *NamePrefix));
				return JsonHandlePin();
			}
			UUserDefinedStruct* Nested = BuildStructFromEntries(Outer, NamePrefix, Children, Depth + 1, OutWarnings);
			if (!Nested)
			{
				return JsonHandlePin();
			}
			FEdGraphPinType Pin;
			Pin.PinCategory = UEdGraphSchema_K2::PC_Struct;
			Pin.PinSubCategoryObject = Nested;
			return Pin;
		}

		if (Type == TEXT("list") || Type == TEXT("array") || Type == TEXT("dict"))
		{
			const TArray<TSharedPtr<FJsonValue>> Children = ChildEntries(Field);
			const TSharedPtr<FJsonObject> Element = Children.IsEmpty() ? nullptr : Children[0]->AsObject();
			if (!Element.IsValid())
			{
				OutWarnings.Add(FString::Printf(
					TEXT("'%s': container with no declared element type; emitted as an opaque JSON handle."), *NamePrefix));
				return JsonHandlePin();
			}

			FEdGraphPinType ElementPin = ResolveFieldPin(Element.ToSharedRef(), Outer,
				NamePrefix + TEXT("Item"), Depth + 1, OutWarnings);

			// Unreal cannot express a container of containers as a property, so an already-containered
			// element degrades rather than producing a struct that fails to compile.
			if (ElementPin.ContainerType != EPinContainerType::None)
			{
				OutWarnings.Add(FString::Printf(
					TEXT("'%s': nested container (a list of lists or similar) is not expressible in Blueprint; ")
					TEXT("emitted as an opaque JSON handle."), *NamePrefix));
				return JsonHandlePin();
			}

			if (Type == TEXT("dict"))
			{
				// For a map pin the category describes the KEY; the value rides in PinValueType. Dict keys
				// are author data and always arrive as strings.
				FEdGraphPinType MapPin = SimplePin(UEdGraphSchema_K2::PC_String);
				MapPin.ContainerType = EPinContainerType::Map;
				MapPin.PinValueType = FEdGraphTerminalType::FromPinType(ElementPin);
				return MapPin;
			}

			ElementPin.ContainerType = EPinContainerType::Array;
			return ElementPin;
		}

		OutWarnings.Add(FString::Printf(
			TEXT("'%s': unrecognized type '%s'; emitted as an opaque JSON handle."), *NamePrefix, *WireType));
		return JsonHandlePin();
	}

	UUserDefinedStruct* BuildStructFromEntries(UObject* Outer, const FString& StructName,
		const TArray<TSharedPtr<FJsonValue>>& Entries, int32 Depth, TArray<FString>& OutWarnings)
	{
		UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(
			Outer, FName(*StructName), RF_Public | RF_Standalone);
		if (!Struct)
		{
			OutWarnings.Add(FString::Printf(TEXT("Could not create struct '%s'."), *StructName));
			return nullptr;
		}

		// CreateUserDefinedStruct seeds one placeholder member. Its guid is captured now and it is removed
		// after the real members are added — removing it first would leave a struct with no members, which
		// the editor utilities refuse to produce.
		const TArray<FStructVariableDescription>& Seeded = FStructureEditorUtils::GetVarDesc(Struct);
		const FGuid PlaceholderGuid = Seeded.Num() > 0 ? Seeded[0].VarGuid : FGuid();

		int32 Added = 0;
		for (const TSharedPtr<FJsonValue>& Entry : Entries)
		{
			const TSharedPtr<FJsonObject> Field = Entry.IsValid() ? Entry->AsObject() : nullptr;
			if (!Field.IsValid())
			{
				continue;
			}

			FString DeclaredName;
			if (!Field->TryGetStringField(TEXT("field_name"), DeclaredName) || DeclaredName.IsEmpty())
			{
				continue;
			}
			if (!FFlockStructEmitter::IsUsableMemberName(DeclaredName))
			{
				// Skipped rather than sanitized: a renamed member would write a name the server rejects.
				OutWarnings.Add(FString::Printf(
					TEXT("'%s.%s': field name is not a legal Blueprint member name, so it was skipped. ")
					TEXT("Rename it on the dashboard to use it from Blueprint."), *StructName, *DeclaredName));
				continue;
			}

			const FEdGraphPinType Pin = ResolveFieldPin(Field.ToSharedRef(), Outer,
				StructName + DeclaredName, Depth, OutWarnings);

			if (!FStructureEditorUtils::AddVariable(Struct, Pin))
			{
				OutWarnings.Add(FString::Printf(TEXT("'%s.%s': could not add the member."), *StructName, *DeclaredName));
				continue;
			}
			const TArray<FStructVariableDescription>& Vars = FStructureEditorUtils::GetVarDesc(Struct);
			if (!FStructureEditorUtils::RenameVariable(Struct, Vars.Last().VarGuid, DeclaredName))
			{
				OutWarnings.Add(FString::Printf(
					TEXT("'%s.%s': could not name the member; it will not bind to the wire field."),
					*StructName, *DeclaredName));
				continue;
			}
			++Added;
		}

		if (Added > 0 && PlaceholderGuid.IsValid())
		{
			FStructureEditorUtils::RemoveVariable(Struct, PlaceholderGuid);
		}
		else if (Added == 0)
		{
			// A schema with nothing emittable leaves the placeholder behind rather than an invalid struct;
			// the warning is what tells the designer why the asset looks wrong.
			OutWarnings.Add(FString::Printf(
				TEXT("'%s': no usable fields; the struct is empty."), *StructName));
		}
		return Struct;
	}
}

FString FFlockStructEmitter::MakeStructName(const FString& EntityName, const FString& Suffix)
{
	FString Pascal;
	bool bUpperNext = true;
	for (const TCHAR Character : EntityName)
	{
		if (FChar::IsAlnum(Character))
		{
			Pascal.AppendChar(bUpperNext ? FChar::ToUpper(Character) : Character);
			bUpperNext = false;
		}
		else
		{
			// Any separator (space, underscore, dash) starts a new word rather than surviving into the name.
			bUpperNext = true;
		}
	}
	if (Pascal.IsEmpty() || FChar::IsDigit(Pascal[0]))
	{
		// A leading digit is not a legal identifier, and an unnamed entity still needs a stable asset name.
		Pascal = Pascal.IsEmpty() ? TEXT("Unnamed") : TEXT("_") + Pascal;
	}
	return Pascal + Suffix;
}

bool FFlockStructEmitter::IsUsableMemberName(const FString& DeclaredName)
{
	if (DeclaredName.IsEmpty() || FChar::IsDigit(DeclaredName[0]))
	{
		return false;
	}
	for (const TCHAR Character : DeclaredName)
	{
		if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
		{
			return false;
		}
	}
	return true;
}

UUserDefinedStruct* FFlockStructEmitter::BuildStruct(UObject* Outer, const FString& StructName,
	const FString& SchemaJson, TArray<FString>& OutWarnings)
{
	return BuildStructFromEntries(Outer, StructName, ParseSchemaArray(SchemaJson), /*Depth*/ 0, OutWarnings);
}

FFlockStructEmitter::FEmitResult FFlockStructEmitter::BuildAll(const FFlockSchemaSnapshot& Snapshot, UObject* Outer)
{
	FEmitResult Result;

	// Names are de-duplicated across the whole run: two templates named the same, or a template and a
	// config that collide after Pascal-casing, would otherwise overwrite one another's asset.
	TSet<FString> UsedNames;
	auto UniqueName = [&UsedNames](const FString& Base)
	{
		FString Candidate = Base;
		int32 Suffix = 2;
		while (UsedNames.Contains(Candidate))
		{
			Candidate = FString::Printf(TEXT("%s_%d"), *Base, Suffix++);
		}
		UsedNames.Add(Candidate);
		return Candidate;
	};

	for (const FFlockPlayerTemplateSchema& Template : Snapshot.PlayerTemplates)
	{
		const FString StructName = UniqueName(MakeStructName(Template.Name, TemplateSuffix));
		if (UUserDefinedStruct* Struct = BuildStruct(Outer, StructName, Template.SchemaJson, Result.Warnings))
		{
			Result.StructNameById.Add(Template.Id, StructName);
			Result.StructById.Add(Template.Id, Struct);
			++Result.StructCount;
		}
	}
	for (const FFlockGameConfigSchema& Config : Snapshot.GameConfigs)
	{
		const FString StructName = UniqueName(MakeStructName(Config.Name, ConfigSuffix));
		if (UUserDefinedStruct* Struct = BuildStruct(Outer, StructName, Config.SchemaJson, Result.Warnings))
		{
			Result.StructNameById.Add(Config.Id, StructName);
			Result.StructById.Add(Config.Id, Struct);
			++Result.StructCount;
		}
	}
	return Result;
}

FFlockStructEmitter::FEmitResult FFlockStructEmitter::Emit(const FFlockSchemaSnapshot& Snapshot,
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

	// One package per struct, named after it, so an asset can be found where its name says it is and a
	// nested struct sits beside its parent.
	TArray<UPackage*> Packages;
	auto BuildInto = [&](const FString& StructName, const FString& SchemaJson, const FString& SourceId)
	{
		const FString PackageName = FString::Printf(TEXT("%s/%s"), *Root, *StructName);
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			Result.Warnings.Add(FString::Printf(TEXT("Could not create package '%s'."), *PackageName));
			return;
		}
		Package->FullyLoad();
		if (UUserDefinedStruct* Struct = BuildStruct(Package, StructName, SchemaJson, Result.Warnings))
		{
			FAssetRegistryModule::AssetCreated(Struct);
			Struct->MarkPackageDirty();
			Packages.Add(Package);
			Result.StructNameById.Add(SourceId, StructName);
			Result.StructById.Add(SourceId, Struct);
			++Result.StructCount;
		}
	};

	TSet<FString> UsedNames;
	auto UniqueName = [&UsedNames](const FString& Base)
	{
		FString Candidate = Base;
		int32 Suffix = 2;
		while (UsedNames.Contains(Candidate))
		{
			Candidate = FString::Printf(TEXT("%s_%d"), *Base, Suffix++);
		}
		UsedNames.Add(Candidate);
		return Candidate;
	};

	for (const FFlockPlayerTemplateSchema& Template : Snapshot.PlayerTemplates)
	{
		BuildInto(UniqueName(MakeStructName(Template.Name, TemplateSuffix)), Template.SchemaJson, Template.Id);
	}
	for (const FFlockGameConfigSchema& Config : Snapshot.GameConfigs)
	{
		BuildInto(UniqueName(MakeStructName(Config.Name, ConfigSuffix)), Config.SchemaJson, Config.Id);
	}

	if (!Packages.IsEmpty() && !UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty*/ false))
	{
		OutError = TEXT("Could not save one or more generated struct assets.");
		return Result;
	}

	OutError.Reset();
	return Result;
}
