// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockErrorCode.h"
#include "Http/FlockErrorHints.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockResult.h"
#include "Internationalization/Regex.h"
#include "Models/FlockGameModels.h"
#include "FlockLogger.h"
#include "Tests/Support/FlockFakeTransport.h"

namespace
{
	struct FMultiplayerRefusal
	{
		int32 Status;
		EFlockErrorType Type;
		const TCHAR* Code;
		EFlockErrorCode Member;
		const TCHAR* Body;
	};

	// Bodies as the backend sends them: 21 copied from live answers, the other 8 from the spec's examples or the backend's own messages.
	const FMultiplayerRefusal MultiplayerRefusals[] = {
		{ 409, EFlockErrorType::Network, TEXT("matchmaking.already_queued"), EFlockErrorCode::MatchmakingAlreadyQueued,
			TEXT(R"BODY({"detail":{"code":"matchmaking.already_queued","message":"Player 01M46J2QM0KSQ1X3QNF03RRT6V is already queued for this game"}})BODY") },
		{ 404, EFlockErrorType::Network, TEXT("matchmaking.match_not_found"), EFlockErrorCode::MatchmakingMatchNotFound,
			TEXT(R"BODY({"detail":{"code":"matchmaking.match_not_found","message":"Match not found"}})BODY") },
		{ 403, EFlockErrorType::Auth, TEXT("matchmaking.not_party_leader"), EFlockErrorCode::MatchmakingNotPartyLeader,
			TEXT(R"BODY({"detail":{"code":"matchmaking.not_party_leader","message":"Only the party leader can queue the party"}})BODY") },
		{ 400, EFlockErrorType::Validation, TEXT("matchmaking.party_too_large"), EFlockErrorCode::MatchmakingPartyTooLarge,
			TEXT(R"BODY({"detail":{"code":"matchmaking.party_too_large","message":"The party has more players than the queue allows"}})BODY") },
		{ 403, EFlockErrorType::Auth, TEXT("matchmaking.player_not_eligible"), EFlockErrorCode::MatchmakingPlayerNotEligible,
			TEXT(R"BODY({"detail":{"code":"matchmaking.player_not_eligible","message":"A player is not eligible for this queue"}})BODY") },
		{ 404, EFlockErrorType::Network, TEXT("matchmaking.queue_not_found"), EFlockErrorCode::MatchmakingQueueNotFound,
			TEXT(R"BODY({"detail":{"code":"matchmaking.queue_not_found","message":"Matchmaking queue not found"}})BODY") },
		{ 409, EFlockErrorType::Network, TEXT("matchmaking.ticket_not_cancelable"), EFlockErrorCode::MatchmakingTicketNotCancelable,
			TEXT(R"BODY({"detail":{"code":"matchmaking.ticket_not_cancelable","message":"This ticket is no longer queued"}})BODY") },
		{ 404, EFlockErrorType::Network, TEXT("matchmaking.ticket_not_found"), EFlockErrorCode::MatchmakingTicketNotFound,
			TEXT(R"BODY({"detail":{"code":"matchmaking.ticket_not_found","message":"Ticket not found"}})BODY") },
		{ 409, EFlockErrorType::Network, TEXT("matchmaking.version_mismatch"), EFlockErrorCode::MatchmakingVersionMismatch,
			TEXT(R"BODY({"detail":{"code":"matchmaking.version_mismatch","message":"Player 01M46J4VCTQ54NSPP206GSA069 is on a different game version than the party leader, and this queue matches same-version players only"}})BODY") },
		{ 409, EFlockErrorType::Network, TEXT("multiplayer.already_in_session"), EFlockErrorCode::MultiplayerAlreadyInSession,
			TEXT(R"BODY({"detail":{"code":"multiplayer.already_in_session","message":"Player is already in a live multiplayer session for this game"}})BODY") },
		{ 404, EFlockErrorType::Network, TEXT("multiplayer.invalid_join_code"), EFlockErrorCode::MultiplayerInvalidJoinCode,
			TEXT(R"BODY({"detail":{"code":"multiplayer.invalid_join_code","message":"Join code is not valid"}})BODY") },
		{ 400, EFlockErrorType::Validation, TEXT("multiplayer.invalid_join_token"), EFlockErrorCode::MultiplayerInvalidJoinToken,
			TEXT(R"BODY({"detail":{"code":"multiplayer.invalid_join_token","message":"Join token is not valid"}})BODY") },
		{ 500, EFlockErrorType::Network, TEXT("multiplayer.join_code_unavailable"), EFlockErrorCode::MultiplayerJoinCodeUnavailable,
			TEXT(R"BODY({"detail":{"code":"multiplayer.join_code_unavailable","message":"Could not allocate a join code, try again"}})BODY") },
		{ 429, EFlockErrorType::Network, TEXT("multiplayer.mint_rate_limited"), EFlockErrorCode::MultiplayerMintRateLimited,
			TEXT(R"BODY({"detail":{"code":"multiplayer.mint_rate_limited","message":"This player has requested relay credentials too frequently"}})BODY") },
		{ 404, EFlockErrorType::Network, TEXT("multiplayer.not_a_participant"), EFlockErrorCode::MultiplayerNotAParticipant,
			TEXT(R"BODY({"detail":{"code":"multiplayer.not_a_participant","message":"You are not in this session"}})BODY") },
		{ 403, EFlockErrorType::Auth, TEXT("multiplayer.not_host"), EFlockErrorCode::MultiplayerNotHost,
			TEXT(R"BODY({"detail":{"code":"multiplayer.not_host","message":"Only the host can perform this action"}})BODY") },
		{ 429, EFlockErrorType::Network, TEXT("multiplayer.seat_mint_limit"), EFlockErrorCode::MultiplayerSeatMintLimit,
			TEXT(R"BODY({"detail":{"code":"multiplayer.seat_mint_limit","message":"This seat has drawn the most relay credentials a seat may (10)"}})BODY") },
		{ 409, EFlockErrorType::Network, TEXT("multiplayer.session_full"), EFlockErrorCode::MultiplayerSessionFull,
			TEXT(R"BODY({"detail":{"code":"multiplayer.session_full","message":"This session is full"}})BODY") },
		{ 404, EFlockErrorType::Network, TEXT("multiplayer.session_not_found"), EFlockErrorCode::MultiplayerSessionNotFound,
			TEXT(R"BODY({"detail":{"code":"multiplayer.session_not_found","message":"Session not found"}})BODY") },
		{ 404, EFlockErrorType::Network, TEXT("multiplayer.target_not_a_participant"), EFlockErrorCode::MultiplayerTargetNotAParticipant,
			TEXT(R"BODY({"detail":{"code":"multiplayer.target_not_a_participant","message":"That player is not in this session"}})BODY") },
		{ 409, EFlockErrorType::Network, TEXT("multiplayer.version_mismatch"), EFlockErrorCode::MultiplayerVersionMismatch,
			TEXT(R"BODY({"detail":{"code":"multiplayer.version_mismatch","message":"This session is on a different game version"}})BODY") },
		{ 409, EFlockErrorType::Network, TEXT("party.already_in_party"), EFlockErrorCode::PartyAlreadyInParty,
			TEXT(R"BODY({"detail":{"code":"party.already_in_party","message":"You are already in a party for this game"}})BODY") },
		{ 400, EFlockErrorType::Validation, TEXT("party.cannot_kick_leader"), EFlockErrorCode::PartyCannotKickLeader,
			TEXT(R"BODY({"detail":{"code":"party.cannot_kick_leader","message":"The leader cannot be kicked; leave or transfer leadership instead"}})BODY") },
		{ 409, EFlockErrorType::Network, TEXT("party.full"), EFlockErrorCode::PartyFull,
			TEXT(R"BODY({"detail":{"code":"party.full","message":"This party is full"}})BODY") },
		{ 404, EFlockErrorType::Network, TEXT("party.invalid_invite_code"), EFlockErrorCode::PartyInvalidInviteCode,
			TEXT(R"BODY({"detail":{"code":"party.invalid_invite_code","message":"Invite code is not valid"}})BODY") },
		{ 404, EFlockErrorType::Network, TEXT("party.not_a_member"), EFlockErrorCode::PartyNotAMember,
			TEXT(R"BODY({"detail":{"code":"party.not_a_member","message":"You are not a member of this party"}})BODY") },
		{ 404, EFlockErrorType::Network, TEXT("party.not_found"), EFlockErrorCode::PartyNotFound,
			TEXT(R"BODY({"detail":{"code":"party.not_found","message":"Party not found"}})BODY") },
		{ 403, EFlockErrorType::Auth, TEXT("party.not_party_leader"), EFlockErrorCode::PartyNotPartyLeader,
			TEXT(R"BODY({"detail":{"code":"party.not_party_leader","message":"Only the party leader can perform this action"}})BODY") },
		{ 404, EFlockErrorType::Network, TEXT("party.target_not_a_member"), EFlockErrorCode::PartyTargetNotAMember,
			TEXT(R"BODY({"detail":{"code":"party.target_not_a_member","message":"That player is not a member of this party"}})BODY") },
	};

	// A 403 status picks out the refusals that mean "not allowed", so the sign-in check reads them off the table above.
	constexpr int32 NotAllowedStatus = 403;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockMultiplayerRefusalsParseTest, "Flock.Http.ErrorHints.MultiplayerRefusalsParseToTheirCode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockMultiplayerRefusalsParseTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
	const TSharedRef<FFlockHttpClient> Client = MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>());

	TestEqual(TEXT("one row for each of the 29 multiplayer codes"), static_cast<int32>(UE_ARRAY_COUNT(MultiplayerRefusals)), 29);

	for (const FMultiplayerRefusal& Refusal : MultiplayerRefusals)
	{
		Fake->On(TEXT("refusal"), FFlockFakeTransport::Status(Refusal.Status, Refusal.Body));
		bool bAnswered = false;
		FFlockError Error;
		Client->Get<FFlockGameVersionSchema>(TEXT("http://x/refusal"), {},
			[&bAnswered, &Error](TFlockResult<FFlockGameVersionSchema> Result) { bAnswered = true; Error = Result.Error; });

		if (!TestTrue(FString::Printf(TEXT("%s answered"), Refusal.Code), bAnswered))
		{
			continue;
		}
		TestEqualSensitive(FString::Printf(TEXT("%s: raw code kept"), Refusal.Code), Error.Code, FString(Refusal.Code));
		TestEqual(FString::Printf(TEXT("%s: its own member"), Refusal.Code),
			static_cast<int32>(Error.ErrorCode), static_cast<int32>(Refusal.Member));
		TestEqual(FString::Printf(TEXT("%s: status kept"), Refusal.Code), Error.StatusCode, Refusal.Status);
		TestEqual(FString::Printf(TEXT("%s: error type for its status"), Refusal.Code),
			static_cast<int32>(Error.Type), static_cast<int32>(Refusal.Type));
		TestFalse(FString::Printf(TEXT("%s: the server's reason survives"), Refusal.Code), Error.ServerMessage.IsEmpty());
		TestFalse(FString::Printf(TEXT("%s: carries a hint"), Refusal.Code), Error.Hint.IsEmpty());
		TestEqualSensitive(FString::Printf(TEXT("%s: the hint is its code's"), Refusal.Code), Error.Hint, FFlockErrorHints::For(Refusal.Member));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockMultiplayerPermissionHintsTest, "Flock.Http.ErrorHints.PermissionRefusalsNeverSendThePlayerToSignIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockMultiplayerPermissionHintsTest::RunTest(const FString& Parameters)
{
	// A coded 403 here arrives as an Auth error but means "not allowed", so its hint never sends the player to sign in.
	const FRegexPattern SignInWords(TEXT("\\b(sign|log)(g?ed)?[\\s-]?(in|out)\\b|authenticat"));

	// The pattern has to catch each way a hint could say it, or every "no match" below proves nothing.
	const TCHAR* SaysSignIn[] = { TEXT("sign in again"), TEXT("the signed-in player"), TEXT("Log in first"), TEXT("after a login"),
		TEXT("once logged out"), TEXT("re-authenticate") };
	for (const TCHAR* Phrase : SaysSignIn)
	{
		FRegexMatcher Control(SignInWords, FString(Phrase).ToLower());
		TestTrue(FString::Printf(TEXT("the pattern catches \"%s\""), Phrase), Control.FindNext());
	}
	FRegexMatcher Unrelated(SignInWords, FString(TEXT("start the search from the leader's game")).ToLower());
	TestFalse(TEXT("the pattern leaves an unrelated hint alone"), Unrelated.FindNext());

	int32 Checked = 0;
	for (const FMultiplayerRefusal& Refusal : MultiplayerRefusals)
	{
		if (Refusal.Status != NotAllowedStatus)
		{
			continue;
		}
		const FString Hint = FFlockErrorHints::For(Refusal.Member);
		TestFalse(FString::Printf(TEXT("%s has a hint"), Refusal.Code), Hint.IsEmpty());
		FRegexMatcher Matcher(SignInWords, Hint.ToLower());
		TestFalse(FString::Printf(TEXT("%s's hint sends the player to sign in, but this refusal is about permission: %s"), Refusal.Code, *Hint),
			Matcher.FindNext());
		++Checked;
	}

	TestTrue(TEXT("the refusal table has 403 rows to check"), Checked > 0);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
