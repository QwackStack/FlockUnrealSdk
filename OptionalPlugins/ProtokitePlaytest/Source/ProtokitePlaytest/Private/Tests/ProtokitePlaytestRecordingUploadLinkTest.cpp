// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockLogger.h"
#include "ProtokitePlaytestRecordingUpload.h"
#include "ProtokiteClient.h"
#include "Http/FlockHttpClient.h"
#include "Tests/ProtokitePlaytestFakeTransport.h"
#include "Tests/ProtokitePlaytestRecordingUploadTestSupport.h"

using namespace ProtokitePlaytestRecordingUploadTesting;

namespace
{
	struct FLinkFixture
	{
		TSharedRef<FProtokitePlaytestFakeTransport> Transport = MakeShared<FProtokitePlaytestFakeTransport>();
		TSharedPtr<FProtokiteClient> Client;
		TSharedRef<TOptional<TFlockResult<FProtokitePlaytestRecordingUploadLink>>> Result =
			MakeShared<TOptional<TFlockResult<FProtokitePlaytestRecordingUploadLink>>>();

		FLinkFixture()
		{
			const TSharedRef<IFlockLogger> Logger = MakeShared<FFlockNullLogger>();
			FFlockRetryPolicy Policy;
			Policy.MaxRetries = 0;
			Policy.InitialDelaySeconds = 0.f;
			Policy.bUseJitter = false;
			Client = MakeShared<FProtokiteClient>(MakeShared<FFlockHttpClient>(Transport, Logger), Policy, Logger);
		}

		void Ask(const FString& ContentType = ProtokitePlaytestRecordingContentTypes::WebM)
		{
			TMap<FString, FString> Headers;
			Headers.Add(TEXT("X-Flock-API-Key"), TEXT("secret"));
			Headers.Add(TEXT("X-Game-Version-ID"), ProtokitePlaytestFixtures::GameVersionId);
			const TSharedRef<TOptional<TFlockResult<FProtokitePlaytestRecordingUploadLink>>> Out = Result;
			Client->RequestRecordingUploadLink(TEXT("http://localhost:8020"), Headers, UploadSessionId, ContentType,
				[Out](TFlockResult<FProtokitePlaytestRecordingUploadLink> Answer) { *Out = Answer; });
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadLinkUrlTest,
	"Protokite.Playtest.RecordingUpload.LinkUrlNamesTheSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadLinkUrlTest::RunTest(const FString& Parameters)
{
	const FString Expected = TEXT("http://localhost:8020/game/sdk/playtest-session/") + UploadSessionId + TEXT("/recording-upload");
	TestEqual(TEXT("No trailing slash"),
		FProtokiteClient::MakeRecordingUploadUrl(TEXT("http://localhost:8020"), UploadSessionId), Expected);
	TestEqual(TEXT("Trailing slash"),
		FProtokiteClient::MakeRecordingUploadUrl(TEXT("http://localhost:8020/"), UploadSessionId), Expected);
	return true;
}

/**
 * The route answers GenericResponse_PlaytestRecordingUploadResponse_, so the link is under `result`. A fixture shaped
 * like the root would pass against a reader that gets this wrong, which is why this one mirrors the real envelope.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadLinkEnvelopeTest,
	"Protokite.Playtest.RecordingUpload.ReadsTheEnvelopedLink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadLinkEnvelopeTest::RunTest(const FString& Parameters)
{
	FLinkFixture Fixture;
	Fixture.Transport->Answer(UploadRoute, FProtokitePlaytestFakeTransport::Status(200, EnvelopedLinkBody()));
	Fixture.Ask();

	if (!TestTrue(TEXT("The call completed"), Fixture.Result->IsSet()))
	{
		return false;
	}
	const TFlockResult<FProtokitePlaytestRecordingUploadLink>& Answer = Fixture.Result->GetValue();
	if (!TestTrue(TEXT("It succeeded"), Answer.IsSuccess()))
	{
		return false;
	}
	TestEqual(TEXT("The signed URL"), Answer.Value.UploadUrl, SignedUploadUrl);
	TestEqual(TEXT("The bucket"), Answer.Value.Bucket, FString(TEXT("recordings")));
	TestEqual(TEXT("The key"), Answer.Value.Key, FString(TEXT("a/b.webm")));
	return true;
}

/** A link with nowhere to send the recording is not a usable answer, however successful the status was. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadLinkWithoutUrlTest,
	"Protokite.Playtest.RecordingUpload.AnAnswerWithNoUrlFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadLinkWithoutUrlTest::RunTest(const FString& Parameters)
{
	FLinkFixture Fixture;
	Fixture.Transport->Answer(UploadRoute, FProtokitePlaytestFakeTransport::Status(200,
		TEXT("{\"error\":null,\"response\":null,\"result\":{\"upload_url\":\"   \",\"bucket\":\"b\",\"key\":\"k\"}}")));
	Fixture.Ask();

	if (!TestTrue(TEXT("The call completed"), Fixture.Result->IsSet()))
	{
		return false;
	}
	TestFalse(TEXT("Whitespace is not somewhere to upload to"), Fixture.Result->GetValue().IsSuccess());
	return true;
}

/**
 * The link is signed for the content type the request names, and the PUT has to carry the same one, so the request has
 * to send it rather than leaning on the server's default.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestUploadLinkSendsContentTypeTest,
	"Protokite.Playtest.RecordingUpload.AsksForTheContentTypeItWillSend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestUploadLinkSendsContentTypeTest::RunTest(const FString& Parameters)
{
	FLinkFixture Fixture;
	Fixture.Transport->Answer(UploadRoute, FProtokitePlaytestFakeTransport::Status(200, EnvelopedLinkBody()));
	Fixture.Ask();

	if (!TestEqual(TEXT("One request"), Fixture.Transport->Requests.Num(), 1))
	{
		return false;
	}
	const FFlockHttpRequest& Request = Fixture.Transport->Requests[0];
	TestEqual(TEXT("A POST"), Request.Method, FString(TEXT("POST")));
	TestTrue(TEXT("It names the session"), Request.Url.Contains(UploadSessionId));
	TestTrue(TEXT("It asks for video/webm"), Request.JsonBody.Contains(TEXT("\"content_type\":\"video/webm\""), ESearchCase::CaseSensitive));
	// Neither is recorded, so neither is claimed: the server's own defaults are false.
	TestFalse(TEXT("It claims no webcam"), Request.JsonBody.Contains(TEXT("has_webcam")));
	TestFalse(TEXT("It claims no voice"), Request.JsonBody.Contains(TEXT("has_voice")));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
