// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockStructuredData.h"
#include "FlockStructLibrary.generated.h"

/**
 * The wildcard bridge between a provider's flattened data and a generated Blueprint struct.
 *
 * Codegen emits a struct asset per player template and game config, so a graph can Break it into typed
 * pins instead of reading values by string path. The SDK cannot name those types at compile time, so
 * these two nodes take the struct as a wildcard: drop one on a graph, connect any struct, and its pin
 * adopts that type — the same mechanism Get Data Table Row uses.
 *
 * These are the plumbing, not the intended call site. The generated Blueprint function library wraps them
 * per entity, so a graph sees `Get Player Progress` / `Update Player Progress` rather than these. They
 * stay public because they also work by hand against a struct you authored yourself.
 *
 * Round trip: **Flock Data To Struct** → *Set members in struct* → **Flock Struct To Command Data** →
 * Flock Update Player Data. Member names go out to the server exactly as spelled, so a generated struct's
 * members carry the names its template declares.
 */
UCLASS()
class FLOCK_API UFlockStructLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Fills any struct from a row's or config's data. Members bind to fields by name, tolerating the
	 * snake_case/PascalCase split between what the dashboard declares and what a read hands back. False
	 * when nothing bound — a wrong struct type, or data that has no field the struct knows.
	 */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "Flock|Data",
		meta = (CustomStructureParam = "OutStruct", DisplayName = "Flock Data To Struct"))
	static bool DataToStruct(const FFlockStructuredData& Data, int32& OutStruct);
	DECLARE_FUNCTION(execDataToStruct);

	/**
	 * Reads any struct back into a command body for Flock Update Player Data. Each member is written under
	 * its own name, which is why a generated struct is named after the template's declared fields.
	 */
	UFUNCTION(BlueprintPure, CustomThunk, Category = "Flock|Commands",
		meta = (CustomStructureParam = "Struct", DisplayName = "Flock Struct To Command Data"))
	static FFlockCommandData StructToCommandData(const int32& Struct);
	DECLARE_FUNCTION(execStructToCommandData);
};
