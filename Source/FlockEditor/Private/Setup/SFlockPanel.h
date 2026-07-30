// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Config/FlockConfig.h"
#include "Setup/FlockLiveSnapshot.h"
#include "Setup/FlockSetupStatus.h"
// Complete type, not a forward declaration: TStrongObjectPtr static_asserts on UObject-ness, which it
// cannot check against an incomplete type.
#include "Setup/FlockEditorLiveProbe.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

class SVerticalBox;
class SWidgetSwitcher;

/**
 * The Flock panel. Two modes on one tab:
 *
 *  - **Setup** — what is wrong with this project's setup, and the buttons that fix it. Renders whatever
 *    FFlockSetupStatus reports and decides nothing itself, so it cannot disagree with the settings
 *    banner, the PIE guard, or the packaging validator.
 *  - **Live** — during Play In Editor, the SDK's runtime state. Reads public accessors only.
 *
 * One tab rather than two because they are never both useful at once, and because a developer who
 * docked "Flock" somewhere expects it to stay there when they hit Play.
 */
class SFlockPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFlockPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SFlockPanel() override;

	/** Re-evaluates and rebuilds the findings list. Cheap — no network. */
	void Refresh();

private:
	// ── Setup mode ──

	TSharedRef<SWidget> BuildSetupView();
	TSharedRef<SWidget> BuildFindingRow(const FFlockSetupFinding& Finding);
	TSharedRef<SWidget> BuildCredentialForm();
	TSharedRef<SWidget> BuildActionBar();

	/** One labelled, editable settings field bound straight to the config CDO member. */
	TSharedRef<SWidget> BuildField(const FText& Label, FString UFlockConfig::* Member, bool bPassword);

	/** Runs the fix a finding names. Only TestConnection touches the network. */
	void RunFix(EFlockSetupFix Fix);
	void RunTestConnection();

	/** Commits one credential field and persists it to DefaultGame.ini, then refreshes. */
	void CommitField(const FText& NewValue, ETextCommit::Type CommitType, FString UFlockConfig::* Field);
	FText GetField(FString UFlockConfig::* Field) const;

	// ── Live mode ──

	TSharedRef<SWidget> BuildLiveView();
	void RefreshLive();
	void EnterLiveMode();
	void ExitLiveMode();
	void HandleLiveEvent(const FString& Line);
	EActiveTimerReturnType PollLive(double InCurrentTime, float InDeltaTime);

	static TSharedRef<SWidget> LiveRow(const FText& Label, const FText& Value);

	TSharedPtr<SWidgetSwitcher> ModeSwitcher;
	TSharedPtr<SVerticalBox> FindingsBox;
	TSharedPtr<SVerticalBox> LiveBox;

	/**
	 * Strong, not weak: the hub holds only a dynamic-delegate reference to this object, which does not
	 * keep it alive. Without this it would be collected mid-session.
	 */
	TStrongObjectPtr<UFlockEditorLiveProbe> LiveProbe;

	TSharedPtr<IFlockLiveSnapshotSource> LiveSource;

	/** Newest last. Bounded, because a long play session would otherwise grow this without limit. */
	TArray<FString> RecentEvents;

	FDelegateHandle PieStartHandle;
	FDelegateHandle PieEndHandle;
	TWeakPtr<FActiveTimerHandle> PollTimer;

	/** True while a Test Connection is in flight, so the button cannot be double-fired. */
	bool bProbeInFlight = false;
};
