// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Setup/SFlockPanel.h"
#include "Setup/FlockSetupContext.h"
#include "Setup/FlockConnectionProbe.h"
#include "Setup/FlockEditorUserSettings.h"
#include "Setup/FlockEditorLiveProbe.h"
#include "FlockSubsystem.h"
#include "FlockEvents.h"
#include "Editor.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Codegen/FlockCodegenRunner.h"
#include "Version/FlockVersionResolver.h"
#include "Config/FlockConfig.h"
#include "FlockEditor.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"
#include "ISettingsModule.h"
#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "FlockEditor"

namespace
{
	const FSlateBrush* SeverityIcon(EFlockSetupSeverity Severity)
	{
		switch (Severity)
		{
		case EFlockSetupSeverity::Error:   return FAppStyle::GetBrush("Icons.ErrorWithColor");
		case EFlockSetupSeverity::Warning: return FAppStyle::GetBrush("Icons.WarningWithColor");
		default:                           return FAppStyle::GetBrush("Icons.InfoWithColor");
		}
	}

	FText FixLabel(EFlockSetupFix Fix)
	{
		switch (Fix)
		{
		case EFlockSetupFix::OpenSettings:   return LOCTEXT("FixOpenSettings", "Open Settings");
		case EFlockSetupFix::Resolve:        return LOCTEXT("FixResolve", "Resolve");
		case EFlockSetupFix::TestConnection: return LOCTEXT("FixTest", "Test Connection");
		case EFlockSetupFix::SyncSchemas:    return LOCTEXT("FixSync", "Sync Schemas");
		default:                             return FText::GetEmpty();
		}
	}

	void Toast(const FText& Message, bool bSuccess)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = bSuccess ? 5.f : 8.f;
		if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
			Item->ExpireAndFadeout();
		}
	}
}

void SFlockPanel::Construct(const FArguments& InArgs)
{
	LiveSource = MakeShared<FFlockPieSnapshotSource>();

	ChildSlot
	[
		SAssignNew(ModeSwitcher, SWidgetSwitcher)

		+ SWidgetSwitcher::Slot()
		[
			BuildSetupView()
		]

		+ SWidgetSwitcher::Slot()
		[
			BuildLiveView()
		]
	];

	PieStartHandle = FEditorDelegates::PostPIEStarted.AddLambda([this](bool) { EnterLiveMode(); });
	PieEndHandle = FEditorDelegates::EndPIE.AddLambda([this](bool) { ExitLiveMode(); });

	// A tab opened during an already-running session should show live state, not stale setup.
	if (GEditor && GEditor->PlayWorld)
	{
		EnterLiveMode();
	}
	else
	{
		Refresh();
	}
}

SFlockPanel::~SFlockPanel()
{
	// Mandatory, not hygiene: this widget outlives the PIE session that raised the events, and the
	// editor outlives the widget.
	FEditorDelegates::PostPIEStarted.Remove(PieStartHandle);
	FEditorDelegates::EndPIE.Remove(PieEndHandle);

	if (LiveProbe.IsValid())
	{
		LiveProbe->OnLiveEvent.RemoveAll(this);
		LiveProbe->Unbind();
		LiveProbe.Reset();
	}
}

TSharedRef<SWidget> SFlockPanel::BuildSetupView()
{
	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		.Padding(12.f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
			[
				SAssignNew(FindingsBox, SVerticalBox)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
			[
				BuildCredentialForm()
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				BuildActionBar()
			]
		];
}

void SFlockPanel::Refresh()
{
	if (!FindingsBox.IsValid())
	{
		return;
	}

	FindingsBox->ClearChildren();

	const TArray<FFlockSetupFinding> Findings = FFlockSetupContext::Evaluate();

	if (Findings.Num() == 0)
	{
		// A healthy project says so plainly and offers nothing to click. Silence is the point.
		FindingsBox->AddSlot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SImage).Image(FAppStyle::GetBrush("Icons.SuccessWithColor"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("AllGood", "Flock is set up. Nothing needs attention."))
			]
		];
		return;
	}

	for (const FFlockSetupFinding& Finding : Findings)
	{
		FindingsBox->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
		[
			BuildFindingRow(Finding)
		];
	}
}

TSharedRef<SWidget> SFlockPanel::BuildFindingRow(const FFlockSetupFinding& Finding)
{
	const EFlockSetupFix Fix = Finding.Fix;

	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 2.f, 8.f, 0.f)
		[
			SNew(SImage).Image(SeverityIcon(Finding.Severity))
		]

		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(Finding.Title)
				.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(Finding.Detail)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];

	if (Fix != EFlockSetupFix::None)
	{
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton)
			.Text(FixLabel(Fix))
			.OnClicked_Lambda([this, Fix]()
			{
				RunFix(Fix);
				return FReply::Handled();
			})
		];
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.f)
		[
			Row
		];
}

TSharedRef<SWidget> SFlockPanel::BuildField(const FText& Label, FString UFlockConfig::* Member, bool bPassword)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(0.35f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(Label)
		]
		+ SHorizontalBox::Slot().FillWidth(0.65f)
		[
			SNew(SEditableTextBox)
			.Text(this, &SFlockPanel::GetField, Member)
			.IsPassword(bPassword)
			.OnTextCommitted(this, &SFlockPanel::CommitField, Member)
		];
}

TSharedRef<SWidget> SFlockPanel::BuildCredentialForm()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("CredentialsHeading", "Credentials"))
				.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
			[
				BuildField(LOCTEXT("FieldApiUrl", "API URL"), &UFlockConfig::ApiUrl, false)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
			[
				BuildField(LOCTEXT("FieldApiKey", "API Key"), &UFlockConfig::ApiKey, true)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
			[
				BuildField(LOCTEXT("FieldGameId", "Game Name"), &UFlockConfig::GameId, false)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
			[
				BuildField(LOCTEXT("FieldGameVersion", "Game Version"), &UFlockConfig::GameVersion, false)
			]
		];
}

TSharedRef<SWidget> SFlockPanel::BuildActionBar()
{
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("TestConnection", "Test Connection"))
			.ToolTipText(LOCTEXT("TestConnectionTip",
				"Checks the API URL, key, and game version against the backend, and reports which one is wrong."))
			.IsEnabled_Lambda([this]() { return !bProbeInFlight; })
			.OnClicked_Lambda([this]() { RunTestConnection(); return FReply::Handled(); })
		]

		+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("ResolveVersion", "Resolve Version"))
			.OnClicked_Lambda([this]() { RunFix(EFlockSetupFix::Resolve); return FReply::Handled(); })
		]

		+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("SyncSchemas2", "Sync Schemas"))
			.OnClicked_Lambda([this]() { RunFix(EFlockSetupFix::SyncSchemas); return FReply::Handled(); })
		]

		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton)
			.Text(LOCTEXT("ShowAllNotices", "Show All Notices Again"))
			.ToolTipText(LOCTEXT("ShowAllNoticesTip", "Un-mutes every notice you chose to stop being interrupted by."))
			.Visibility_Lambda([]()
			{
				const UFlockEditorUserSettings* Settings = UFlockEditorUserSettings::Get();
				return (Settings && Settings->SuppressedFindingIds.Num() > 0) ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.OnClicked_Lambda([this]()
			{
				if (UFlockEditorUserSettings* Settings = UFlockEditorUserSettings::Get())
				{
					Settings->ClearSuppressions();
				}
				Refresh();
				return FReply::Handled();
			})
		];
}

FText SFlockPanel::GetField(FString UFlockConfig::* Member) const
{
	const UFlockConfig* Config = GetDefault<UFlockConfig>();
	return Config ? FText::FromString(Config->*Member) : FText::GetEmpty();
}

void SFlockPanel::CommitField(const FText& NewValue, ETextCommit::Type CommitType, FString UFlockConfig::* Member)
{
	UFlockConfig* Config = GetMutableDefault<UFlockConfig>();
	if (!Config)
	{
		return;
	}

	const FString Incoming = NewValue.ToString();
	if ((Config->*Member).Equals(Incoming, ESearchCase::CaseSensitive))
	{
		return;
	}

	Config->*Member = Incoming;
	Config->TryUpdateDefaultConfigFile();

	// A credential change invalidates whatever the last probe concluded — reporting "key accepted"
	// about a key that has since been edited would be a lie.
	FFlockSetupContext::ClearProbe();
	Refresh();
}

void SFlockPanel::RunFix(EFlockSetupFix Fix)
{
	switch (Fix)
	{
	case EFlockSetupFix::OpenSettings:
		if (ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
		{
			Settings->ShowViewer("Project", "Plugins", "Flock SDK Settings");
		}
		break;

	case EFlockSetupFix::Resolve:
		if (UFlockConfig* Config = GetMutableDefault<UFlockConfig>())
		{
			FFlockVersionResolver::ResolveAndBake(*Config);
		}
		break;

	case EFlockSetupFix::TestConnection:
		RunTestConnection();
		return; // refreshes itself when the probe lands

	case EFlockSetupFix::SyncSchemas:
	{
		TWeakPtr<SFlockPanel> WeakSelf = StaticCastSharedRef<SFlockPanel>(AsShared());
		FFlockCodegenRunner::Sync(FFlockCodegenRunner::FOnSyncComplete::CreateLambda(
			[WeakSelf](const FFlockCodegenRunner::FRunResult& Result)
			{
				Toast(FText::FromString(Result.Describe()), Result.bSucceeded);
				if (const TSharedPtr<SFlockPanel> Self = WeakSelf.Pin())
				{
					Self->Refresh();
				}
			}));
		return;
	}

	default:
		break;
	}

	Refresh();
}

void SFlockPanel::RunTestConnection()
{
	if (bProbeInFlight)
	{
		return;
	}

	const UFlockConfig* Config = GetDefault<UFlockConfig>();
	if (!Config)
	{
		return;
	}

	bProbeInFlight = true;

	// Weak self: the tab can be closed while the request is in flight.
	TWeakPtr<SFlockPanel> WeakSelf = StaticCastSharedRef<SFlockPanel>(AsShared());

	FFlockConnectionProbe::Run(Config->ApiUrl, Config->ApiKey, Config->GameVersion,
		FFlockProbeComplete::CreateLambda([WeakSelf](const FFlockProbeResult& Result)
		{
			FFlockSetupContext::SetLastProbe(Result);

			const bool bOk = Result.State == EFlockProbeState::Ok;
			Toast(bOk
				? LOCTEXT("ProbeOk", "Flock: connection OK.")
				: LOCTEXT("ProbeBad", "Flock: connection test failed — see the Flock panel."), bOk);

			if (const TSharedPtr<SFlockPanel> Self = WeakSelf.Pin())
			{
				Self->bProbeInFlight = false;
				Self->Refresh();
			}
		}));
}

// ─────────────────────────────────── Live mode ────────────────────────────────────

TSharedRef<SWidget> SFlockPanel::BuildLiveView()
{
	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		.Padding(12.f)
		[
			SAssignNew(LiveBox, SVerticalBox)
		];
}

TSharedRef<SWidget> SFlockPanel::LiveRow(const FText& Label, const FText& Value)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(0.4f)
		[
			SNew(STextBlock).Text(Label).ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
		+ SHorizontalBox::Slot().FillWidth(0.6f)
		[
			SNew(STextBlock).Text(Value).AutoWrapText(true)
		];
}

void SFlockPanel::EnterLiveMode()
{
	if (ModeSwitcher.IsValid())
	{
		ModeSwitcher->SetActiveWidgetIndex(1);
	}

	RecentEvents.Reset();

	if (!LiveProbe.IsValid())
	{
		LiveProbe.Reset(NewObject<UFlockEditorLiveProbe>());
		LiveProbe->OnLiveEvent.AddSP(this, &SFlockPanel::HandleLiveEvent);
	}

	// The subsystem is per-GameInstance, so the hub is a different object each play session — rebind
	// every time rather than assuming the previous binding still points anywhere.
	if (GEditor)
	{
		for (const FWorldContext& Context : GEditor->GetWorldContexts())
		{
			if (Context.WorldType != EWorldType::PIE || !Context.World())
			{
				continue;
			}
			if (UGameInstance* GameInstance = Context.World()->GetGameInstance())
			{
				if (UFlockSubsystem* Sdk = GameInstance->GetSubsystem<UFlockSubsystem>())
				{
					LiveProbe->Bind(Sdk->GetEvents());
					break;
				}
			}
		}
	}

	// Events cover state changes; the poll covers the two counters nothing broadcasts.
	if (!PollTimer.IsValid())
	{
		PollTimer = RegisterActiveTimer(1.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SFlockPanel::PollLive));
	}

	RefreshLive();
}

void SFlockPanel::ExitLiveMode()
{
	if (LiveProbe.IsValid())
	{
		LiveProbe->Unbind();
	}

	if (const TSharedPtr<FActiveTimerHandle> Timer = PollTimer.Pin())
	{
		UnRegisterActiveTimer(Timer.ToSharedRef());
	}
	PollTimer.Reset();

	if (ModeSwitcher.IsValid())
	{
		ModeSwitcher->SetActiveWidgetIndex(0);
	}

	Refresh();
}

EActiveTimerReturnType SFlockPanel::PollLive(double InCurrentTime, float InDeltaTime)
{
	RefreshLive();
	return EActiveTimerReturnType::Continue;
}

void SFlockPanel::HandleLiveEvent(const FString& Line)
{
	RecentEvents.Add(Line);

	// Bounded: a long play session would otherwise grow this until the panel is unusable.
	constexpr int32 MaxEvents = 40;
	if (RecentEvents.Num() > MaxEvents)
	{
		RecentEvents.RemoveAt(0, RecentEvents.Num() - MaxEvents, EAllowShrinking::No);
	}

	RefreshLive();
}

void SFlockPanel::RefreshLive()
{
	if (!LiveBox.IsValid() || !LiveSource.IsValid())
	{
		return;
	}

	const FFlockLiveSnapshot Snapshot = LiveSource->Capture();

	LiveBox->ClearChildren();

	if (!Snapshot.bSdkPresent)
	{
		LiveBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("NoSdkInPie", "No Flock subsystem in this play session."))
			.AutoWrapText(true)
		];
		return;
	}

	auto AddRow = [this](const FText& Label, const FText& Value)
	{
		LiveBox->AddSlot().AutoHeight().Padding(0.f, 2.f)
		[
			LiveRow(Label, Value)
		];
	};

	AddRow(LOCTEXT("LiveInit", "Initialized"),
		Snapshot.bInitialized
			? LOCTEXT("Yes", "Yes")
			: (Snapshot.InitializationError.IsEmpty()
				? LOCTEXT("No", "No")
				: FText::FromString(FString::Printf(TEXT("No — %s"), *Snapshot.InitializationError))));

	AddRow(LOCTEXT("LiveAuth", "Signed in"),
		Snapshot.bAuthenticated
			? FText::FromString(Snapshot.PlayerId)
			: (Snapshot.bRestoringSession ? LOCTEXT("Restoring", "Restoring…") : LOCTEXT("No", "No")));

	AddRow(LOCTEXT("LiveSession", "Analytics session"),
		Snapshot.bHasAnalyticsSession ? FText::FromString(Snapshot.AnalyticsSessionId) : LOCTEXT("None", "None"));

	AddRow(LOCTEXT("LiveConsent", "Analytics consent"),
		Snapshot.bAnalyticsConsent ? LOCTEXT("Granted", "Granted") : LOCTEXT("NotGranted", "Not granted"));

	AddRow(LOCTEXT("LiveQueue", "Queued commands"),
		FText::AsNumber(Snapshot.PendingCommandWrites));

	AddRow(LOCTEXT("LiveOffline", "Connectivity"),
		Snapshot.bLikelyOffline ? LOCTEXT("Offline", "Offline (last request never reached the server)")
		                        : LOCTEXT("Online", "Online"));

	LiveBox->AddSlot().AutoHeight().Padding(0.f, 12.f, 0.f, 4.f)
	[
		SNew(STextBlock)
		.Text(LOCTEXT("LiveActivity", "Activity"))
		.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
	];

	if (RecentEvents.Num() == 0)
	{
		LiveBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("NoActivity", "Nothing yet."))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
		return;
	}

	// Newest first reads better in a live list — the interesting line is always at the top.
	for (int32 Index = RecentEvents.Num() - 1; Index >= 0; --Index)
	{
		LiveBox->AddSlot().AutoHeight().Padding(0.f, 1.f)
		[
			SNew(STextBlock).Text(FText::FromString(RecentEvents[Index])).AutoWrapText(true)
		];
	}
}

#undef LOCTEXT_NAMESPACE
