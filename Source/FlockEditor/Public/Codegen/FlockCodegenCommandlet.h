// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "FlockCodegenCommandlet.generated.h"

/**
 * Codegen for CI: `UnrealEditor-Cmd.exe <project> -run=FlockCodegen [-mode=sync|verify]`.
 *
 * A commandlet rather than an `-ExecCmds` console call, because CI needs an **exit code**, and a console
 * command has no way to produce one. The three-way split matters more than it looks:
 *
 * | Code | Meaning | What CI should do |
 * |---|---|---|
 * | 0 | up to date (verify), or generated cleanly (sync) | continue |
 * | 1 | could not run — bad settings, unreachable backend, timeout | fail the job, but as infrastructure |
 * | 2 | drift — the backend's schema no longer matches what is committed | fail the job, and re-sync |
 *
 * Collapsing 1 and 2 into "non-zero" is the failure worth avoiding: an unreachable backend would then read
 * as "your generated code is stale" and send someone to regenerate perfectly good output.
 *
 * `verify` is read-only. It fetches and compares; it never writes, so it is safe on a protected branch.
 */
UCLASS()
class FLOCKEDITOR_API UFlockCodegenCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFlockCodegenCommandlet();

	virtual int32 Main(const FString& Params) override;
};
