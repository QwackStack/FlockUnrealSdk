// Disk-backed crash recovery. Session subsystem writes active.json on start, refreshes on
// heartbeat, deletes on graceful end. On next launch any leftover active.json rotates into
// pending/<ticks>_<guid>.json and is replayed as a deferred EndSession. Off the UObject graph.

#pragma once

#include "CoreMinimal.h"

struct FFlockSessionMarker
{
	FString SessionId;
	FString PlayerId;
	FString GameVersionId;
	FDateTime StartedAt = FDateTime(0);
	FDateTime LastSeenAt = FDateTime(0);
	int32 ScreensViewed = 0;
};

class QWACK_UE_SDK_API FFlockSessionStore
{
public:
	// InRoot is typically <ProjectSaved>/Flock/sessions. Creates the root and pending/.
	explicit FFlockSessionStore(const FString& InRoot);

	// Atomic via .tmp + same-volume rename.
	bool WriteActive(const FFlockSessionMarker& Marker) const;

	bool TryReadActive(FFlockSessionMarker& OutMarker) const;

	void DeleteActive() const;

	// Returns the destination filename, or empty on failure / nothing to rotate.
	FString RotateActiveToPending() const;

	// Filenames only, lexically sorted (= chronological by ticks prefix).
	TArray<FString> ListPending() const;

	bool TryReadPending(const FString& Filename, FFlockSessionMarker& OutMarker) const;

	void DeletePending(const FString& Filename) const;

private:
	FString ActivePath() const;
	FString PendingDir() const;
	FString PendingPathFor(const FString& Filename) const;

	static bool WriteJsonAtomic(const FString& AbsPath, const FFlockSessionMarker& Marker);
	static bool ReadJsonFile(const FString& AbsPath, FFlockSessionMarker& OutMarker);
	static FString SerializeMarker(const FFlockSessionMarker& Marker);
	static bool DeserializeMarker(const FString& Json, FFlockSessionMarker& OutMarker);

	FString Root;
};
