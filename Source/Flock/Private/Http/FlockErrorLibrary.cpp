// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Http/FlockErrorLibrary.h"

FString UFlockErrorLibrary::ToDisplayString(const FFlockError& Error)
{
	return Error.ToDisplayText();
}

bool UFlockErrorLibrary::IsAlreadyRegistered(const FFlockError& Error)
{
	return Error.IsAlreadyRegistered();
}
