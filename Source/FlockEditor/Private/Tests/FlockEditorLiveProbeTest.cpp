// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Setup/FlockEditorLiveProbe.h"
#include "Setup/FlockLiveSnapshot.h"
#include "FlockEvents.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLiveProbeBindingTest, "Flock.Editor.Setup.LiveProbe.ForwardsAndUnbinds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLiveProbeBindingTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UFlockEvents> Events(NewObject<UFlockEvents>());
	TStrongObjectPtr<UFlockEditorLiveProbe> Probe(NewObject<UFlockEditorLiveProbe>());

	TArray<FString> Lines;
	Probe->OnLiveEvent.AddLambda([&Lines](const FString& Line) { Lines.Add(Line); });

	Probe->Bind(Events.Get());

	// The hub is DYNAMIC multicast, so this only works at all because the handlers are UFUNCTIONs on a
	// UObject. If someone converts the probe to a plain class, this fails rather than silently going
	// quiet in the panel.
	Events->OnInitialized.Broadcast();
	TestEqual(TEXT("An event reaches the panel"), Lines.Num(), 1);

	Events->OnLoggedOut.Broadcast();
	Events->OnTokenRefreshed.Broadcast();
	TestEqual(TEXT("Further events keep arriving"), Lines.Num(), 3);

	// Teardown is mandatory, not hygiene: the panel outlives the PIE session that raised these events.
	Probe->Unbind();
	Lines.Reset();

	Events->OnInitialized.Broadcast();
	Events->OnLoggedOut.Broadcast();
	TestEqual(TEXT("Nothing arrives after unbinding"), Lines.Num(), 0);

	// Rebinding is what happens on the second play session, since the subsystem — and therefore the hub —
	// is a different object each time.
	Probe->Bind(Events.Get());
	Events->OnInitialized.Broadcast();
	TestEqual(TEXT("Rebinding works"), Lines.Num(), 1);

	// Binding twice must not double-deliver.
	Probe->Bind(Events.Get());
	Lines.Reset();
	Events->OnInitialized.Broadcast();
	TestEqual(TEXT("A repeat Bind does not double-deliver"), Lines.Num(), 1);

	Probe->Unbind();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLiveSnapshotNoPieTest, "Flock.Editor.Setup.LiveProbe.QuietWithoutPie",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLiveSnapshotNoPieTest::RunTest(const FString& Parameters)
{
	// The null path is the one that crashes if the source assumes a running session. No PIE world exists
	// during an automation run, so this exercises exactly that.
	const FFlockPieSnapshotSource Source;
	const FFlockLiveSnapshot Snapshot = Source.Capture();

	TestFalse(TEXT("No PIE session means no SDK"), Snapshot.bSdkPresent);
	TestFalse(TEXT("...and nothing is claimed about it"), Snapshot.bInitialized);
	TestEqual(TEXT("...including the queue"), Snapshot.PendingCommandWrites, 0);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
