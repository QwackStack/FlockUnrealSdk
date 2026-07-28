// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockStructLibrary.h"

#include "Codegen/FlockStructBinder.h"
#include "UObject/UnrealType.h"

// The non-thunk bodies are never entered: a CustomThunk function is dispatched through its exec* below.
// They exist so the declarations link.

bool UFlockStructLibrary::DataToStruct(const FFlockStructuredData& Data, int32& OutStruct)
{
	checkNoEntry();
	return false;
}

FFlockCommandData UFlockStructLibrary::StructToCommandData(const int32& Struct)
{
	checkNoEntry();
	return FFlockCommandData();
}

DEFINE_FUNCTION(UFlockStructLibrary::execDataToStruct)
{
	P_GET_STRUCT_REF(FFlockStructuredData, Data);

	// Step the wildcard pin manually to recover both the connected struct's type and the address of the
	// instance behind it; a declared parameter type could only ever be the placeholder int32.
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	void* StructMemory = Stack.MostRecentPropertyAddress;
	const FStructProperty* StructProperty = CastField<FStructProperty>(Stack.MostRecentProperty);

	P_FINISH;

	bool bBound = false;
	P_NATIVE_BEGIN;
	if (StructProperty != nullptr && StructProperty->Struct != nullptr && StructMemory != nullptr)
	{
		bBound = FFlockStructBinder::FillStruct(StructProperty->Struct, StructMemory, Data) > 0;
	}
	P_NATIVE_END;

	*static_cast<bool*>(RESULT_PARAM) = bBound;
}

DEFINE_FUNCTION(UFlockStructLibrary::execStructToCommandData)
{
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	const void* StructMemory = Stack.MostRecentPropertyAddress;
	const FStructProperty* StructProperty = CastField<FStructProperty>(Stack.MostRecentProperty);

	P_FINISH;

	FFlockCommandData Result;
	P_NATIVE_BEGIN;
	if (StructProperty != nullptr && StructProperty->Struct != nullptr && StructMemory != nullptr)
	{
		Result = FFlockStructBinder::ToCommandData(StructProperty->Struct, StructMemory);
	}
	P_NATIVE_END;

	*static_cast<FFlockCommandData*>(RESULT_PARAM) = Result;
}
