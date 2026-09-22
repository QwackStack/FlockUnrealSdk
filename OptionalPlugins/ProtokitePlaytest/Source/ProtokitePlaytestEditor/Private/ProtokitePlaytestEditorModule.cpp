// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Config/FlockConfig.h"
#include "Editor.h"
#include "ProtokitePlaytestSettings.h"
#include "ProtokitePlaytestSetupStatus.h"
#include "ISettingsModule.h"
#include "Logging/MessageLog.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogProtokitePlaytestEditor, Log, All);

#define LOCTEXT_NAMESPACE "ProtokitePlaytestEditor"

namespace
{
	/** Opens the settings page a finding is fixed on, found by the section each page registers under. */
	void OpenSettingsPageFor(EProtokitePlaytestSetupFix Fix)
	{
		ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
		if (Settings == nullptr)
		{
			return;
		}
		const UDeveloperSettings* Page = Fix == EProtokitePlaytestSetupFix::OpenFlockSettings
			? static_cast<const UDeveloperSettings*>(GetDefault<UFlockConfig>())
			: static_cast<const UDeveloperSettings*>(GetDefault<UProtokitePlaytestSettings>());
		Settings->ShowViewer(Page->GetContainerName(), Page->GetCategoryName(), Page->GetSectionName());
	}

	/**
	 * Says what stands in a playtest's way as Play starts, in the Play message log, each finding with a link to the page
	 * that fixes it. Nothing is said while Enable Playtesting is off, and nothing interrupts Play: a playtest problem
	 * never stops the game from running.
	 *
	 * Called once Play has started, not before: the engine opens this session's page of the Play message log in between,
	 * so anything written earlier lands on the previous session's page, and the log the engine opens when Play ends,
	 * because it holds warnings, never shows it.
	 */
	void SayWhatStandsInThePlaytestsWay(const bool /*bIsSimulating*/)
	{
		if (FApp::IsUnattended() || IsRunningCommandlet())
		{
			return;
		}

		const TArray<FProtokitePlaytestSetupFinding> Findings = FProtokitePlaytestSetupStatus::Evaluate(FProtokitePlaytestSetupInput::FromProjectSettings());
		if (Findings.Num() == 0)
		{
			return;
		}

		FMessageLog PlayLog(TEXT("PIE"));
		for (const FProtokitePlaytestSetupFinding& Finding : Findings)
		{
			const EMessageSeverity::Type Severity = Finding.Severity == EProtokitePlaytestSetupSeverity::Info
				? EMessageSeverity::Info
				: EMessageSeverity::Warning;
			const FText Line = FText::Format(LOCTEXT("FindingLine", "Protokite Playtest: {0}. {1}"), Finding.Title, Finding.Detail);
			const EProtokitePlaytestSetupFix Fix = Finding.Fix;
			PlayLog.Message(Severity, Line)->AddToken(FActionToken::Create(
				Fix == EProtokitePlaytestSetupFix::OpenFlockSettings
					? LOCTEXT("OpenFlockSettings", "Open Flock SDK Settings")
					: LOCTEXT("OpenPlaytestSettings", "Open Protokite Playtest Settings"),
				LOCTEXT("OpenSettingsTooltip", "Opens the settings page this is fixed on."),
				FOnActionTokenExecuted::CreateLambda([Fix]() { OpenSettingsPageFor(Fix); })));

			if (Finding.Severity == EProtokitePlaytestSetupSeverity::Info)
			{
				UE_LOG(LogProtokitePlaytestEditor, Log, TEXT("%s. %s"), *Finding.Title.ToString(), *Finding.Detail.ToString());
			}
			else
			{
				UE_LOG(LogProtokitePlaytestEditor, Warning, TEXT("%s. %s"), *Finding.Title.ToString(), *Finding.Detail.ToString());
			}
		}
	}
}

/** The playtest plugin's editor half: it says what in the project's settings stands in a playtest's way. */
class FProtokitePlaytestEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		PlayStartedHandle = FEditorDelegates::PostPIEStarted.AddStatic(&SayWhatStandsInThePlaytestsWay);
	}

	virtual void ShutdownModule() override
	{
		FEditorDelegates::PostPIEStarted.Remove(PlayStartedHandle);
		PlayStartedHandle.Reset();
	}

private:
	FDelegateHandle PlayStartedHandle;
};

IMPLEMENT_MODULE(FProtokitePlaytestEditorModule, ProtokitePlaytestEditor)

#undef LOCTEXT_NAMESPACE
