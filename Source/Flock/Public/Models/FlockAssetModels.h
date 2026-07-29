// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockAssetModels.generated.h"

/**
 * An asset record from the backend (OpenAPI `AssetSchema`). A plain reflected model — both asset routes
 * are enveloped, and the snake_case wire maps via FFlockJsonUtils (extension_type -> ExtensionType,
 * s3_download_url -> S3DownloadUrl). Dates stay raw ISO-8601 strings, consistent with the game models.
 *
 * `S3DownloadUrl` is a *presigned* URL with a server-side lifetime, so a record served from the offline
 * snapshot can carry one that has already expired. FFlockAssetProvider handles that by refetching the
 * record once when a download is refused, rather than by expiring the snapshot — the metadata is still
 * good, only the signature went stale.
 *
 * The wire marks `extension_type` and `size_bytes` nullable. UE has no nullable scalar, so an unreported
 * size reads as -1 rather than 0: a genuinely empty asset is a different fact from an unknown one, and
 * the cache size check has to tell them apart.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockAsset
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Name;

	/** File extension as the server reported it ("png", "mp3"), or empty when it reported none. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString ExtensionType;

	/** Size in bytes, or -1 when the server reported none. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int64 SizeBytes = -1;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString S3DownloadUrl;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString GameId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString UpdatedAt;

	/** True once the record carries an id. An empty struct is how "no such asset" reads on a Blueprint pin. */
	bool IsValid() const { return !Id.IsEmpty(); }

	/**
	 * The half of the cache key that changes when the bytes change. Canonical uses `UpdatedAt.Ticks`;
	 * UpdatedAt is a raw ISO-8601 string here, so this is a CRC of that string — the same discriminating
	 * power without a date parse that could fail on an unexpected format, and filename-safe by construction.
	 */
	FString VersionToken() const;
};
