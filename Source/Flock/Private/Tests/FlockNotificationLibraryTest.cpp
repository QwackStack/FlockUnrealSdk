// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Blueprint/FlockNotificationAsyncActions.h"
#include "Blueprint/FlockNotificationLibrary.h"
#include "Providers/FlockNotificationProvider.h"

// The Blueprint accessors must answer exactly what the structs do. They are thin by design, and this is
// what keeps them that way: a graph and C++ reading the same row must never disagree about whether it is
// read, pending, delivered or canceled.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationLibraryParityTest, "Flock.Notification.Library.CppParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationLibraryParityTest::RunTest(const FString& Parameters)
{
	FFlockNotification Unread;
	FFlockNotification Read;
	Read.ReadAt = TEXT("2026-08-13T01:00:00Z");

	TestEqual(TEXT("unread parity"), UFlockNotificationLibrary::IsNotificationRead(Unread), Unread.IsRead());
	TestEqual(TEXT("read parity"), UFlockNotificationLibrary::IsNotificationRead(Read), Read.IsRead());
	TestFalse(TEXT("empty ReadAt is unread"), UFlockNotificationLibrary::IsNotificationRead(Unread));
	TestTrue(TEXT("stamped ReadAt is read"), UFlockNotificationLibrary::IsNotificationRead(Read));

	FFlockScheduledNotification Pending;
	FFlockScheduledNotification Delivered;
	Delivered.DeliveredAt = TEXT("2026-08-13T09:00:00Z");
	FFlockScheduledNotification Canceled;
	Canceled.CanceledAt = TEXT("2026-08-12T09:00:00Z");

	for (const FFlockScheduledNotification* Row : { &Pending, &Delivered, &Canceled })
	{
		TestEqual(TEXT("pending parity"), UFlockNotificationLibrary::IsScheduledPending(*Row), Row->IsPending());
		TestEqual(TEXT("delivered parity"), UFlockNotificationLibrary::IsScheduledDelivered(*Row), Row->IsDelivered());
		TestEqual(TEXT("canceled parity"), UFlockNotificationLibrary::IsScheduledCanceled(*Row), Row->IsCanceled());
	}

	// The three states are mutually exclusive in the way a graph will branch on them.
	TestTrue(TEXT("pending row is pending"), UFlockNotificationLibrary::IsScheduledPending(Pending));
	TestFalse(TEXT("delivered row is not pending"), UFlockNotificationLibrary::IsScheduledPending(Delivered));
	TestFalse(TEXT("canceled row is not pending"), UFlockNotificationLibrary::IsScheduledPending(Canceled));

	// Wire-spelling helpers delegate to the same functions the provider sends with.
	TestEqual(TEXT("platform string parity"), UFlockNotificationLibrary::DevicePlatformToString(EFlockDevicePlatform::IOS),
		FString(FlockDevicePlatformToWire(EFlockDevicePlatform::IOS)));
	TestEqual(TEXT("channel string parity"), UFlockNotificationLibrary::NotificationChannelToString(EFlockNotificationChannel::Push),
		FString(FlockNotificationChannelToWire(EFlockNotificationChannel::Push)));

	// And the platform query answers the same thing the provider would act on.
	EFlockDevicePlatform FromLibrary = EFlockDevicePlatform::Web;
	EFlockDevicePlatform FromProvider = EFlockDevicePlatform::Web;
	const bool bLib = UFlockNotificationLibrary::GetCurrentDevicePlatform(FromLibrary);
	const bool bProv = FFlockNotificationProvider::GetCurrentDevicePlatform(FromProvider);
	TestEqual(TEXT("platform support parity"), bLib, bProv);
	TestEqual(TEXT("platform value parity"), static_cast<uint8>(FromLibrary), static_cast<uint8>(FromProvider));

	return true;
}

// ── The schedule-status nodes answer the wire spellings, and the node default matches the constant ──
// These nodes exist so a graph never types a status literal. Nothing asserted them, which left the SDK
// shipping the same unguarded literal it tells users to avoid — including the async node's own default,
// which UHT forces to be written out longhand.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationScheduleStatusParityTest, "Flock.Notification.Library.ScheduleStatusParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationScheduleStatusParityTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("pending node matches the constant"),
		UFlockNotificationLibrary::ScheduleStatusPending(), FString(FlockScheduledNotificationStatuses::Pending));
	TestEqual(TEXT("delivered node matches the constant"),
		UFlockNotificationLibrary::ScheduleStatusDelivered(), FString(FlockScheduledNotificationStatuses::Delivered));
	TestEqual(TEXT("canceled node matches the constant"),
		UFlockNotificationLibrary::ScheduleStatusCanceled(), FString(FlockScheduledNotificationStatuses::Canceled));

	// The single-l spelling is the whole reason these are nodes; pin it against the British form.
	TestNotEqual(TEXT("the wire spelling is not the double-l form"),
		FString(FlockScheduledNotificationStatuses::Canceled), FString(TEXT("cancelled")));

	// UHT needs a literal for a BP pin default, so the constant cannot be used there. Nothing else stops
	// the two drifting — a changed constant would leave the node silently defaulting to the old value.
	if (const UFunction* Fn = UFlockGetScheduledNotificationsAction::StaticClass()
			->FindFunctionByName(TEXT("GetScheduled")))
	{
		TestEqual(TEXT("the node's Status pin defaults to the pending constant"),
			Fn->GetMetaData(TEXT("CPP_Default_Status")), FString(FlockScheduledNotificationStatuses::Pending));
	}
	else
	{
		AddError(TEXT("Flock Get Scheduled Notifications has no GetScheduled UFunction to inspect."));
	}

	return true;
}

#endif // WITH_AUTOMATION_TESTS