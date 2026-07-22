// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Analytics/FlockEventCache.h"

#if WITH_AUTOMATION_TESTS

/**
 * In-memory IFlockEventCache for tests — same semantics as the file cache (oldest-first ordering,
 * cap eviction, empty handle when it cannot store) without touching disk.
 *
 * Provider tests use this so a failed flush leaves no debris in the project's Saved dir; the file
 * cache's own tests exercise the real thing against a temp directory.
 */
class FFlockMemoryEventCache : public IFlockEventCache
{
public:
	explicit FFlockMemoryEventCache(int32 InMaxEntries = 1000)
		: MaxEntries(InMaxEntries)
	{
	}

	/** Set to make every Enqueue fail, standing in for a full or unwritable disk. */
	bool bFailWrites = false;

	virtual int32 PendingCount() const override { return Handles.Num(); }

	virtual FString Enqueue(const FString& Payload) override
	{
		if (bFailWrites || MaxEntries <= 0)
		{
			return FString();
		}
		const FString Handle = FString::Printf(TEXT("%08x"), ++Sequence);
		Handles.Add(Handle);
		Payloads.Add(Handle, Payload);
		EvictToCap();
		return Handles.Contains(Handle) ? Handle : FString();
	}

	virtual bool Read(const FString& Handle, FString& OutPayload) const override
	{
		const FString* Found = Payloads.Find(Handle);
		if (Found == nullptr)
		{
			return false;
		}
		OutPayload = *Found;
		return true;
	}

	virtual void Replace(const FString& Handle, const FString& Payload) override
	{
		if (Payloads.Contains(Handle))
		{
			Payloads[Handle] = Payload;
		}
	}

	virtual void Remove(const FString& Handle) override
	{
		const int32 Index = Handles.IndexOfByKey(Handle);
		if (Index == INDEX_NONE)
		{
			return;
		}
		Handles.RemoveAt(Index);
		Payloads.Remove(Handle);
	}

	virtual void PeekBatch(int32 MaxCount, TArray<FString>& OutHandles, TArray<FString>& OutPayloads) const override
	{
		OutHandles.Reset();
		OutPayloads.Reset();
		const int32 Count = FMath::Min(FMath::Max(MaxCount, 0), Handles.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			OutHandles.Add(Handles[Index]);
			OutPayloads.Add(Payloads[Handles[Index]]);
		}
	}

	virtual TArray<FString> AllHandles() const override { return Handles; }

	virtual void Clear() override
	{
		Handles.Reset();
		Payloads.Reset();
	}

private:
	void EvictToCap()
	{
		while (Handles.Num() > FMath::Max(MaxEntries, 0))
		{
			Payloads.Remove(Handles[0]);
			Handles.RemoveAt(0);
		}
	}

	int32 MaxEntries = 0;
	TArray<FString> Handles;
	TMap<FString, FString> Payloads;
	uint32 Sequence = 0;
};

#endif // WITH_AUTOMATION_TESTS
