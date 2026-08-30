// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

// A dev-only console command that drives the Flock SDK surface and narrates each step to the log,
// so you can watch boot/init, config/game, authentication, shop, commands, leaderboards, assets, and
// analytics behavior against your configured Flock backend.
// It initializes from Project Settings > Flock SDK (API URL, key, and the resolved Game Version),
// so it needs valid settings and a reachable backend to get past init. Run from the editor or
// in-game console: `Flock.SelfTest` (also works via -ExecCmds in a development build).
//
// This is a demonstration harness, not a unit test — it exercises a transient subsystem instance
// through the injectable IFlockLogger so the breadcrumbs are visible end to end. Guards and edge
// cases live in the automation tests; this just proves the surface is wired and nothing crashes.

#if !UE_BUILD_SHIPPING

#include "FlockSubsystem.h"
#include "Analytics/FlockLogSink.h"
#include "Analytics/FlockMetadata.h"
#include "FlockEvents.h"
#include "FlockSelfTestListener.h"
#include "FlockSelfTestNarration.h"
#include "Dom/JsonObject.h"
#include "FlockLogger.h"
#include "Http/FlockJsonUtils.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Http/FlockResult.h"
#include "Models/FlockAssetModels.h"
#include "Models/FlockAuthModels.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockConfigModels.h"
#include "Models/FlockGameModels.h"
#include "Models/FlockLeaderboardModels.h"
#include "Models/FlockPlayerModels.h"
#include "Models/FlockShopModels.h"
#include "Providers/FlockAssetProvider.h"
#include "Providers/FlockAuthProvider.h"
#include "Providers/FlockCommandProvider.h"
#include "Providers/FlockConfigProvider.h"
#include "Providers/FlockGameProvider.h"
#include "Providers/FlockLeaderboardProvider.h"
#include "Providers/FlockNotificationProvider.h"
#include "Providers/FlockPlayerProvider.h"
#include "Providers/FlockShopProvider.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	// Demo credentials for the auth sweep. Mirror the canonical SDK test values with a "UE" suffix so
	// a player registered from this SDK stays distinct from one made elsewhere. The sweep runs against
	// your configured backend: a first run registers this player and signs in; later runs report
	// "already registered" and log in.
	const TCHAR* const DemoEmail = TEXT("pUE@x.com");
	const TCHAR* const DemoPassword = TEXT("pwUE");
	const TCHAR* const DemoName = TEXT("PlayerUE");
	const TCHAR* const DemoCode = TEXT("123456UE");
	const TCHAR* const DemoNewPassword = TEXT("new-pwUE");

	// Configs to pull by name in the config sweep. These must exist on your backend for this game
	// version; the sweep narrates a clean "failed" line for any that don't, rather than aborting.
	const TCHAR* const DemoConfigNames[] = { TEXT("GameplayTest"), TEXT("TestConfig") };

	// Commands sweep. The currency name is a last-resort fallback only: the sweep reads the wallet row and
	// takes a currency the player actually holds, so it adapts to whatever the "currency" template declares
	// instead of guessing. The achievement name is expected to be absent from a real achievement template;
	// that rejection is coded, so it still proves the authenticated round trip, and it is narrated as such.
	const TCHAR* const DemoCurrencyFallback = TEXT("coins");
	const TCHAR* const DemoAchievement = TEXT("self_test_achievement_UE");

	// Shop sweep. The item NAME to buy, preferred over "whatever came first in the catalog".
	//
	// This exists because taking the first item is a silent coverage hole: a catalog whose first entry is a
	// plain item means the purchase leg never exercises the reward path at all, and the run still looks
	// green. Naming a reward-bearing item makes the interesting response the one under test. Empty, or a
	// name this backend does not stock, falls back to the first item and says so — a backend without this
	// item still gets a working sweep.
	const TCHAR* const DemoShopItemName = TEXT("RewardTest");

	// Leaderboards sweep. The board NAME as it appears on the dashboard — the SDK takes names only, and
	// resolves the id internally. Empty board name skips the whole block, so a backend without this board
	// narrates cleanly instead of failing six steps.
	const TCHAR* const DemoLeaderboardName = TEXT("HighScoreTest");
	// Empty = the board's live window. A "season:<id>" prefix routes through the Season maker so that maker
	// is exercised too; anything else is a raw period key ("2026-W31") sent verbatim.
	const TCHAR* const DemoLeaderboardWindowKey = TEXT("");
	// ISO country code for a country-scoped board; empty for a global one.
	const TCHAR* const DemoLeaderboardCountry = TEXT("");
	constexpr int32 DemoLeaderboardLimit = 10;
	constexpr int32 DemoLeaderboardNeighbours = 3;

	// Score-projection check. A board ranks a player-data field, so writing that field is how a score is
	// submitted — this is the feature's whole premise, and the before/after rank also shows whether the
	// projection is live or batched.
	//
	// The template the board ranks (its source_template).
	const TCHAR* const DemoScoreTemplate = TEXT("gameplay");
	// The TOP-LEVEL field the board ranks by. Deliberately empty, which skips the projection step:
	// HighScoreTest ranks "Level.Stage", a nested path, and the update-field command takes a declared
	// top-level field name rather than a path. There is no valid value here until a board ranks a
	// top-level field — at which point this constant is the only thing that needs to change.
	const TCHAR* const DemoScoreField = TEXT("");
	constexpr double DemoScoreValue = 1234.0;

	/** Funds granted by the commands sweep. Deliberately the smallest non-zero amount — this is real money movement on a real backend. */
	constexpr int32 DemoFundsAmount = 1;

	// Notifications sweep. The template NAME as it appears on the dashboard — the SDK schedules by name and
	// resolves the id internally. **This is the one thing you must create on the backend for this sweep to
	// cover scheduling**; an empty name skips the schedule/cancel steps and narrates why, leaving the inbox
	// and device-token steps to run as normal.
	//
	// The template should declare **no required variables**: the sweep deliberately sends none, which also
	// exercises the omit-empty-optionals path. A template with required placeholders will narrate a clean
	// rejection instead.
	const TCHAR* const DemoNotificationTemplate = TEXT("SelfTestReminder");
	/**
	 * One template variable, so the sweep covers the variables path rather than only the omit-empty one.
	 *
	 * Give the template a placeholder matching this key — a title of "Reminder for {player}" is enough. An
	 * empty key sends no variables at all, which is still a valid request.
	 */
	const TCHAR* const DemoNotificationVariableKey = TEXT("player");
	const TCHAR* const DemoNotificationVariableValue = DemoName;
	/** How far ahead the demo reminder is scheduled. It is cancelled straight away, so this only has to be in the future. */
	constexpr int32 DemoNotificationDeliverInSeconds = 3600;
	constexpr int32 DemoNotificationLimit = 10;
	/**
	 * A synthetic push token, registered under an explicit **web** platform.
	 *
	 * Not a real APNs or FCM token, and it does not need to be: this proves the register/unregister wire
	 * contract, which is the whole of what the SDK owns. Actual delivery needs a device build — see the
	 * push-testing note in CHANGELOG.md. The sweep unregisters it immediately so nothing is left live.
	 */
	const TCHAR* const DemoDeviceToken = TEXT("self-test-token-UE");

	// Device credential the account-linking leg attaches and then detaches. Distinct from any device id
	// a login leg uses, so a half-finished run can never strand the session's own credential.
	const TCHAR* const DemoLinkDeviceId = TEXT("self-test-link-device-UE");

	// Assets sweep. The asset NAME as it appears on the dashboard. There is no by-name route on the
	// backend — the SDK filters the index — which is why the sweep lists the index first: a name miss is
	// then a visible spelling difference rather than a bare failure. An empty name skips the by-name and
	// download steps and leaves the index listing to run on its own.
	const TCHAR* const DemoAssetName = TEXT("iconTest.jpg");

	/**
	 * The providers the signed-in leg of the chain hands along. Bundled because that leg is a stack of
	 * nested completions, and threading one raw pointer per feature through all of them turns every capture
	 * list into a list of everything. Raw pointers: the subsystem owning them is rooted until teardown.
	 * Any of them may be null (a feature switched off in settings) — each sweep checks its own.
	 */
	struct FSignedInSweeps
	{
		FFlockAnalyticsProvider* Analytics = nullptr;
		FFlockShopProvider* Shop = nullptr;
		FFlockCommandProvider* Commands = nullptr;
		FFlockPlayerProvider* Players = nullptr;
		FFlockLeaderboardProvider* Leaderboards = nullptr;
		FFlockNotificationProvider* Notifications = nullptr;

		/** The events hub, so the notification sweep can show the two events firing off its own reads. */
		UFlockEvents* Events = nullptr;
	};

	/**
	 * Drives the analytics session lifecycle against the configured backend, then hands off to
	 * Teardown. Runs signed in, because a session needs a player id — which is why it is chained off
	 * the auth sweep rather than run alongside it.
	 *
	 * This is the part that only a real backend can prove: session start is the one analytics route
	 * with a typed response, and session end is the only PATCH. Both pass against test fakes whatever
	 * the wire shape is.
	 */
	void RunAnalyticsSweep(FFlockAnalyticsProvider& AnalyticsRef, const FString& PlayerId,
		const TSharedRef<IFlockLogger>& Logger, TFunction<void()> Teardown)
	{
		FFlockAnalyticsProvider* Analytics = &AnalyticsRef;

		// Signing in auto-starts a session when Auto Start Session is on, so one may already be in
		// flight here — that wiring is itself worth seeing.
		Logger->LogInfo(FString::Printf(
			TEXT("Self-test: analytics signed in — HasConsent=%s SessionAutoStarted=%s ServerSessionId='%s'"),
			Analytics->HasConsent() ? TEXT("true") : TEXT("false"),
			Analytics->HasActiveSession() ? TEXT("true") : TEXT("false"),
			*Analytics->GetCurrentSessionId()));

		// Close whatever sign-in opened, so the id narrated below is unambiguously this sweep's.
		Analytics->EndSession(EFlockSessionEndReason::Manual,
			[Analytics, PlayerId, Logger, Teardown](TFlockResult<FFlockAnalyticsAck>)
			{
				Analytics->StartSession(PlayerId,
					[Analytics, Logger, Teardown](TFlockResult<FString> StartResult)
					{
						if (!StartResult.bSuccess)
						{
							Logger->LogInfo(FString::Printf(
								TEXT("Self-test: analytics start session -> failed (%s); skipping the rest of the sweep."),
								*StartResult.Error.Message));
							Teardown();
							return;
						}
						Logger->LogInfo(FString::Printf(
							TEXT("Self-test: analytics start session -> server session id %s"), *StartResult.Value));

						// Screen views only count against a live session, which is exactly what the
						// signed-out probe earlier cannot exercise.
						Analytics->RecordScreenView(TEXT("MainMenu"));
						Analytics->RecordScreenView(TEXT("Shop"));
						Analytics->LogEvent(TEXT("self-test event (signed in)"),
							FFlockMetadata().Add(TEXT("source"), TEXT("Flock.SelfTest")).Add(TEXT("sweep"), 2));

						FFlockLogDetails ErrorDetails;
						ErrorDetails.LogicalExpression = TEXT("selfTest == true");
						ErrorDetails.ErrorCode = TEXT("SELF_TEST");
						Analytics->LogError(TEXT("self-test logic error"), ErrorDetails);

						// No stack trace argument: the SDK walks one. This used to pass a hand-written
						// "at SelfTest()" placeholder, which is exactly the failure a required
						// stack-trace parameter invites.
						Analytics->LogException(TEXT("self-test exception"));

						const FFlockSessionSnapshot Snapshot = Analytics->GetCurrentSnapshot();
						Logger->LogInfo(FString::Printf(
							TEXT("Self-test: analytics snapshot — screens=%d firstSession=%s pendingSpooled=%d"),
							Snapshot.ScreensViewed,
							Snapshot.IsFirstSession ? TEXT("true") : TEXT("false"),
							Analytics->GetPendingEventCount()));

						Analytics->Flush([Analytics, Logger, Teardown](TFlockResult<FFlockAnalyticsAck> FlushResult)
						{
							Logger->LogInfo(FlushResult.bSuccess
								? FString::Printf(TEXT("Self-test: analytics flush -> spool drained (pending=%d)"),
									Analytics->GetPendingEventCount())
								: FString::Printf(TEXT("Self-test: analytics flush -> failed (%s); entries stay spooled."),
									*FlushResult.Error.Message));

							Analytics->EndSession(EFlockSessionEndReason::Manual,
								[Analytics, Logger, Teardown](TFlockResult<FFlockAnalyticsAck> EndResult)
								{
									Logger->LogInfo(EndResult.bSuccess
										? FString(TEXT("Self-test: analytics end session -> closed out on the backend."))
										: FString::Printf(TEXT("Self-test: analytics end session -> failed (%s)"),
											*EndResult.Error.Message));

									// Consent round trip: withdrawing must stop collection and drop the
									// queue; restoring must reopen the session (the opt-in path).
									Analytics->LogEvent(TEXT("self-test entry before revoke"));
									const int32 PendingBefore = Analytics->GetPendingEventCount();
									Analytics->SetConsent(false);
									Logger->LogInfo(FString::Printf(
										TEXT("Self-test: analytics consent withdrawn — pending %d -> %d, HasConsent=%s"),
										PendingBefore, Analytics->GetPendingEventCount(),
										Analytics->HasConsent() ? TEXT("true") : TEXT("false")));

									Analytics->SetConsent(true);
									Logger->LogInfo(FString::Printf(
										TEXT("Self-test: analytics consent restored — HasConsent=%s SessionReopened=%s"),
										Analytics->HasConsent() ? TEXT("true") : TEXT("false"),
										Analytics->HasActiveSession() ? TEXT("true") : TEXT("false")));

									// Leave no residue on this machine: the spool, the persisted consent
									// decision, and any crash marker all go.
									Analytics->EraseLocalData();
									Logger->LogInfo(TEXT("Self-test: analytics local data erased (spool + consent decision + crash marker)."));

									Teardown();
								});
						});
					});
			});
	}

	/**
	 * Public shop catalog reads against the configured backend, signed out — the leg only a real backend
	 * can prove, because it exercises all three shop wire shapes at once: paginated `GetAll`, the bare
	 * shop-by-id read, and the enveloped-list items-by-shop. Independent one-shots that narrate their own
	 * result; does not own teardown.
	 */
	void RunShopSweep(FFlockShopProvider* Shop, const TSharedRef<IFlockLogger>& Logger)
	{
		if (Shop == nullptr)
		{
			return;
		}

		Shop->GetAll(1, 50, [Shop, Logger](TFlockResult<FFlockShopPage> Result)
		{
			if (!Result.bSuccess)
			{
				Logger->LogInfo(FString::Printf(TEXT("Self-test: shops (paginated) -> failed (%s)"), *Result.Error.Message));
				return;
			}
			Logger->LogInfo(FString::Printf(TEXT("Self-test: shops (paginated) -> %d shop(s) (total %d)."),
				Result.Value.Items.Num(), Result.Value.Total));
			if (Result.Value.Items.Num() == 0)
			{
				return;
			}

			const FFlockShop& First = Result.Value.Items[0];
			Logger->LogInfo(FString::Printf(TEXT("Self-test: first shop -> id=%s name='%s' items=%d"),
				*First.Id, *First.Name, First.ShopItems.Num()));

			const FString ShopId = First.Id;
			// Bare shop-by-id (proves the raw verb, not the envelope).
			Shop->GetById(ShopId, [ShopId, Logger](TFlockResult<FFlockShop> ByIdResult)
			{
				Logger->LogInfo(ByIdResult.bSuccess
					? FString::Printf(TEXT("Self-test: shop by id %s -> name='%s'"), *ShopId, *ByIdResult.Value.Name)
					: FString::Printf(TEXT("Self-test: shop by id %s -> failed (%s)"), *ShopId, *ByIdResult.Error.Message));
			});
			// Enveloped-list items-by-shop (proves the GetList path).
			Shop->GetItemsByShop(ShopId, FString(), [ShopId, Logger](TFlockResult<TArray<FFlockShopItem>> ItemsResult)
			{
				Logger->LogInfo(ItemsResult.bSuccess
					? FString::Printf(TEXT("Self-test: items for shop %s -> %d item(s)"), *ShopId, ItemsResult.Value.Num())
					: FString::Printf(TEXT("Self-test: items for shop %s -> failed (%s)"), *ShopId, *ItemsResult.Error.Message));
			});
		});
	}

	/**
	 * Signed-in shop sweep against the configured backend: list the catalog, buy the first item found,
	 * then read the player's inventory — a fresh (never-cached) read that shows the new entry. A purchase
	 * failure (e.g. insufficient funds) still proves the wiring: the request reaches the backend and a
	 * coded error comes back. The analytics transactions around the purchase are fire-and-forget, so this
	 * narration never waits on them. Hands off to Next, which continues the signed-in chain.
	 */
	void RunShopSignedInSweep(FFlockShopProvider* Shop, FFlockCommandProvider* Commands,
		FFlockAnalyticsProvider* Analytics, const TSharedRef<IFlockLogger>& Logger, TFunction<void()> Next)
	{
		if (Shop == nullptr)
		{
			Next();
			return;
		}

		Shop->GetAll(1, 50, [Shop, Commands, Analytics, Logger, Next](TFlockResult<FFlockShopPage> ShopsResult)
		{
			// The purchase candidate: DemoShopItemName if the catalog stocks it, else the first item.
			// Its price and currency come along because the sweep funds the wallet before buying.
			FString ItemId;
			FString ItemName;
			FString ItemCurrency;
			int32 ItemPrice = 0;
			int32 ItemRewardCount = 0;
			bool bPreferredFound = false;
			if (ShopsResult.bSuccess)
			{
				const FString Preferred(DemoShopItemName);
				for (const FFlockShop& S : ShopsResult.Value.Items)
				{
					for (const FFlockShopItem& Item : S.ShopItems)
					{
						if (Item.Id.IsEmpty())
						{
							continue;
						}
						// Take the first item as a floor, then upgrade to the named one if it turns up.
						const bool bIsPreferred = !Preferred.IsEmpty() && Item.Name == Preferred;
						if (ItemId.IsEmpty() || bIsPreferred)
						{
							ItemId = Item.Id;
							ItemName = Item.Name;
							ItemCurrency = Item.Currency;
							ItemPrice = Item.Price;
							ItemRewardCount = Item.Rewards.Num();
							bPreferredFound = bIsPreferred;
						}
						if (bPreferredFound)
						{
							break;
						}
					}
					if (bPreferredFound)
					{
						break;
					}
				}
			}
			if (!ItemId.IsEmpty())
			{
				// Narrated because it decides which response shape the purchase leg actually exercises:
				// a reward-bearing item is the only way to see a populated `granted` on the wire.
				Logger->LogInfo(FString::Printf(
					TEXT("Self-test: purchase candidate '%s' (%s) advertises %d reward(s)%s"),
					*ItemName, *ItemId, ItemRewardCount,
					bPreferredFound ? TEXT("") : TEXT(" [fell back to the first item; DemoShopItemName not stocked]")));
			}
			Logger->LogInfo(ShopsResult.bSuccess
				? FString::Printf(TEXT("Self-test: shop catalog (signed in) -> %d shop(s); purchase candidate: %s"),
					ShopsResult.Value.Items.Num(), ItemId.IsEmpty() ? TEXT("(none found)") : *ItemName)
				: FString::Printf(TEXT("Self-test: shop catalog (signed in) -> failed (%s)"), *ShopsResult.Error.Message));

			// A fresh inventory read (never cached), reused after the purchase to show the new entry.
			auto ReadInventory = [Shop, Logger, Next]()
			{
				Shop->GetPlayerInventory(FString(), 1, 50, [Logger, Next](TFlockResult<FFlockPlayerInventoryPage> InvResult)
				{
					Logger->LogInfo(InvResult.bSuccess
						? FString::Printf(TEXT("Self-test: player inventory -> %d item(s) on this page (total %d)"),
							InvResult.Value.Items.Num(), InvResult.Value.Total)
						: FString::Printf(TEXT("Self-test: player inventory -> failed (%s)"), *InvResult.Error.Message));
					Next();
				});
			};

			if (ItemId.IsEmpty())
			{
				Logger->LogInfo(TEXT("Self-test: no shop item to purchase; reading inventory only."));
				ReadInventory();
				return;
			}

			// The purchase itself, once the wallet can afford it.
			auto DoPurchase = [Shop, Analytics, Logger, ReadInventory, ItemId, ItemName, ItemCurrency, ItemPrice]()
			{
				Logger->LogInfo(FString::Printf(
					TEXT("Self-test: purchasing shop item '%s' (%s) for the signed-in player."), *ItemName, *ItemId));
				Shop->Purchase(ItemId, FString(),
					[Analytics, Logger, ReadInventory, ItemId, ItemCurrency, ItemPrice](TFlockResult<FFlockPurchaseResult> PurchaseResult)
				{
					if (PurchaseResult.bSuccess)
					{
						// Narration lives in FlockSelfTestNarration.h so its three branches can be pinned
						// by tests — the live run cannot reach them while BE-5 blocks reward granting.
						FlockNarratePurchaseResult(PurchaseResult.Value, *Logger);
					}
					else
					{
						Logger->LogInfo(FString::Printf(
							TEXT("Self-test: purchase -> failed (%s); wiring still proven — a coded error means the request reached the backend."),
							*PurchaseResult.Error.Message));
					}

					// The shop provider already fired Started/Failed (or Purchased) transactions around the
					// call, fire-and-forget with no completion — so their outcome only ever reached the log.
					// This one is sent explicitly with a completion so the sweep can narrate it, which is
					// what turns a silent analytics failure into a visible one.
					if (Analytics == nullptr)
					{
						ReadInventory();
						return;
					}
					FFlockAnalyticsTransactionRequest Tx;
					Tx.Amount = static_cast<double>(ItemPrice);
					Tx.CurrencyCode = ItemCurrency;
					Tx.ShopItemId = ItemId;
					Tx.TransactionType = TEXT("Purchase");
					Tx.Status = PurchaseResult.bSuccess ? TEXT("Purchased") : TEXT("Failed");
					// CurrencyId deliberately left empty: there is **no `/v1` currency route**, so a client
					// cannot resolve one. `/currency` exists only outside `/v1`, on the dashboard surface.
					Analytics->RecordTransaction(Tx, [Logger, ReadInventory](TFlockResult<FFlockAnalyticsAck> Ack)
					{
						if (Ack.bSuccess)
						{
							Logger->LogInfo(TEXT("Self-test: analytics transaction -> ok"));
						}
						else
						{
							Logger->LogInfo(FString::Printf(TEXT("Self-test: analytics transaction -> failed (%s)"), *Ack.Error.Message));
							Logger->LogInfo(TEXT("Self-test:   the SDK sends currency_code from the shop item but cannot send currency_id ")
								TEXT("— no /v1 currency route exists. If this is 'currency_not_found', create a currency for the game ")
								TEXT("in the dashboard, or have the backend resolve currency_code."));
						}
						ReadInventory();
					});
				});
			};

			// Fund the wallet first, so the purchase *success* path actually runs. Without this the demo
			// player has no balance in the item's currency and every run stops at insufficient_funds —
			// which proves the wiring but never the thing a shop exists to do. Granting exactly the price
			// is net-neutral: the purchase spends what the grant added.
			if (Commands != nullptr && ItemPrice > 0 && !ItemCurrency.IsEmpty())
			{
				Logger->LogInfo(FString::Printf(
					TEXT("Self-test: funding %d '%s' so the purchase can succeed (granting exactly the price)."),
					ItemPrice, *ItemCurrency));
				Commands->AddGameFunds(ItemCurrency, ItemPrice,
					[Logger, DoPurchase, ItemCurrency](TFlockResult<FFlockPlayerData> Funded)
				{
					Logger->LogInfo(Funded.bSuccess
						? FString::Printf(TEXT("Self-test: pre-purchase grant -> ok (wallet %s)"), *Funded.Value.Data.ToJsonString())
						: FString::Printf(
							TEXT("Self-test: pre-purchase grant -> failed (%s); the purchase will likely report insufficient funds. ")
							TEXT("A grant needs a 'currency'-tagged template declaring '%s'."),
							*Funded.Error.Message, *ItemCurrency));
					DoPurchase();
				});
				return;
			}

			Logger->LogInfo(TEXT("Self-test: pre-purchase grant -> skipped (no command provider, or the item has no price/currency)."));
			DoPurchase();
		});
	}

	/**
	 * Failure narration for the command legs. A command can fail two very different ways and the terse
	 * message reads identically for both, so this prints the server's error code and its own wording: a
	 * *coded* rejection (game_command.template_validation_failed) means the body was understood and the
	 * values were refused — the wiring is proven. An uncoded 422 is FastAPI rejecting the body's shape,
	 * which is a real SDK bug and must not be read as "the backend just didn't like my value".
	 */
	FString DescribeCommandFailure(const FFlockError& Error)
	{
		if (Error.Code.IsEmpty())
		{
			return FString::Printf(
				TEXT("%s [NO ERROR CODE — if this is a 422 the request body shape was rejected, not its values] body=%s"),
				*Error.Message, *Error.Body.Left(300));
		}
		return FString::Printf(TEXT("%s [code=%s] %s"), *Error.Message, *Error.Code, *Error.ServerMessage);
	}

	/**
	 * What the commands sweep read off the player's wallet row before writing anything back. Derived rather
	 * than hardcoded, because the sweep must write values the row's own template will accept — a made-up
	 * field or currency only ever produces a coded rejection, which proves the transport and nothing else.
	 */
	struct FWalletProbe
	{
		FString RowId;

		/** A field of the row, named as the *template* declares it, with its current value kept verbatim. */
		FString FieldName;
		FFlockCommandValue FieldValue;

		/** Every declared field with its current value — writing this back is a no-op the template accepts. */
		FFlockCommandData EchoBag;

		/**
		 * A currency the wallet actually holds. `AddGameFunds` wants the currency's own name (a leaf inside
		 * the wallet's currency map), not the name of the map — passing the map yields
		 * `game_command.currency_not_found`, which is how this was found in the first place.
		 */
		FString CurrencyName;
	};

	/** Reads a structured-data handle back into a JSON object; null when it holds nothing parseable. */
	TSharedPtr<FJsonObject> ParseStructured(const FFlockStructuredData& Data)
	{
		const FString Json = Data.ToJsonString();
		if (Json.IsEmpty())
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
		return (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid()) ? Parsed : nullptr;
	}

	/**
	 * The field names a template declares, read from its verbatim `schema` — the names the backend validates
	 * a write against.
	 *
	 * This matters more than it looks. A row's flattened data exposes those same fields snake->Pascal cased
	 * ("game_currencies" reads back as "GameCurrencies") so a codegen struct can bind by reflection, and the
	 * dotted reads accept either spelling. A **write** has no such tolerance: the server matches the template
	 * exactly, so writing the name GetFieldNames() handed you earns
	 * `game_command.template_validation_failed`. The schema is where the real names live, so the sweep takes
	 * them from there rather than from the row it just read.
	 */
	TArray<FString> ReadSchemaFieldNames(const FFlockPlayerTemplateSchema& Template)
	{
		TArray<FString> Names;
		if (Template.SchemaJson.IsEmpty())
		{
			return Names;
		}
		TArray<TSharedPtr<FJsonValue>> Fields;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Template.SchemaJson);
		if (!FJsonSerializer::Deserialize(Reader, Fields))
		{
			return Names;
		}
		for (const TSharedPtr<FJsonValue>& Element : Fields)
		{
			const TSharedPtr<FJsonObject> Field = Element.IsValid() ? Element->AsObject() : nullptr;
			FString FieldName;
			if (Field.IsValid() && Field->TryGetStringField(TEXT("field_name"), FieldName) && !FieldName.IsEmpty())
			{
				Names.Add(FieldName);
			}
		}
		return Names;
	}

	/**
	 * Derives the write targets from the wallet row and the template that governs it. False when the two
	 * cannot be lined up — no declared fields, or no readable data on the row.
	 */
	bool ProbeWallet(const FFlockPlayerData& Row, const FFlockPlayerTemplateSchema& Template, FWalletProbe& OutProbe)
	{
		const TSharedPtr<FJsonObject> Object = ParseStructured(Row.Data);
		const TArray<FString> DeclaredNames = ReadSchemaFieldNames(Template);
		if (!Object.IsValid() || DeclaredNames.Num() == 0)
		{
			return false;
		}

		OutProbe.RowId = Row.Id;
		for (const FString& Declared : DeclaredNames)
		{
			// The row's key is the flattened spelling; the write's key must be the declared one. Look up
			// exact-first-then-Pascal, exactly as the dotted reads do.
			TSharedPtr<FJsonValue> Value = Object->TryGetField(Declared);
			if (!Value.IsValid())
			{
				Value = Object->TryGetField(FFlockJsonUtils::SnakeToPascal(Declared));
			}
			if (!Value.IsValid())
			{
				continue; // declared but never written by this player — nothing to echo back
			}

			// FromJsonValue keeps the value exactly as the server sent it, so the echo write is type-correct.
			OutProbe.EchoBag.Set(Declared, FFlockCommandValue::FromJsonValue(Value));

			if (OutProbe.FieldName.IsEmpty())
			{
				OutProbe.FieldName = Declared;
				OutProbe.FieldValue = FFlockCommandValue::FromJsonValue(Value);
			}
			// The first nested object is the currency map; its first key is a currency the wallet holds.
			if (OutProbe.CurrencyName.IsEmpty() && Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Inner = Value->AsObject();
				const TArray<FString> InnerKeys = FFlockJsonUtils::GetFieldNames(Inner);
				if (InnerKeys.Num() > 0)
				{
					OutProbe.CurrencyName = InnerKeys[0];
				}
			}
		}
		if (OutProbe.FieldName.IsEmpty())
		{
			return false;
		}
		if (OutProbe.CurrencyName.IsEmpty())
		{
			// A flat wallet: the declared field names are themselves the currencies.
			OutProbe.CurrencyName = OutProbe.FieldName;
		}
		return true;
	}

	/**
	 * The half of the commands surface that only shows itself with no connectivity, forced here through the
	 * provider's reachability seam rather than by unplugging anything: a data write queues and replays,
	 * while a funds grant is refused outright. That asymmetry is the whole money rule, and it is invisible
	 * in a normal online run.
	 *
	 * The queued write is the same no-op field write the online leg already made, so replaying it changes
	 * nothing on the backend — the point is the delivery, not the value.
	 */
	void RunCommandsOfflineSweep(FFlockCommandProvider* Commands, const FWalletProbe& Probe,
		const TSharedRef<IFlockLogger>& Logger, TFunction<void()> Next)
	{
		Logger->LogInfo(TEXT("Self-test: forcing the reachability probe offline to exercise the command queue."));
		// Keep the real one to put back afterwards. Clearing it to null would not restore the default — it
		// would leave the provider always-reachable for the rest of the session, silently disabling the
		// offline branch the subsystem wired up.
		TSharedRef<TFunction<bool()>> RealProbe = MakeShared<TFunction<bool()>>(Commands->GetReachabilityProbe());
		Commands->SetReachabilityProbe([]() { return false; });

		const int32 Before = Commands->GetPendingWriteCount();

		// Money first, because the interesting outcome is the refusal.
		Commands->AddGameFunds(Probe.CurrencyName, DemoFundsAmount,
			[Commands, Probe, Before, Logger, Next, RealProbe](TFlockResult<FFlockPlayerData> FundsResult)
			{
				Logger->LogInfo(FundsResult.bSuccess
					? FString(TEXT("Self-test: add game funds (offline) -> UNEXPECTEDLY succeeded; money must never be sent while unreachable."))
					: FString::Printf(TEXT("Self-test: add game funds (offline) -> refused as designed (%s: %s)"),
						FundsResult.Error.Type == EFlockErrorType::Connection ? TEXT("Connection") : TEXT("other"),
						*FundsResult.Error.Message));
				Logger->LogInfo(FString::Printf(
					TEXT("Self-test: queue after the refused grant -> %d (unchanged from %d; money is never queued)."),
					Commands->GetPendingWriteCount(), Before));

				// A data write in the same state does queue, and answers with the optimistically-updated row.
				Commands->UpdatePlayerDataField(Probe.RowId, Probe.FieldName, Probe.FieldValue,
					[Commands, Logger, Next, RealProbe](TFlockResult<FFlockPlayerData> QueuedResult)
					{
						const int32 Queued = Commands->GetPendingWriteCount();
						Logger->LogInfo(QueuedResult.bSuccess
							? FString::Printf(TEXT("Self-test: field write (offline) -> queued, returned the optimistic row %s; pending=%d"),
								*QueuedResult.Value.Id, Queued)
							: FString::Printf(TEXT("Self-test: field write (offline) -> failed (%s)"), *QueuedResult.Error.Message));

						Logger->LogInfo(TEXT("Self-test: restoring the reachability probe and flushing the queue."));
						Commands->SetReachabilityProbe(*RealProbe);

						Commands->FlushPendingWrites([Commands, Queued, Logger, Next](TFlockResult<int32> FlushResult)
						{
							if (FlushResult.bSuccess)
							{
								// Delivered and dropped both empty the queue, and only the first is a success —
								// reporting one number would read as "nothing happened" for either.
								const int32 Remaining = Commands->GetPendingWriteCount();
								const int32 Dropped = FMath::Max(0, Queued - FlushResult.Value - Remaining);
								Logger->LogInfo(FString::Printf(
									TEXT("Self-test: flush -> delivered %d, dropped %d (permanently rejected), still queued %d"),
									FlushResult.Value, Dropped, Remaining));
							}
							else
							{
								Logger->LogInfo(FString::Printf(TEXT("Self-test: flush -> failed (%s)"), *FlushResult.Error.Message));
							}
							Next();
						});
					});
			});
	}

	/**
	 * Game commands against the configured backend. Runs signed in — every command resolves a row belonging
	 * to the current player — so it is chained off the auth sweep, between the shop and analytics legs.
	 *
	 * The data writes are deliberately non-destructive: they put the wallet's own current values straight
	 * back, with the types the server sent, so what is proven is the round trip and the cache write-through
	 * rather than a change nobody asked for. The funds grant is the exception — it moves real currency, by
	 * the smallest amount that is still a grant, because the wallet resolution and the non-idempotent post
	 * are exactly what a fake cannot prove.
	 *
	 * On a failure the coded error is printed: a coded rejection means the body was understood and the values
	 * were refused (wiring proven), while an uncoded 422 means the body's *shape* was rejected — an SDK bug.
	 * See DescribeCommandFailure.
	 */
	void RunCommandsSweep(FFlockCommandProvider* Commands, FFlockPlayerProvider* Players,
		const TSharedRef<IFlockLogger>& Logger, TFunction<void()> Next)
	{
		if (Commands == nullptr || Players == nullptr)
		{
			Logger->LogInfo(TEXT("Self-test: command or player provider unavailable; skipping the commands sweep."));
			Next();
			return;
		}

		// The template first, then the player's row for it. A game would normally just call GetMyDataByTag,
		// but the template is what declares the field names a write has to use, so the sweep needs both.
		Players->GetTemplateByTag(TEXT("currency"),
			[Commands, Players, Logger, Next](TFlockResult<FFlockPlayerTemplateSchema> TemplateResult)
			{
				if (!TemplateResult.bSuccess)
				{
					Logger->LogInfo(FString::Printf(
						TEXT("Self-test: currency template (tag 'currency') -> failed (%s); skipping the commands sweep."),
						*TemplateResult.Error.Message));
					Next();
					return;
				}

				const FFlockPlayerTemplateSchema Template = TemplateResult.Value;
				Logger->LogInfo(FString::Printf(
					TEXT("Self-test: currency template -> id=%s name='%s' declares [%s]"),
					*Template.Id, *Template.Name, *FString::Join(ReadSchemaFieldNames(Template), TEXT(", "))));

				Players->GetMyDataByTemplate(Template.Id,
					[Commands, Template, Logger, Next](TFlockResult<FFlockPlayerData> WalletResult)
					{
						if (!WalletResult.bSuccess)
						{
							Logger->LogInfo(FString::Printf(
								TEXT("Self-test: wallet row (tag 'currency') -> failed (%s); skipping the commands sweep."),
								*WalletResult.Error.Message));
							Next();
							return;
						}
						if (WalletResult.Value.Id.IsEmpty())
						{
							// A success with an empty record is "this player has no row yet", not an error.
							Logger->LogInfo(TEXT("Self-test: wallet row (tag 'currency') -> none for this player; "
								"skipping the commands sweep (create a 'currency'-tagged player template to exercise it)."));
							Next();
							return;
						}

						FWalletProbe Probe;
						if (!ProbeWallet(WalletResult.Value, Template, Probe))
						{
							Logger->LogInfo(FString::Printf(
								TEXT("Self-test: wallet row %s and its template don't line up (no declared field with a value); "
									"skipping the commands sweep."),
								*WalletResult.Value.Id));
							Next();
							return;
						}

						Logger->LogInfo(FString::Printf(
							TEXT("Self-test: wallet row -> id=%s template=%s write-field='%s' currency='%s'"),
							*Probe.RowId, *WalletResult.Value.PlayerTemplateId, *Probe.FieldName, *Probe.CurrencyName));
						// The row's shape as read (flattened, PascalCase) next to the name a write must use, because
						// the difference between the two is the whole trap.
						Logger->LogInfo(FString::Printf(TEXT("Self-test: wallet data (as read) =%s"),
							*WalletResult.Value.Data.ToJsonString().Left(300)));

						// 1) The whole-bag write: every field put back exactly as it came, so the template validates
						//    it and nothing changes. The returned row is the server's, written into the player cache.
						Commands->UpdatePlayerData(Probe.RowId, Probe.EchoBag,
							[Commands, Probe, Logger, Next](TFlockResult<FFlockPlayerData> BagResult)
							{
								Logger->LogInfo(BagResult.bSuccess
									? FString::Printf(TEXT("Self-test: data write (%d field(s) echoed) -> row %s updated_at=%s"),
										Probe.EchoBag.GetFieldNames().Num(), *BagResult.Value.Id, *BagResult.Value.UpdatedAt)
									: FString::Printf(TEXT("Self-test: data write -> failed (%s)"),
										*DescribeCommandFailure(BagResult.Error)));

								// 2) The single-field route, same value, same type.
								Commands->UpdatePlayerDataField(Probe.RowId, Probe.FieldName, Probe.FieldValue,
									[Commands, Probe, Logger, Next](TFlockResult<FFlockPlayerData> FieldResult)
									{
										Logger->LogInfo(FieldResult.bSuccess
											? FString::Printf(TEXT("Self-test: field write '%s'=%s -> row %s"),
												*Probe.FieldName, *Probe.FieldValue.ToJsonString().Left(80), *FieldResult.Value.Id)
											: FString::Printf(TEXT("Self-test: field write -> failed (%s)"),
												*DescribeCommandFailure(FieldResult.Error)));

										// 3) The money path. No player-data id is passed: the wallet is resolved from
										//    the "currency"-tagged template, and the post is non-idempotent.
										Logger->LogInfo(FString::Printf(
											TEXT("Self-test: granting %d '%s' — real currency movement, non-idempotent by design."),
											DemoFundsAmount, *Probe.CurrencyName));
										Commands->AddGameFunds(Probe.CurrencyName, DemoFundsAmount,
											[Commands, Probe, Logger, Next](TFlockResult<FFlockPlayerData> FundsResult)
											{
												Logger->LogInfo(FundsResult.bSuccess
													? FString::Printf(TEXT("Self-test: add game funds -> row %s, wallet now %s"),
														*FundsResult.Value.Id, *FundsResult.Value.Data.ToJsonString().Left(200))
													: FString::Printf(TEXT("Self-test: add game funds -> failed (%s)"),
														*DescribeCommandFailure(FundsResult.Error)));

												// 4) The achievement row is resolved by tag too, so a missing template
												//    narrates cleanly instead of needing an id nobody has. The demo name
												//    is expected to be absent from a real template — that rejection is
												//    coded, so it still proves the authenticated round trip.
												Commands->UnlockAchievement(DemoAchievement,
													[Commands, Probe, Logger, Next](TFlockResult<FFlockPlayerData> UnlockResult)
													{
														if (UnlockResult.bSuccess)
														{
															Logger->LogInfo(FString::Printf(TEXT("Self-test: unlock achievement '%s' -> row %s"),
																DemoAchievement, *UnlockResult.Value.Id));
														}
														else if (UnlockResult.Error.Code == TEXT("game_command.achievement_not_found"))
														{
															// The expected outcome, and not a failure of anything the SDK does:
															// reaching this error means the "achievement"-tagged template was
															// resolved, the player's row was found, and the body was understood.
															// Only the demo name is absent — and the sweep deliberately does not
															// unlock a real one, because that cannot be undone.
															Logger->LogInfo(FString::Printf(
																TEXT("Self-test: unlock achievement '%s' -> rejected as expected (%s); ")
																TEXT("template + row resolution and the round trip are proven."),
																DemoAchievement, *UnlockResult.Error.ServerMessage));
														}
														else
														{
															Logger->LogInfo(FString::Printf(TEXT("Self-test: unlock achievement '%s' -> failed (%s)"),
																DemoAchievement, *DescribeCommandFailure(UnlockResult.Error)));
														}

														RunCommandsOfflineSweep(Commands, Probe, Logger, Next);
													});
											});
								});
						});
					});
			});
	}

	// Runs the auth surface against the configured backend. The register -> login flow is chained (login
	// fires on register success OR "already registered") and hands off to the signed-in shop sweep and
	// then the analytics sweep — which calls Teardown when it finishes; the other calls are independent
	// one-shots that narrate their own result. Every completion captures shared refs / the raw providers
	// (kept alive by the caller's AddToRoot until Teardown), never `this`, so a late arrival stays safe.
	/** Empty = the board's live window; a "season:" prefix goes through the Season maker; else verbatim. */
	FFlockLeaderboardWindow DemoWindow()
	{
		const FString Key = DemoLeaderboardWindowKey;
		if (Key.IsEmpty())
		{
			return FFlockLeaderboardWindow::Current();
		}
		if (Key.StartsWith(TEXT("season:")))
		{
			return FFlockLeaderboardWindow::Season(Key.RightChop(7));
		}
		return FFlockLeaderboardWindow::Period(Key);
	}

	/**
	 * The score-projection check, and the reason the leaderboard surface has no submit call: a board ranks
	 * a player-data field, so writing that field is how a score is submitted. Reads the rank, writes the
	 * field, re-reads, and reports whether the placement moved — which also shows whether the backend's
	 * projection is live or batched.
	 *
	 * ClearCache() before the second read is not optional: an online read always hits the server, but if
	 * the reachability probe reports offline — which a local backend with no internet does — the "after"
	 * read would answer from the snapshot and the comparison would be meaningless.
	 */
	void RunLeaderboardProjectionStep(FFlockLeaderboardProvider* Leaderboards, FFlockPlayerProvider* Players,
		FFlockCommandProvider* Commands, const TSharedRef<IFlockLogger>& Logger, TFunction<void()> Next)
	{
		const FString ScoreField = DemoScoreField;
		if (ScoreField.IsEmpty() || Players == nullptr || Commands == nullptr)
		{
			// Not a failure: HighScoreTest ranks a nested path, and the write command takes a top-level
			// declared field name. Narrated so a reader knows this leg was skipped rather than passed.
			Logger->LogInfo(TEXT("Self-test: leaderboard score projection -> skipped ")
				TEXT("(no top-level source field configured; the board ranks a nested path)"));
			Next();
			return;
		}

		Leaderboards->GetMyRank(DemoLeaderboardName, DemoWindow(), DemoLeaderboardCountry,
			[Leaderboards, Players, Commands, Logger, Next, ScoreField](TFlockResult<FFlockPlayerRank> Before)
			{
				const bool bBeforeRanked = Before.bSuccess && Before.Value.Ranked;
				const int32 BeforeRank = bBeforeRanked ? Before.Value.Rank : -1;
				const double BeforeScore = bBeforeRanked ? Before.Value.Score : 0.0;

				Players->GetMyDataByTemplate(DemoScoreTemplate,
					[Leaderboards, Commands, Logger, Next, ScoreField, BeforeRank, BeforeScore](TFlockResult<FFlockPlayerData> Row)
					{
						if (!Row.bSuccess || Row.Value.Id.IsEmpty())
						{
							Logger->LogInfo(FString::Printf(
								TEXT("Self-test: leaderboard score projection -> no player-data row for template '%s'; nothing to write."),
								DemoScoreTemplate));
							Next();
							return;
						}

						const FString RowId = Row.Value.Id;
						Commands->UpdatePlayerDataField(RowId, ScoreField, FFlockCommandValue(DemoScoreValue),
							[Leaderboards, Logger, Next, ScoreField, RowId, BeforeRank, BeforeScore](TFlockResult<FFlockPlayerData> Write)
							{
								if (!Write.bSuccess)
								{
									Logger->LogInfo(FString::Printf(
										TEXT("Self-test: leaderboard score projection -> write of '%s' failed (%s)"),
										*ScoreField, *Write.Error.Message));
									Next();
									return;
								}

								Leaderboards->ClearCache();
								Leaderboards->GetMyRank(DemoLeaderboardName, DemoWindow(), DemoLeaderboardCountry,
									[Logger, Next, ScoreField, RowId, BeforeRank, BeforeScore](TFlockResult<FFlockPlayerRank> After)
									{
										const int32 AfterRank = (After.bSuccess && After.Value.Ranked) ? After.Value.Rank : -1;
										const double AfterScore = (After.bSuccess && After.Value.Ranked) ? After.Value.Score : 0.0;
										const bool bMoved = AfterRank != BeforeRank || AfterScore != BeforeScore;
										Logger->LogInfo(FString::Printf(
											TEXT("Self-test: leaderboard score projection -> wrote %s=%.0f to player_data '%s' | ")
											TEXT("rank %d -> %d, score %.2f -> %.2f (%s)"),
											*ScoreField, DemoScoreValue, *RowId, BeforeRank, AfterRank, BeforeScore, AfterScore,
											bMoved ? TEXT("moved") : TEXT("unchanged; the projection may be batched")));
										Next();
									});
							});
					});
			});
	}

	/**
	 * Leaderboards against the configured backend, addressed by board name throughout — the id never
	 * appears as an argument. Runs signed in, because my-rank and around-me are the two bearer routes.
	 *
	 * Every step narrates and then continues the chain, so one missing board or one failed read reports
	 * itself rather than stopping the run. Hands off to Next when done.
	 */
	/**
	 * The whole notification surface against the configured backend, in one chain: template catalog ->
	 * schedule and cancel -> inbox reads -> mark read -> device token register and unregister.
	 *
	 * Every step narrates its own outcome and hands on regardless, so a backend missing the demo template
	 * reports one clean line rather than aborting the run.
	 *
	 * What this cannot prove: that a push notification actually arrives on a handset. Delivery needs a
	 * device build, a push plugin to mint the token, and APNs/FCM credentials on the backend. What it does
	 * prove is every call the SDK makes — which is the part the SDK owns.
	 */
	/** Renders a credential list the way a game would read it back. */
	FString DescribeAccounts(const TArray<FFlockPlayerLinkedAccount>& Accounts)
	{
		TArray<FString> Parts;
		for (const FFlockPlayerLinkedAccount& Account : Accounts)
		{
			Parts.Add(FString::Printf(TEXT("%s%s"), *Account.Provider,
				Account.ProviderType == EFlockCredentialProvider::Unknown ? TEXT(" (unknown to this SDK)") : TEXT("")));
		}
		return Parts.Num() > 0 ? FString::Join(Parts, TEXT(", ")) : TEXT("<none>");
	}

	/**
	 * Account linking against the signed-in email session: read the list, attach a device credential,
	 * read it back, then detach it so nothing live is left behind. Unlinking the *device* (never the
	 * email the session was established with) is what keeps this clear of cannot_unlink_last_credential.
	 */
	void RunAccountLinkingSweep(FFlockAuthProvider* Auth, const TSharedRef<IFlockLogger>& Logger,
		TFunction<void()> Next)
	{
		if (Auth == nullptr)
		{
			Logger->LogInfo(TEXT("Self-test: auth provider unavailable; skipping the account-linking sweep."));
			Next();
			return;
		}

		TFunction<void()> Detach = [Auth, Logger, Next]()
		{
			Auth->Unlink(EFlockCredentialProvider::DeviceId,
				[Logger, Next](TFlockResult<FFlockPlayerAccountsResponse> Result)
				{
					Logger->LogInfo(Result.bSuccess
						? FString::Printf(TEXT("Self-test: unlink device -> ok (now: %s)"), *DescribeAccounts(Result.Value.Accounts))
						: FString::Printf(TEXT("Self-test: unlink device -> failed (%s)"), *Result.Error.Message));
					Next();
				});
		};

		Auth->GetLinkedAccounts([Auth, Logger, Detach, Next](TFlockResult<FFlockPlayerAccountsResponse> Listed)
		{
			if (!Listed.bSuccess)
			{
				Logger->LogInfo(FString::Printf(TEXT("Self-test: list linked accounts -> failed (%s)"), *Listed.Error.Message));
				Next();
				return;
			}
			Logger->LogInfo(FString::Printf(TEXT("Self-test: list linked accounts -> %d (%s)"),
				Listed.Value.Accounts.Num(), *DescribeAccounts(Listed.Value.Accounts)));

			Auth->LinkDevice(DemoLinkDeviceId, [Auth, Logger, Detach, Next](TFlockResult<FFlockPlayerAccountsResponse> Linked)
			{
				if (!Linked.bSuccess)
				{
					// A previous run that died before its unlink leaves the device attached; that 409 is
					// a known state, not a defect, and the detach below still cleans it up.
					const bool bAlreadyLinked = Linked.Error.ErrorCode == EFlockErrorCode::PlayerAccountAlreadyLinked;
					Logger->LogInfo(FString::Printf(TEXT("Self-test: link device -> %s (%s)"),
						bAlreadyLinked ? TEXT("already linked from an earlier run") : TEXT("failed"),
						*Linked.Error.Message));
					if (bAlreadyLinked)
					{
						Detach();
						return;
					}
					Next();
					return;
				}
				Logger->LogInfo(FString::Printf(TEXT("Self-test: link device -> ok (now: %s)"),
					*DescribeAccounts(Linked.Value.Accounts)));
				Detach();
			});
		});
	}

	void RunNotificationSweep(FFlockNotificationProvider* Notifications, UFlockEvents* Events,
		const TSharedRef<IFlockLogger>& Logger, TFunction<void()> Next)
	{
		if (Notifications == nullptr)
		{
			Logger->LogInfo(TEXT("Self-test: notification provider unavailable; skipping the notifications sweep."));
			Next();
			return;
		}

		// The two notification events are raised off the reads further down this chain, never by a poller —
		// so binding here, before any of them run, is what makes them observable at all.
		//
		// Rooted for the duration: the hub holds only a weak reference (dynamic delegates do), so an
		// unrooted listener could be collected mid-sweep and the events would silently stop arriving.
		UFlockSelfTestListener* Listener = nullptr;
		if (Events != nullptr)
		{
			Listener = NewObject<UFlockSelfTestListener>();
			Listener->AddToRoot();
			Events->OnUnreadCountChanged.AddDynamic(Listener, &UFlockSelfTestListener::HandleUnreadCountChanged);
			Events->OnNotificationReceived.AddDynamic(Listener, &UFlockSelfTestListener::HandleNotificationReceived);
		}

		// Runs last, after every read that could raise: reports what the events saw, then unbinds and
		// unroots so the sweep leaves nothing behind.
		TFunction<void()> ReportEvents = [Events, Listener, Logger, Next]()
		{
			if (Listener == nullptr)
			{
				Logger->LogInfo(TEXT("Self-test: notification events -> skipped (no events hub)"));
				Next();
				return;
			}

			Logger->LogInfo(FString::Printf(
				TEXT("Self-test: OnUnreadCountChanged -> fired %d time(s), last count %d (server-reported only; the SDK never polls)"),
				Listener->UnreadCountEvents, Listener->LastUnreadCount));

			// Zero here is the *expected* steady state on a repeat run: "received" means first seen by a
			// read, and this player's inbox was already seeded by an earlier run. A non-zero count means
			// something genuinely new arrived since.
			Logger->LogInfo(FString::Printf(
				TEXT("Self-test: OnNotificationReceived -> %d newly-seen notification(s)%s"),
				Listener->ReceivedIds.Num(),
				Listener->ReceivedIds.Num() == 0
					? TEXT(" (expected once the watermark is seeded; the first ever fetch seeds silently)")
					: TEXT("")));
			for (const FString& Id : Listener->ReceivedIds)
			{
				Logger->LogInfo(FString::Printf(TEXT("Self-test:   received '%s'"), *Id));
			}

			if (Events != nullptr)
			{
				Events->OnUnreadCountChanged.RemoveDynamic(Listener, &UFlockSelfTestListener::HandleUnreadCountChanged);
				Events->OnNotificationReceived.RemoveDynamic(Listener, &UFlockSelfTestListener::HandleNotificationReceived);
			}
			Listener->RemoveFromRoot();
			Next();
		};
		Next = ReportEvents;

		// Device tokens close the chain. Registered under an EXPLICIT web platform rather than the
		// auto-detecting overload: the self-test host is a desktop editor or -game run, where auto-detect
		// correctly refuses. Web is a platform the backend accepts, so this exercises the real request,
		// response and parse on a machine that could never hold an APNs or FCM token.
		TFunction<void()> DeviceTokens = [Notifications, Logger, Next]()
		{
			EFlockDevicePlatform Detected = EFlockDevicePlatform::Android;
			const bool bPushHere = FFlockNotificationProvider::GetCurrentDevicePlatform(Detected);
			Logger->LogInfo(FString::Printf(TEXT("Self-test: push available on this platform -> %s%s"),
				bPushHere ? TEXT("yes") : TEXT("no"),
				bPushHere ? TEXT("") : TEXT(" (expected on desktop; registering explicitly as web instead)")));

			Notifications->RegisterDeviceToken(EFlockDevicePlatform::Web, DemoDeviceToken,
				[Notifications, Logger, Next](TFlockResult<FFlockDeviceToken> Registered)
				{
					if (!Registered.bSuccess)
					{
						Logger->LogInfo(FString::Printf(TEXT("Self-test: register device token -> failed (%s)"),
							*Registered.Error.Message));
						Next();
						return;
					}
					Logger->LogInfo(FString::Printf(TEXT("Self-test: register device token -> ok (id '%s', platform '%s', active %s)"),
						*Registered.Value.Id, *Registered.Value.Platform, Registered.Value.IsActive ? TEXT("true") : TEXT("false")));

					// Unregistered immediately so the self-test never leaves a live push target behind.
					Notifications->UnregisterDeviceToken(DemoDeviceToken,
						[Logger, Next](TFlockResult<FFlockUnregisterDeviceTokenResult> Off)
						{
							Logger->LogInfo(Off.bSuccess
								? FString::Printf(TEXT("Self-test: unregister device token -> ok (deactivated %s)"),
									Off.Value.Deactivated ? TEXT("true") : TEXT("false"))
								: FString::Printf(TEXT("Self-test: unregister device token -> failed (%s)"), *Off.Error.Message));
							Next();
						});
				});
		};

		// Inbox: page, count, summary, then mark one read and the rest read. Mark-read needs a real row, so
		// it takes the first one the page returned and skips cleanly on an empty inbox.
		TFunction<void()> Inbox = [Notifications, Logger, DeviceTokens]()
		{
			Notifications->GetNotifications(/*bUnreadOnly*/ false, 1, DemoNotificationLimit,
				[Notifications, Logger, DeviceTokens](TFlockResult<FFlockNotificationPage> Page)
				{
					if (!Page.bSuccess)
					{
						Logger->LogInfo(FString::Printf(TEXT("Self-test: notification inbox -> failed (%s)"), *Page.Error.Message));
						DeviceTokens();
						return;
					}
					Logger->LogInfo(FString::Printf(TEXT("Self-test: notification inbox -> ok (%d of %d total on page %d)"),
						Page.Value.Items.Num(), Page.Value.Total, Page.Value.Page));

					// The first unread row, if any — what mark-read will act on.
					FString FirstUnreadId;
					for (const FFlockNotification& Row : Page.Value.Items)
					{
						if (!Row.IsRead())
						{
							FirstUnreadId = Row.Id;
							break;
						}
					}

					Notifications->GetUnreadCount([Notifications, Logger, DeviceTokens, FirstUnreadId](TFlockResult<int32> Count)
					{
						Logger->LogInfo(Count.bSuccess
							? FString::Printf(TEXT("Self-test: unread count -> ok (%d)"), Count.Value)
							: FString::Printf(TEXT("Self-test: unread count -> failed (%s)"), *Count.Error.Message));

						Notifications->GetSummary([Notifications, Logger, DeviceTokens, FirstUnreadId](TFlockResult<FFlockNotificationSummary> Summary)
						{
							Logger->LogInfo(Summary.bSuccess
								? FString::Printf(TEXT("Self-test: notification summary -> ok (%d unread, %d previewed)"),
									Summary.Value.UnreadCount, Summary.Value.Items.Num())
								: FString::Printf(TEXT("Self-test: notification summary -> failed (%s)"), *Summary.Error.Message));

							TFunction<void()> MarkAll = [Notifications, Logger, DeviceTokens]()
							{
								Notifications->MarkAllRead([Logger, DeviceTokens](TFlockResult<FFlockMarkAllReadResult> All)
								{
									Logger->LogInfo(All.bSuccess
										? FString::Printf(TEXT("Self-test: mark all notifications read -> ok (%d updated)"), All.Value.Updated)
										: FString::Printf(TEXT("Self-test: mark all notifications read -> failed (%s)"), *All.Error.Message));
									DeviceTokens();
								});
							};

							if (FirstUnreadId.IsEmpty())
							{
								// Not a failure: an inbox with nothing unread is the normal steady state once
								// this sweep has run before.
								Logger->LogInfo(TEXT("Self-test: mark notification read -> skipped (no unread row in the first page)"));
								MarkAll();
								return;
							}
							Notifications->MarkRead(FirstUnreadId, [Logger, MarkAll, FirstUnreadId](TFlockResult<FFlockNotification> Marked)
							{
								Logger->LogInfo(Marked.bSuccess
									? FString::Printf(TEXT("Self-test: mark notification read -> ok ('%s' now read: %s)"),
										*FirstUnreadId, Marked.Value.IsRead() ? TEXT("true") : TEXT("false"))
									: FString::Printf(TEXT("Self-test: mark notification read -> failed (%s)"), *Marked.Error.Message));
								MarkAll();
							});
						});
					});
				});
		};

		// Scheduling: create one in the future, then cancel it immediately. Cancelling is the point — the
		// self-test must not leave a real reminder queued against the demo player.
		TFunction<void()> Scheduling = [Notifications, Logger, Inbox]()
		{
			const FString TemplateName = DemoNotificationTemplate;
			if (TemplateName.IsEmpty())
			{
				Logger->LogInfo(TEXT("Self-test: notification scheduling -> skipped (no DemoNotificationTemplate configured)"));
				Inbox();
				return;
			}

			const FDateTime DeliverAt = FDateTime::UtcNow() + FTimespan::FromSeconds(DemoNotificationDeliverInSeconds);

			// One variable and two explicit channels, so the sweep covers the filled-in path. An empty key
			// falls back to sending neither, which is the omit-empty-optionals path and equally valid.
			FFlockCommandData Variables;
			const FString VariableKey = DemoNotificationVariableKey;
			if (!VariableKey.IsEmpty())
			{
				Variables.Set(VariableKey, FString(DemoNotificationVariableValue));
			}
			const TArray<EFlockNotificationChannel> Channels =
				{ EFlockNotificationChannel::InApp, EFlockNotificationChannel::Push };

			// DeliverAt/Variables/Channels ride along so the cancel-all step can schedule a second reminder
			// with exactly the same shape as the first.
			Notifications->ScheduleByTemplateName(TemplateName, DeliverAt, Variables, Channels,
				[Notifications, Logger, Inbox, TemplateName, DeliverAt, Variables, Channels](TFlockResult<FFlockScheduledNotification> Scheduled)
				{
					if (!Scheduled.bSuccess)
					{
						// Most likely causes: no template by this name on the backend, or a placeholder the
						// sweep did not fill. Narrated rather than fatal so the inbox steps still run.
						Logger->LogInfo(FString::Printf(TEXT("Self-test: schedule notification '%s' -> failed (%s)"),
							*TemplateName, *Scheduled.Error.Message));
						Inbox();
						return;
					}
					Logger->LogInfo(FString::Printf(TEXT("Self-test: schedule notification '%s' -> ok (id '%s', deliver_at %s, pending %s)"),
						*TemplateName, *Scheduled.Value.Id, *Scheduled.Value.DeliverAt,
						Scheduled.Value.IsPending() ? TEXT("true") : TEXT("false")));

					// The SDK's own record of what it scheduled. There is no route to list or read a
					// schedule back, so this list is the only handle on a pending reminder — which is why
					// it is worth showing that scheduling actually populated it.
					const TArray<FFlockPendingSchedule> AfterSchedule = Notifications->GetPendingSchedules();
					Logger->LogInfo(FString::Printf(TEXT("Self-test: pending schedules after scheduling -> %d"), AfterSchedule.Num()));
					for (const FFlockPendingSchedule& Entry : AfterSchedule)
					{
						Logger->LogInfo(FString::Printf(TEXT("Self-test:   pending '%s' template '%s' (%s) deliver_at %s"),
							*Entry.Id, *Entry.TemplateName, *Entry.TemplateId, *Entry.DeliverAt));
					}

					Notifications->CancelScheduled(Scheduled.Value.Id,
						[Notifications, Logger, Inbox, TemplateName, DeliverAt, Variables, Channels](TFlockResult<FFlockScheduledNotification> Canceled)
						{
							Logger->LogInfo(Canceled.bSuccess
								? FString::Printf(TEXT("Self-test: cancel scheduled notification -> ok (canceled %s)"),
									Canceled.Value.IsCanceled() ? TEXT("true") : TEXT("false"))
								: FString::Printf(TEXT("Self-test: cancel scheduled notification -> failed (%s)"), *Canceled.Error.Message));

							Logger->LogInfo(FString::Printf(TEXT("Self-test: pending schedules after cancel -> %d (cancelling untracks it)"),
								Notifications->GetPendingSchedules().Num()));

							// A second schedule, cancelled through the batch path instead of by id. That is
							// the only way to exercise CancelAllScheduled against a real backend, and it
							// leaves nothing live behind for the same reason the first one is cancelled.
							Notifications->ScheduleByTemplateName(TemplateName, DeliverAt, Variables, Channels,
								[Notifications, Logger, Inbox](TFlockResult<FFlockScheduledNotification> Second)
								{
									if (!Second.bSuccess)
									{
										Logger->LogInfo(FString::Printf(
											TEXT("Self-test: cancel-all -> skipped (second schedule failed: %s)"), *Second.Error.Message));
										Inbox();
										return;
									}
									Logger->LogInfo(FString::Printf(
										TEXT("Self-test: scheduled a second reminder '%s' for the cancel-all step; pending now %d"),
										*Second.Value.Id, Notifications->GetPendingSchedules().Num()));

									Notifications->CancelAllScheduled([Notifications, Logger, Inbox](TFlockResult<int32> Cleared)
									{
										Logger->LogInfo(Cleared.bSuccess
											? FString::Printf(TEXT("Self-test: cancel all scheduled -> ok (%d cancelled server-side, %d still tracked)"),
												Cleared.Value, Notifications->GetPendingSchedules().Num())
											: FString::Printf(TEXT("Self-test: cancel all scheduled -> failed (%s); entries stay tracked for a retry"),
												*Cleared.Error.Message));
										Inbox();
									});
								});
						});
				});
		};

		// Templates first: the catalog is what makes scheduling addressable by name, and fetching it warms
		// the name memo the schedule step then resolves through.
		Notifications->GetTemplates([Notifications, Logger, Scheduling](TFlockResult<TArray<FFlockNotificationTemplate>> Catalog)
		{
			if (!Catalog.bSuccess)
			{
				Logger->LogInfo(FString::Printf(TEXT("Self-test: notification templates -> failed (%s)"), *Catalog.Error.Message));
				Scheduling();
				return;
			}
			FString Names;
			for (const FFlockNotificationTemplate& Template : Catalog.Value)
			{
				Names += (Names.IsEmpty() ? TEXT("") : TEXT(", ")) + Template.Name;
			}
			Logger->LogInfo(FString::Printf(TEXT("Self-test: notification templates -> ok (%d: %s)"),
				Catalog.Value.Num(), Names.IsEmpty() ? TEXT("none") : *Names));

			const FString TemplateName = DemoNotificationTemplate;
			if (TemplateName.IsEmpty())
			{
				Scheduling();
				return;
			}
			Notifications->GetTemplateByName(TemplateName,
				[Logger, Scheduling, TemplateName](TFlockResult<FFlockNotificationTemplate> ByName)
				{
					Logger->LogInfo(ByName.bSuccess
						? FString::Printf(TEXT("Self-test: notification template by name '%s' -> ok (id '%s', category '%s')"),
							*TemplateName, *ByName.Value.Id, *ByName.Value.Category)
						: FString::Printf(TEXT("Self-test: notification template by name '%s' -> failed (%s)"),
							*TemplateName, *ByName.Error.Message));
					Scheduling();
				});
		});
	}

	void RunLeaderboardSweep(FFlockLeaderboardProvider* Leaderboards, FFlockPlayerProvider* Players,
		FFlockCommandProvider* Commands, const TSharedRef<IFlockLogger>& Logger, TFunction<void()> Next)
	{
		const FString BoardName = DemoLeaderboardName;
		if (Leaderboards == nullptr || BoardName.IsEmpty())
		{
			Next();
			return;
		}

		// Step 1: the board's own config plus the two helpers that hang off it. The reads that follow address
		// the board by name directly, so each one is a single request and a wrong name is rejected by the
		// server rather than by a lookup in front of it (step 6 exercises exactly that).
		Leaderboards->GetByName(BoardName,
			[Leaderboards, Players, Commands, Logger, Next, BoardName](TFlockResult<FFlockLeaderboard> BoardResult)
			{
				if (!BoardResult.bSuccess)
				{
					Logger->LogInfo(FString::Printf(TEXT("Self-test: leaderboard '%s' -> failed (%s); skipping the rest of the sweep."),
						*BoardName, *BoardResult.Error.Message));
					Next();
					return;
				}

				const FFlockLeaderboard Board = BoardResult.Value;
				Logger->LogInfo(FString::Printf(
					TEXT("Self-test: leaderboard board by name -> id=%s name='%s' | %s/%s/%s window=%s scope=%s | ")
					TEXT("higherIsBetter=%s FormatScore(%.0f)='%s' FormatScore(unranked)='%s'"),
					*Board.Id, *Board.Name,
					*FlockLeaderboardValueTypeToWire(Board.ValueType),
					*FlockLeaderboardDirectionToWire(Board.Direction),
					*FlockLeaderboardAggregationToWire(Board.Aggregation),
					*FlockLeaderboardWindowTypeToWire(Board.WindowType),
					*FlockLeaderboardScopeToWire(Board.Scope),
					Board.IsHigherBetter() ? TEXT("true") : TEXT("false"),
					DemoScoreValue, *Board.FormatScore(DemoScoreValue), *Board.FormatScore(0.0, false)));

				// Step 2: the id is exposed for logging and deep links only — no read method takes one.
				Leaderboards->ResolveId(BoardName,
					[Leaderboards, Players, Commands, Logger, Next, BoardName, Board](TFlockResult<FString> IdResult)
					{
						Logger->LogInfo(IdResult.bSuccess
							? FString::Printf(TEXT("Self-test: leaderboard resolve id -> '%s' resolves to %s"), *BoardName, *IdResult.Value)
							: FString::Printf(TEXT("Self-test: leaderboard resolve id -> failed (%s)"), *IdResult.Error.Message));

						// Step 3: standings, with the top row formatted the way the board measures — a
						// duration board reads as a clock rather than raw seconds.
						Leaderboards->GetStandings(BoardName, DemoWindow(), DemoLeaderboardCountry, 1, DemoLeaderboardLimit,
							[Leaderboards, Players, Commands, Logger, Next, BoardName, Board](TFlockResult<FFlockStandings> Standings)
							{
								if (!Standings.bSuccess)
								{
									Logger->LogInfo(FString::Printf(TEXT("Self-test: leaderboard standings -> failed (%s)"),
										*Standings.Error.Message));
								}
								else
								{
									const FString Top = Standings.Value.Items.Num() > 0
										? FString::Printf(TEXT("#1 %s = %s"), *Standings.Value.Items[0].PlayerName,
											*Board.FormatScore(Standings.Value.Items[0].Score))
										: TEXT("no entries");
									Logger->LogInfo(FString::Printf(
										TEXT("Self-test: leaderboard standings -> window=%s total=%d returned=%d | %s"),
										*Standings.Value.Window, Standings.Value.Total, Standings.Value.Items.Num(), *Top));
								}

								// Step 4: the signed-in player's placement. Unranked is a documented result.
								Leaderboards->GetMyRank(BoardName, DemoWindow(), DemoLeaderboardCountry,
									[Leaderboards, Players, Commands, Logger, Next, BoardName, Board](TFlockResult<FFlockPlayerRank> Rank)
									{
										if (!Rank.bSuccess)
										{
											Logger->LogInfo(FString::Printf(TEXT("Self-test: leaderboard my rank -> failed (%s)"),
												*Rank.Error.Message));
										}
										else
										{
											Logger->LogInfo(Rank.Value.Ranked
												? FString::Printf(TEXT("Self-test: leaderboard my rank -> rank %d score %s (window=%s)"),
													Rank.Value.Rank, *Board.FormatScore(Rank.Value.Score), *Rank.Value.Window)
												: FString::Printf(TEXT("Self-test: leaderboard my rank -> unranked, no entry yet (window=%s)"),
													*Rank.Value.Window));
										}

										// Step 5: the "you are here" slice.
										Leaderboards->GetAroundMe(BoardName, DemoLeaderboardNeighbours, DemoWindow(), DemoLeaderboardCountry,
											[Leaderboards, Players, Commands, Logger, Next, BoardName](TFlockResult<FFlockStandings> Around)
											{
												Logger->LogInfo(Around.bSuccess
													? FString::Printf(TEXT("Self-test: leaderboard around me -> +/-%d returned=%d"),
														DemoLeaderboardNeighbours, Around.Value.Items.Num())
													: FString::Printf(TEXT("Self-test: leaderboard around me -> failed (%s)"),
														*Around.Error.Message));

												// Step 6: a name this game does not have must be rejected. Passing
												// means the call FAILED; a success here is the bug.
												const FString Bogus = BoardName + TEXT("__does_not_exist");
												Leaderboards->GetStandings(Bogus,
													[Leaderboards, Players, Commands, Logger, Next, Bogus](TFlockResult<FFlockStandings> Unknown)
													{
														Logger->LogInfo(Unknown.bSuccess
															? FString::Printf(TEXT("Self-test: leaderboard unknown name -> UNEXPECTED SUCCESS for '%s'; the unknown-name guard is not working"), *Bogus)
															: FString::Printf(TEXT("Self-test: leaderboard unknown name -> '%s' rejected as expected (%s)"),
																*Bogus, *Unknown.Error.Message));

														// Step 7: the projection check (self-gating).
														RunLeaderboardProjectionStep(Leaderboards, Players, Commands, Logger, Next);
													});
											});
									});
							});
					});
			});
	}

	void RunAuthSweep(FFlockAuthProvider& AuthRef, const FSignedInSweeps& Sweeps,
		const TSharedRef<IFlockLogger>& Logger, TFunction<void()> Teardown)
	{
		FFlockAuthProvider* Auth = &AuthRef;

		Logger->LogInfo(FString::Printf(
			TEXT("Self-test: auth sweep with demo creds email=%s name=%s against the configured backend."),
			DemoEmail, DemoName));

		// Shared narrator for the account flows that return {success}.
		auto NarrateAction = [Logger](const TCHAR* Label, TFlockResult<FFlockAuthActionResponse> Result)
		{
			Logger->LogInfo(Result.bSuccess
				? FString::Printf(TEXT("Self-test: %s -> success=%s"), Label, Result.Value.Success ? TEXT("true") : TEXT("false"))
				: FString::Printf(TEXT("Self-test: %s -> failed (%s)"), Label, *Result.Error.Message));
		};

		// Independent one-shots that work signed out. The reset/revoke/refresh calls fail fast on their
		// local guards here (nobody is signed in yet) — that is the guard behavior, not a server error.
		// The email-verification pair is bearer-only server-side, so it runs later, inside the chain.
		Auth->IsNameAvailable(DemoName, [Logger](TFlockResult<FFlockNameAvailableResponse> Result)
		{
			Logger->LogInfo(Result.bSuccess
				? FString::Printf(TEXT("Self-test: name '%s' available=%s"), DemoName, Result.Value.Available ? TEXT("true") : TEXT("false"))
				: FString::Printf(TEXT("Self-test: name availability -> failed (%s)"), *Result.Error.Message));
		});
		Auth->ForgotPassword(DemoEmail, [NarrateAction](TFlockResult<FFlockAuthActionResponse> Result)
		{
			NarrateAction(TEXT("forgot password"), Result);
		});
		Auth->ResetPassword(DemoEmail, DemoCode, DemoNewPassword, [NarrateAction](TFlockResult<FFlockAuthActionResponse> Result)
		{
			NarrateAction(TEXT("reset password"), Result);
		});
		Auth->RevokeToken([Logger](TFlockResult<FFlockTokenRevokeResponse> Result)
		{
			Logger->LogInfo(Result.bSuccess
				? FString::Printf(TEXT("Self-test: token revoke -> revoked=%s"), Result.Value.Revoked ? TEXT("true") : TEXT("false"))
				: FString::Printf(TEXT("Self-test: token revoke -> failed (%s)"), *Result.Error.Message));
		});
		Auth->RefreshToken([Logger](bool bRefreshed)
		{
			Logger->LogInfo(FString::Printf(TEXT("Self-test: token refresh -> %s"), bRefreshed ? TEXT("refreshed") : TEXT("no token to refresh")));
		});

		// Register -> login -> the bearer-only email flows -> teardown. The verification pair carries no
		// sign-in guard (the bearer just rides along when present), so it has to run here, after the
		// chain authenticates, to get a real answer instead of a server 401. Teardown runs on whichever
		// branch ends the chain, so the subsystem outlives every async round trip.
		Auth->RegisterWithEmail(DemoEmail, DemoPassword, DemoName,
			[Auth, Sweeps, Logger, Teardown, NarrateAction](TFlockResult<FFlockRegisterResult> Result)
			{
				if (!Result.bSuccess)
				{
					Logger->LogInfo(FString::Printf(TEXT("Self-test: email register -> failed (%s); skipping login."), *Result.Error.Message));
					Teardown();
					return;
				}

				Logger->LogInfo(Result.Value.bAlreadyRegistered
					? TEXT("Self-test: email register -> already registered; logging in.")
					: TEXT("Self-test: email register -> registered + signed in; logging in to confirm."));

				Auth->LoginWithEmail(DemoEmail, DemoPassword,
					[Auth, Sweeps, Logger, Teardown, NarrateAction](TFlockResult<FFlockPlayerLoginResponse> LoginResult)
					{
						if (!LoginResult.bSuccess)
						{
							Logger->LogInfo(FString::Printf(
								TEXT("Self-test: email login -> failed (%s); skipping the signed-in flows."), *LoginResult.Error.Message));
							Teardown();
							return;
						}

						Logger->LogInfo(FString::Printf(TEXT("Self-test: email login -> signed in as %s"), *LoginResult.Value.PlayerId));
						const FString PlayerId = LoginResult.Value.PlayerId;

						// Signed in from here: the bearer rides along automatically.
						Auth->SendEmailVerification(
							[Auth, Sweeps, PlayerId, Logger, Teardown, NarrateAction](TFlockResult<FFlockAuthActionResponse> SendResult)
							{
								NarrateAction(TEXT("send email verification (signed in)"), SendResult);

								// The real code arrives by email, so this placeholder is expected to be
								// rejected — a code error here still proves the authenticated round trip.
								Auth->VerifyEmail(DemoCode,
									[Auth, Sweeps, PlayerId, Logger, Teardown, NarrateAction](TFlockResult<FFlockAuthActionResponse> VerifyResult)
									{
										NarrateAction(TEXT("verify email (signed in, placeholder code)"), VerifyResult);

										// The shop purchase/inventory, the command writes, and the analytics
										// sweep all need a signed-in player, so they run here, in that order:
										// shop -> commands -> analytics, which owns teardown. Chaining rather
										// than firing them together keeps the subsystem alive across every
										// round trip, and keeps the narration readable.
										TFunction<void()> AfterNotifications = [Sweeps, PlayerId, Logger, Teardown]()
										{
											if (Sweeps.Analytics != nullptr)
											{
												RunAnalyticsSweep(*Sweeps.Analytics, PlayerId, Logger, Teardown);
												return;
											}
											Teardown();
										};
										// Notifications run after leaderboards and before analytics: every call in
										// the sweep is player-scoped, so it needs the signed-in leg, and it owns no
										// teardown of its own.
										TFunction<void()> AfterLeaderboards = [Sweeps, Logger, AfterNotifications]()
										{
											RunNotificationSweep(Sweeps.Notifications, Sweeps.Events, Logger, AfterNotifications);
										};
										// Leaderboards run after the commands sweep on purpose: the projection
										// step writes a player-data field, and reading a rank straight after a
										// write is the only way to see whether the projection is live.
										TFunction<void()> AfterCommands = [Sweeps, Logger, AfterLeaderboards]()
										{
											RunLeaderboardSweep(Sweeps.Leaderboards, Sweeps.Players, Sweeps.Commands,
												Logger, AfterLeaderboards);
										};
										TFunction<void()> AfterShop = [Sweeps, Logger, AfterCommands]()
										{
											RunCommandsSweep(Sweeps.Commands, Sweeps.Players, Logger, AfterCommands);
										};
										// The shop sweep now needs the command provider (to fund the wallet
										// before buying) and the analytics provider (to narrate the purchase
										// transaction, which the shop provider itself only fires and forgets).
										// Account linking runs first among the signed-in legs: it only needs the
										// bearer, and finishing it before the writes keeps the credential list it
										// narrates uncluttered by anything the later sweeps do.
										TFunction<void()> AfterLinking = [Sweeps, Logger, AfterShop]()
										{
											RunShopSignedInSweep(Sweeps.Shop, Sweeps.Commands, Sweeps.Analytics, Logger, AfterShop);
										};
										RunAccountLinkingSweep(Auth, Logger, AfterLinking);
									});
							});
					});
			});
	}

	/**
	 * Assets: list the index, resolve one by name, then pull its bytes. Neither asset route declares
	 * security, so this runs signed out as an independent one-shot and does not own teardown.
	 *
	 * The download is the only step a metadata read cannot stand in for. The record can be perfect while
	 * the bytes are unreachable — the presigned URL carries whatever host the backend was told to sign
	 * for, and one that only resolves inside the API's own network fails here and nowhere else.
	 */
	void RunAssetSweep(FFlockAssetProvider* Assets, const TSharedRef<IFlockLogger>& Logger)
	{
		if (Assets == nullptr)
		{
			Logger->LogInfo(TEXT("Self-test: asset provider unavailable; skipping the asset sweep."));
			return;
		}

		// The index first: GetByName filters it, so listing what this game version holds is what makes a
		// name miss below diagnosable.
		Assets->GetAll([Logger](TFlockResult<TArray<FFlockAsset>> Result)
		{
			if (!Result.bSuccess)
			{
				Logger->LogInfo(FString::Printf(TEXT("Self-test: assets -> failed (%s)"), *Result.Error.Message));
				return;
			}
			TArray<FString> Names;
			for (const FFlockAsset& Asset : Result.Value)
			{
				Names.Add(Asset.Name);
			}
			Logger->LogInfo(FString::Printf(TEXT("Self-test: assets -> %d asset(s): %s"),
				Result.Value.Num(), Names.Num() > 0 ? *FString::Join(Names, TEXT(", ")) : TEXT("<none>")));
		});

		const FString AssetName = DemoAssetName;
		if (AssetName.IsEmpty())
		{
			Logger->LogInfo(TEXT("Self-test: asset by name -> skipped (no DemoAssetName configured)"));
			return;
		}

		Assets->GetByName(AssetName, [Assets, AssetName, Logger](TFlockResult<FFlockAsset> Result)
		{
			if (!Result.bSuccess)
			{
				Logger->LogInfo(FString::Printf(TEXT("Self-test: asset by name '%s' -> failed (%s)"),
					*AssetName, *Result.Error.Message));
				return;
			}

			const FFlockAsset Asset = Result.Value;
			// SizeBytes is -1 when the record never carried one, which is not the same as an empty asset.
			const FString ReportedSize = Asset.SizeBytes < 0
				? FString(TEXT("unreported"))
				: FString::Printf(TEXT("%lld byte(s)"), Asset.SizeBytes);
			Logger->LogInfo(FString::Printf(TEXT("Self-test: asset by name '%s' -> id=%s type='%s' size=%s"),
				*AssetName, *Asset.Id, *Asset.ExtensionType, *ReportedSize));

			Assets->DownloadBytes(Asset, FFlockAssetProgress(),
				[Asset, AssetName, Logger](TFlockResult<TArray<uint8>> Downloaded)
				{
					if (!Downloaded.bSuccess)
					{
						Logger->LogInfo(FString::Printf(TEXT("Self-test: download asset '%s' -> failed (%s)"),
							*AssetName, *Downloaded.Error.Message));
						return;
					}
					const int64 Received = Downloaded.Value.Num();
					const FString Mismatch = (Asset.SizeBytes < 0 || Asset.SizeBytes == Received)
						? FString()
						: FString::Printf(TEXT(" -- record says %lld"), Asset.SizeBytes);
					Logger->LogInfo(FString::Printf(TEXT("Self-test: download asset '%s' -> %lld byte(s)%s"),
						*AssetName, Received, *Mismatch));
				});
		});
	}

	/**
	 * Game + config reads against the configured backend. These routes are public (no sign-in gate), so
	 * this runs signed out as independent one-shots that narrate their own result — it does not own
	 * teardown. Each completion captures the raw providers (kept alive by the caller's AddToRoot) and the
	 * logger, never `this`. The config leg lists what tags return, then resolves the first config's data —
	 * the patch-else-config path — and prints a preview of the flattened values.
	 */
	void RunConfigSweep(FFlockGameProvider* Game, FFlockConfigProvider* Config, const TSharedRef<IFlockLogger>& Logger)
	{
		if (Game != nullptr)
		{
			Game->GetGame([Logger](TFlockResult<FFlockGameSchema> Result)
			{
				Logger->LogInfo(Result.bSuccess
					? FString::Printf(TEXT("Self-test: game -> id=%s name='%s' stage=%s"), *Result.Value.Id, *Result.Value.Name, *Result.Value.Stage)
					: FString::Printf(TEXT("Self-test: game -> failed (%s)"), *Result.Error.Message));
			});

			Game->GetGameVersion([Logger](TFlockResult<FFlockGameVersionSchema> Result)
			{
				Logger->LogInfo(Result.bSuccess
					? FString::Printf(TEXT("Self-test: game version -> id=%s name='%s'"), *Result.Value.Id, *Result.Value.Name)
					: FString::Printf(TEXT("Self-test: game version -> failed (%s)"), *Result.Error.Message));
			});
		}

		if (Config != nullptr)
		{
			// An overview of what this game version has, so a name miss below is easy to diagnose.
			Config->GetConfigsByTag(EFlockConfigTag::Any, [Logger](TFlockResult<TArray<FFlockGameConfigSchema>> Result)
			{
				Logger->LogInfo(Result.bSuccess
					? FString::Printf(TEXT("Self-test: configs by tag(Any) -> %d config(s)."), Result.Value.Num())
					: FString::Printf(TEXT("Self-test: configs by tag(Any) -> failed (%s)"), *Result.Error.Message));
			});

			// Pull each known config by name (game_config/by-name/{name}), then resolve its effective data
			// — the patch for this game version if one exists, else the config's own base values.
			for (const TCHAR* const RawName : DemoConfigNames)
			{
				const FString ConfigName = RawName;
				Config->GetConfigByName(ConfigName, [Config, ConfigName, Logger](TFlockResult<FFlockGameConfigSchema> Result)
				{
					if (!Result.bSuccess)
					{
						Logger->LogInfo(FString::Printf(
							TEXT("Self-test: config by name '%s' -> failed (%s)"), *ConfigName, *Result.Error.Message));
						return;
					}
					Logger->LogInfo(FString::Printf(
						TEXT("Self-test: config by name '%s' -> id=%s tag=%s fields=%d"),
						*ConfigName, *Result.Value.Id, *Result.Value.Tag, Result.Value.Data.GetFieldNames().Num()));

					const FString ConfigId = Result.Value.Id;
					Config->ResolveConfigData(ConfigId, [ConfigName, ConfigId, Logger](TFlockResult<FFlockStructuredData> DataResult)
					{
						if (!DataResult.bSuccess)
						{
							Logger->LogInfo(FString::Printf(
								TEXT("Self-test: resolve '%s' -> failed (%s)"), *ConfigName, *DataResult.Error.Message));
							return;
						}
						const FString Preview = DataResult.Value.ToJsonString().Left(300);
						Logger->LogInfo(FString::Printf(
							TEXT("Self-test: resolve '%s' (id=%s) -> %d field(s); data=%s"),
							*ConfigName, *ConfigId, DataResult.Value.GetFieldNames().Num(), *Preview));
					});
				});
			}
		}
	}

	void RunFlockSelfTest()
	{
		// Verbose logger so every breadcrumb prints, regardless of project settings.
		const TSharedRef<IFlockLogger> Logger = MakeShared<FFlockUnrealLogger>(/*bVerbose*/ true);
		Logger->LogInfo(TEXT("Self-test: starting."));

		// UFlockSubsystem is a UGameInstanceSubsystem (ClassWithin=UGameInstance), so its Outer must be a
		// UGameInstance — creating it directly under the transient package trips a "created in invalid
		// Outer" ensure. A throwaway transient GameInstance is a valid Outer for this driver.
		//
		// Both are rooted for the run: the register -> login chain finishes asynchronously, and teardown
		// must not race a GC that would drop the subsystem (and its provider) mid-chain.
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		UFlockSubsystem* Sdk = NewObject<UFlockSubsystem>(GameInstance);
		GameInstance->AddToRoot();
		Sdk->AddToRoot();
		Sdk->SetLogger(Logger);

		Logger->LogInfo(TEXT("Self-test: initializing from Project Settings > Flock SDK."));
		Sdk->InitializeFromSettings();
		if (!Sdk->IsInitialized())
		{
			Logger->LogError(FString::Printf(
				TEXT("Self-test: init failed (%s). Fill in Project Settings > Flock SDK and resolve the Game Version, then retry."),
				*Sdk->GetInitializationError()));
			Sdk->RemoveFromRoot();
			GameInstance->RemoveFromRoot();
			return;
		}
		Logger->LogInfo(FString::Printf(
			TEXT("Self-test: IsInitialized=true GameId=%s GameVersionId=%s VersionedUrl=%s"),
			*Sdk->GetGameId(), *Sdk->GetGameVersionId(), *Sdk->GetVersionedApiUrl()));

		Logger->LogInfo(FString::Printf(TEXT("Self-test: IsAuthenticated=%s PlayerId='%s' IsRestoringSession=%s"),
			Sdk->IsAuthenticated() ? TEXT("true") : TEXT("false"),
			*Sdk->GetPlayerId(),
			Sdk->IsRestoringSession() ? TEXT("true") : TEXT("false")));

		// ── Analytics (signed out) ──
		// A session needs a player id, so none can open yet. What can be shown here is that the log
		// API and the disk spool work before sign-in — an entry recorded now is not lost, and the
		// sweep after sign-in delivers it. Screen views are deliberately NOT called here: they only
		// count against a live session, so they would be a silent no-op.
		FFlockAnalyticsProvider* Analytics = Sdk->GetAnalyticsProvider();
		if (Analytics != nullptr)
		{
			Logger->LogInfo(FString::Printf(
				TEXT("Self-test: analytics signed out — HasConsent=%s HasActiveSession=%s PendingSpooled=%d"),
				Analytics->HasConsent() ? TEXT("true") : TEXT("false"),
				Analytics->HasActiveSession() ? TEXT("true") : TEXT("false"),
				Analytics->GetPendingEventCount()));

			Analytics->LogEvent(TEXT("self-test event (signed out)"),
				FFlockMetadata().Add(TEXT("source"), TEXT("Flock.SelfTest")).Add(TEXT("signedIn"), false));
			Logger->LogInfo(FString::Printf(
				TEXT("Self-test: analytics spooled an entry while signed out -> pending=%d (delivered by the sweep after sign-in)."),
				Analytics->GetPendingEventCount()));

			// Automatic capture demo. Nothing below calls the SDK: the log sink picks this up off
			// GLog, walks the callstack, and the next tick turns it into a spooled exception. The
			// SDK's own categories are excluded, so an SDK error could not demonstrate this.
			UE_LOG(LogTemp, Error, TEXT("Flock self-test: simulated unhandled error (automatic capture demo)"));

			// Peek (not dequeue) so the entry still drains to the backend on the next tick. This is
			// the exact payload that will be sent, printed here so a capture problem is visible
			// locally instead of only in the dashboard.
			if (const FFlockLogSink* Sink = Analytics->GetLogSinkForTesting())
			{
				FFlockCapturedLog Captured;
				if (Sink->Peek(Captured))
				{
					TArray<FString> Lines;
					Captured.StackTrace.ParseIntoArrayLines(Lines);
					Logger->LogInfo(FString::Printf(
						TEXT("Self-test: sink captured '%s' [%s] with %d callstack frames:"),
						*Captured.Message, *Captured.Category.ToString(), Lines.Num()));
					for (int32 Index = 0; Index < FMath::Min(Lines.Num(), 8); ++Index)
					{
						Logger->LogInfo(FString::Printf(TEXT("Self-test:   #%d %s"), Index, *Lines[Index]));
					}
					if (Lines.Num() > 8)
					{
						Logger->LogInfo(FString::Printf(TEXT("Self-test:   ... %d more frames"), Lines.Num() - 8));
					}
				}
				else
				{
					Logger->LogError(TEXT("Self-test: the engine error was NOT captured by the sink."));
				}
			}

			if (GIsEditor)
			{
				Logger->LogInfo(TEXT("Self-test: analytics crash reporting is off in the editor "
					"(stopping Play-In-Editor is not an app death), so no app_termination check runs here."));
			}
		}
		else
		{
			Logger->LogInfo(TEXT("Self-test: analytics is off in Project Settings > Flock SDK; skipping the analytics sweep."));
		}

		// ── Config & Game (signed out) ──
		// Public routes, so this runs before sign-in. Independent one-shots; the auth chain still owns
		// teardown, and the subsystem stays rooted until it fires, so these late arrivals are safe.
		Logger->LogInfo(TEXT("Self-test: config + game sweep (public routes, signed out)."));
		RunConfigSweep(Sdk->GetGameProvider(), Sdk->GetConfigProvider(), Logger);

		// Shop catalog is public too; the purchase + inventory legs need sign-in and run in the auth chain.
		Logger->LogInfo(TEXT("Self-test: shop catalog sweep (public routes, signed out)."));
		RunShopSweep(Sdk->GetShopProvider(), Logger);

		// Assets are public as well, and the download leg is what proves object storage is reachable from
		// the client rather than only from the API.
		Logger->LogInfo(TEXT("Self-test: asset sweep (public routes, signed out)."));
		RunAssetSweep(Sdk->GetAssetProvider(), Logger);

		Logger->LogInfo(TEXT("Self-test: Logout to start from a clean signed-out state (safe when already signed out)."));
		Sdk->Logout();

		// Shut down + un-root once the register -> login chain ends. Safe to call while the one-shots are
		// still in flight: their completions hold their own shared refs and never touch the subsystem.
		TFunction<void()> Teardown = [Logger, GameInstance, Sdk]()
		{
			Logger->LogInfo(TEXT("Self-test: shutting down."));
			Sdk->ShutdownSdk();
			Sdk->RemoveFromRoot();
			GameInstance->RemoveFromRoot();
			Logger->LogInfo(TEXT("Self-test: complete."));
		};

		FFlockAuthProvider* Auth = Sdk->GetAuthProvider();
		if (!Auth)
		{
			Teardown();
			return;
		}

		FSignedInSweeps Sweeps;
		Sweeps.Analytics = Analytics;
		Sweeps.Shop = Sdk->GetShopProvider();
		Sweeps.Commands = Sdk->GetCommandProvider();
		Sweeps.Players = Sdk->GetPlayerProvider();
		Sweeps.Leaderboards = Sdk->GetLeaderboardProvider();
		Sweeps.Notifications = Sdk->GetNotificationProvider();
		Sweeps.Events = Sdk->GetEvents();

		RunAuthSweep(*Auth, Sweeps, Logger, Teardown);
		Logger->LogInfo(TEXT("Self-test: auth sweep dispatched; the signed-in shop, commands, and analytics sweeps "
			"run once it signs in, and teardown follows those."));
	}

	FAutoConsoleCommand GFlockSelfTestCommand(
		TEXT("Flock.SelfTest"),
		TEXT("Drives the Flock SDK surface (boot/init + config/game + auth + shop + commands + assets + analytics) and narrates each step to the log (development builds only)."),
		FConsoleCommandDelegate::CreateStatic(&RunFlockSelfTest));
}

#endif // !UE_BUILD_SHIPPING
