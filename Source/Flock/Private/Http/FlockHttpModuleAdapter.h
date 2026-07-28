// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockHttpAdapter.h"

/**
 * Default transport: the engine HTTP module (FHttpModule). One adapter covers every platform — UE's HTTP
 * module already abstracts the platform backend, so a single adapter serves every platform.
 * Kept in Private and reached only through FFlockHttpClient::CreateDefault, so the HTTP module never
 * leaks into a public header.
 */
class FFlockHttpModuleAdapter : public IFlockHttpAdapter
{
public:
	explicit FFlockHttpModuleAdapter(float InDefaultTimeoutSeconds);

	virtual FFlockRequestHandle SendAsync(const FFlockHttpRequest& Request, TFunction<void(FFlockHttpResponse)> OnComplete) override;

private:
	float DefaultTimeoutSeconds;
};
