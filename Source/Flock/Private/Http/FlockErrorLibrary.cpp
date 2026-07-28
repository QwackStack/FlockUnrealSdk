// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Http/FlockErrorLibrary.h"

FString UFlockErrorLibrary::ToDisplayString(const FFlockError& Error)
{
	return Error.ToString();
}

bool UFlockErrorLibrary::IsAlreadyRegistered(const FFlockError& Error)
{
	return Error.IsAlreadyRegistered();
}
