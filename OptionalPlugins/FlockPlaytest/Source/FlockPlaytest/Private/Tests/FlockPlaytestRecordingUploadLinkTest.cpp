// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockLogger.h"
#include "FlockPlaytestRecordingUpload.h"
#include "FlockProtokiteClient.h"
#include "Http/FlockHttpClient.h"
#include "Tests/FlockPlaytestFakeTransport.h"
#include "Tests/FlockPlaytestRecordingUploadTestSupport.h"

using namespace FlockPlaytestRecordingUploadTesting;

namespace
{
	struct FLinkFixture
	{
		TSharedRef<FFlockPlaytestFakeTransport> Transport = MakeShared<FFlockPlaytestFakeTransport>();
		TSharedPtr<FFlockProtokiteClient> Client;
		TSharedRef<TOptional<TFlockResult<FFlockPlaytestRecordingUploadLink>>> Result =
			MakeShared<TOptional<TFlockResult<FFlockPlaytestRecordingUploadLink>>>();

		FLinkFixture()
		{
			const TSharedRef<IFlockLogger> Logger = MakeShared<FFlockNullLogger>();
			FFlockRetryPolicy Policy;
			Policy.MaxRetries = 0;
			Policy.InitialDelaySeconds = 0.f;
			Policy.bUseJitter = false;
			Client = MakeShared<FFlockProtokiteClient>(MakeShared<FFlockHttpClient>(Transport, Logger), Policy, Logger);
		}

		void Ask(const FString& ContentType = FlockPlaytestRecordingContentTypes::WebM)
		{
			TMap<FString, FString> Headers;
			Headers.Add(TEXT("X-Flock-API-Key"), TEXT("secret"));
			Headers.Add(TEXT("X-Game-Version-ID"), FlockPlaytestFixtures::GameVersionId);
			const TSharedRef<TOptional<TFlockResult<FFlockPlaytestRecordingUploadLink>>> Out = Result;
			Client->RequestRecordingUploadLink(TEXT("http://localhost:8020"), Headers, UploadSessionId, ContentType,
				[Out](TFlockResult<FFlockPlaytestRecordingUploadLink> Answer) { *Out = Answer; });
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadLinkUrlTest,
	"Flock.Playtest.RecordingUpload.LinkUrlNamesTheSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadLinkUrlTest::RunTest(const FString& Parameters)
{
	const FString Expected = TEXT("http://localhost:8020/game/sdk/playtest-session/") + UploadSessionId + TEXT("/recording-upload");
	TestEqual(TEXT("No trailing slash"),
		FFlockProtokiteClient::MakeRecordingUploadUrl(TEXT("http://localhost:8020"), UploadSessionId), Expected);
	TestEqual(TEXT("Trailing slash"),
		FFlockProtokiteClient::MakeRecordingUploadUrl(TEXT("http://localhost:8020/"), UploadSessionId), Expected);
	return true;
}

/**
 * The route answers GenericResponse_PlaytestRecordingUploadResponse_, so the link is under `result`. A fixture shaped
 * like the root would pass against a reader that gets this wrong, which is why this one mirrors the real envelope.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadLinkEnvelopeTest,
	"Flock.Playtest.RecordingUpload.ReadsTheEnvelopedLink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadLinkEnvelopeTest::RunTest(const FString& Parameters)
{
	FLinkFixture Fixture;
	Fixture.Transport->Answer(UploadRoute, FFlockPlaytestFakeTransport::Status(200, EnvelopedLinkBody()));
	Fixture.Ask();

	if (!TestTrue(TEXT("The call completed"), Fixture.Result->IsSet()))
	{
		return false;
	}
	const TFlockResult<FFlockPlaytestRecordingUploadLink>& Answer = Fixture.Result->GetValue();
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadLinkWithoutUrlTest,
	"Flock.Playtest.RecordingUpload.AnAnswerWithNoUrlFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadLinkWithoutUrlTest::RunTest(const FString& Parameters)
{
	FLinkFixture Fixture;
	Fixture.Transport->Answer(UploadRoute, FFlockPlaytestFakeTransport::Status(200,
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadLinkSendsContentTypeTest,
	"Flock.Playtest.RecordingUpload.AsksForTheContentTypeItWillSend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadLinkSendsContentTypeTest::RunTest(const FString& Parameters)
{
	FLinkFixture Fixture;
	Fixture.Transport->Answer(UploadRoute, FFlockPlaytestFakeTransport::Status(200, EnvelopedLinkBody()));
	Fixture.Ask();

	if (!TestEqual(TEXT("One request"), Fixture.Transport->Requests.Num(), 1))
	{
		return false;
	}
	const FFlockHttpRequest& Request = Fixture.Transport->Requests[0];
	TestEqual(TEXT("A POST"), Request.Method, FString(TEXT("POST")));
	TestTrue(TEXT("It names the session"), Request.Url.Contains(UploadSessionId));
	TestTrue(TEXT("It asks for video/webm"), Request.JsonBody.Contains(TEXT("\"content_type\":\"video/webm\"")));
	// Neither is recorded, so neither is claimed: the server's own defaults are false.
	TestFalse(TEXT("It claims no webcam"), Request.JsonBody.Contains(TEXT("has_webcam")));
	TestFalse(TEXT("It claims no voice"), Request.JsonBody.Contains(TEXT("has_voice")));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
