// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Codegen/FlockCodegenCommandlet.h"

#include "Codegen/FlockCodegenManifest.h"
#include "Codegen/FlockCodegenPaths.h"
#include "Codegen/FlockCodegenRunner.h"
#include "Codegen/FlockSchemaFetcher.h"
#include "Containers/Ticker.h"
#include "FlockEditor.h"
#include "HttpModule.h"
#include "HttpManager.h"

namespace
{
	/** The canonical SDK's exit codes, kept identical so one CI script can drive either. */
	constexpr int32 ExitOk = 0;
	constexpr int32 ExitCouldNotRun = 1;
	constexpr int32 ExitDrift = 2;

	/** Generous: a cold backend plus a paged shop backfill is still well inside this. */
	constexpr double TimeoutSeconds = 120.0;
	constexpr float TickSeconds = 1.0f / 60.0f;

	/**
	 * Drives the engine's asynchronous plumbing until the work reports done.
	 *
	 * A commandlet has no engine loop, so nothing ticks unless it is ticked here — an HTTP request issued
	 * inside one simply never completes otherwise. Both pumps are needed: the HTTP manager retires
	 * requests, and the core ticker is what the SDK's own continuations run on.
	 */
	bool PumpUntil(const TFunction<bool()>& IsDone)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (!IsDone())
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				return false;
			}
			FHttpModule::Get().GetHttpManager().Tick(TickSeconds);
			FTSTicker::GetCoreTicker().Tick(TickSeconds);
			FPlatformProcess::Sleep(TickSeconds);
		}
		return true;
	}

	int32 RunSync()
	{
		bool bDone = false;
		FFlockCodegenRunner::FRunResult Result;
		FFlockCodegenRunner::Sync(FFlockCodegenRunner::FOnSyncComplete::CreateLambda(
			[&bDone, &Result](const FFlockCodegenRunner::FRunResult& Completed)
			{
				Result = Completed;
				bDone = true;
			}));

		if (!PumpUntil([&bDone] { return bDone; }))
		{
			UE_LOG(LogFlockEditor, Error, TEXT("Flock codegen: timed out after %.0f seconds."), TimeoutSeconds);
			return ExitCouldNotRun;
		}

		for (const FString& Warning : Result.Warnings)
		{
			UE_LOG(LogFlockEditor, Warning, TEXT("Flock codegen: %s"), *Warning);
		}
		UE_LOG(LogFlockEditor, Display, TEXT("%s"), *Result.Describe());
		// A sync that ran is a success even against a stale bake: it generated for what the backend
		// actually serves, and the bake is a separate setting with its own warning.
		return Result.bSucceeded ? ExitOk : ExitCouldNotRun;
	}

	int32 RunVerify()
	{
		FString GeneratedRoot;
		FString PathError;
		if (!FFlockCodegenPaths::TryResolveGeneratedRootFromSettings(GeneratedRoot, PathError))
		{
			UE_LOG(LogFlockEditor, Error, TEXT("Flock codegen: %s"), *PathError);
			return ExitCouldNotRun;
		}

		FFlockCodegenManifest Stored;
		const bool bHasManifest = FFlockCodegenManifest::TryRead(GeneratedRoot, Stored);

		bool bDone = false;
		TFlockResult<FFlockSchemaSnapshot> Fetched;
		FFlockSchemaFetcher::FetchFromSettings(FFlockSchemaFetcher::FOnSchemaFetched::CreateLambda(
			[&bDone, &Fetched](TFlockResult<FFlockSchemaSnapshot> Result)
			{
				Fetched = MoveTemp(Result);
				bDone = true;
			}));

		if (!PumpUntil([&bDone] { return bDone; }))
		{
			UE_LOG(LogFlockEditor, Error, TEXT("Flock codegen: timed out after %.0f seconds."), TimeoutSeconds);
			return ExitCouldNotRun;
		}
		// An unreachable backend is infrastructure, not drift — reporting it as drift would send someone to
		// regenerate output that is perfectly current.
		if (!Fetched.bSuccess)
		{
			UE_LOG(LogFlockEditor, Error, TEXT("Flock codegen: could not fetch schemas — %s"),
				*Fetched.Error.Message);
			return ExitCouldNotRun;
		}

		const EFlockCodegenDrift Drift = bHasManifest
			? FFlockCodegenManifest::Compare(Stored, Fetched.Value)
			: EFlockCodegenDrift::NeverGenerated;

		const FString Summary = FFlockCodegenManifest::Describe(Drift, Stored, Fetched.Value);
		if (Drift == EFlockCodegenDrift::Current)
		{
			UE_LOG(LogFlockEditor, Display, TEXT("%s"), *Summary);
			return ExitOk;
		}
		UE_LOG(LogFlockEditor, Error, TEXT("%s"), *Summary);
		return ExitDrift;
	}
}

UFlockCodegenCommandlet::UFlockCodegenCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UFlockCodegenCommandlet::Main(const FString& Params)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> Arguments;
	ParseCommandLine(*Params, Tokens, Switches, Arguments);

	const FString Mode = Arguments.FindRef(TEXT("mode")).ToLower();
	if (Mode.IsEmpty() || Mode == TEXT("sync"))
	{
		return RunSync();
	}
	if (Mode == TEXT("verify"))
	{
		return RunVerify();
	}

	UE_LOG(LogFlockEditor, Error,
		TEXT("Flock codegen: unknown mode '%s'. Use -mode=sync (the default) or -mode=verify."), *Mode);
	return ExitCouldNotRun;
}
