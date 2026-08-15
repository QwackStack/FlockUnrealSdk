// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

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

#endif // WITH_AUTOMATION_TESTS
