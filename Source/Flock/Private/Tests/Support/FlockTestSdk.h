// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "FlockSubsystem.h"
#include "HAL/FileManager.h"
#include "Http/FlockHttpAdapter.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

/** Answers every request as unreachable, and counts them. */
class FFlockTestUnreachableTransport : public IFlockHttpAdapter
{
public:
	int32 Requests = 0;

	virtual FFlockRequestHandle SendAsync(const FFlockHttpRequest& Request, TFunction<void(FFlockHttpResponse)> OnComplete) override
	{
		++Requests;
		if (OnComplete)
		{
			FFlockHttpResponse Response;
			Response.Result = EFlockHttpResult::ConnectionError;
			OnComplete(Response);
		}
		return FFlockRequestHandle();
	}
};

/**
 * A Flock SDK for one test, built directly rather than through a game instance's subsystems, so the test drives its
 * lifecycle itself. Every file it saves (the sign-in, the offline cache, the asset cache and the analytics files) goes in
 * a folder of its own, which is deleted when this ends. It reaches no server either: until the test hands in a transport
 * of its own, every request is answered as unreachable, whatever address the test configured.
 *
 * Without that folder a test's SDK is one more launch of the game. A real launch takes over the analytics a test left and
 * sends them: a killed run sent a fake player's session to the real server. And the test reads, sends or deletes what
 * real launches left: their queued analytics, the consent decision and the saved sign-in.
 *
 * Every Flock test that builds a Flock SDK builds it through this (pinned by Flock.Runtime.TestSdk.EveryTestBuildsTheSdkThroughIt).
 */
struct FFlockTestSdk
{
	/** UFlockSubsystem is ClassWithin=UGameInstance, so its Outer must be one; the transient package trips an ensure. */
	UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
	UFlockSubsystem* Sdk = NewObject<UFlockSubsystem>(GameInstance);
	/** Both kept from garbage collection while this lives, since it shuts the SDK down after the test's own code. */
	TStrongObjectPtr<UGameInstance> KeepGameInstance{ GameInstance };
	TStrongObjectPtr<UFlockSubsystem> KeepSdk{ Sdk };
	/** Where every file this SDK saves goes. Under Intermediate, which no launch of the game reads. */
	FString Folder = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits)));

	/** Stands in for the network until a test hands in a transport of its own. */
	TSharedRef<FFlockTestUnreachableTransport> Unreachable = MakeShared<FFlockTestUnreachableTransport>();

	FFlockTestSdk()
	{
		Sdk->SetSavedFilesFolderForTesting(Folder);
		Sdk->SetHttpAdapterForTesting(Unreachable);
	}

	~FFlockTestSdk()
	{
		// Shut down first: a running SDK holds its launch folder's lock, and Windows cannot delete an open file.
		Sdk->ShutdownSdk();
		IFileManager::Get().DeleteDirectory(*Folder, /*bRequireExists*/ false, /*bTree*/ true);
	}

	FFlockTestSdk(const FFlockTestSdk&) = delete;
	FFlockTestSdk& operator=(const FFlockTestSdk&) = delete;

	UFlockSubsystem* operator->() const { return Sdk; }
};

#endif // WITH_AUTOMATION_TESTS
