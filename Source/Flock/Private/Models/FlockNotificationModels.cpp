// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Models/FlockNotificationModels.h"

namespace
{
	/** A field that is present and not JSON null. Nullable wire members must not read back as "null". */
	bool HasValue(const TSharedRef<FJsonObject>& Object, const TCHAR* Field)
	{
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
		return Value.IsValid() && Value->Type != EJson::Null;
	}

	/** Reads a nullable string, leaving the member empty when the wire says null. */
	void ReadNullableString(const TSharedRef<FJsonObject>& Object, const TCHAR* Field, FString& Out)
	{
		if (HasValue(Object, Field))
		{
			Object->TryGetStringField(Field, Out);
		}
	}
}

bool FFlockNotification::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockNotification& OutStruct, FString& OutError)
{
	Object->TryGetStringField(TEXT("id"), OutStruct.Id);
	Object->TryGetStringField(TEXT("studio_id"), OutStruct.StudioId);
	Object->TryGetStringField(TEXT("recipient_type"), OutStruct.RecipientType);
	Object->TryGetStringField(TEXT("recipient_id"), OutStruct.RecipientId);
	Object->TryGetStringField(TEXT("type"), OutStruct.Type);
	Object->TryGetStringField(TEXT("severity"), OutStruct.Severity);
	Object->TryGetStringField(TEXT("title"), OutStruct.Title);
	Object->TryGetStringField(TEXT("body"), OutStruct.Body);
	Object->TryGetStringField(TEXT("created_at"), OutStruct.CreatedAt);
	Object->TryGetStringField(TEXT("updated_at"), OutStruct.UpdatedAt);

	// Nullable on the wire. read_at absent is the unread state and must stay empty, because IsRead() keys
	// on emptiness — parsing "null" into the string would make every unread row read as read.
	ReadNullableString(Object, TEXT("game_id"), OutStruct.GameId);
	ReadNullableString(Object, TEXT("read_at"), OutStruct.ReadAt);
	ReadNullableString(Object, TEXT("campaign_id"), OutStruct.CampaignId);

	// `data` is an open dict — kept verbatim inside the handle rather than routed through the wire transform,
	// so a sender's key spelling survives.
	OutStruct.Data = FFlockJsonData::FromJson(Object->TryGetField(TEXT("data")));
	return true;
}

bool FFlockNotificationSummary::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockNotificationSummary& OutStruct, FString& OutError)
{
	Object->TryGetNumberField(TEXT("unread_count"), OutStruct.UnreadCount);

	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (Object->TryGetArrayField(TEXT("items"), Items) && Items != nullptr)
	{
		OutStruct.Items.Reserve(Items->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Items)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Entry) || Entry == nullptr)
			{
				continue;
			}
			FFlockNotification Parsed;
			if (FFlockNotification::FromWireObject(Entry->ToSharedRef(), Parsed, OutError))
			{
				OutStruct.Items.Add(MoveTemp(Parsed));
			}
		}
	}
	return true;
}
