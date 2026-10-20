// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Assets/FlockAssetDownloader.h"
#include "FlockLogger.h"
#include "HAL/FileManager.h"
#include "Http/FlockFileUploader.h"
#include "Http/FlockHttpModuleAdapter.h"
#include "Http/FlockHttpShutdown.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Misc/ScopeExit.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace
{
	FString TestFolder()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			TEXT("Transports"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}
}

// ── A request the engine refuses to start is answered once, by every real transport ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockHttpTransportsAnswerARefusedRequestOnceTest, "Flock.Http.Transports.AnswerARefusedRequestOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockHttpTransportsAnswerARefusedRequestOnceTest::RunTest(const FString& Parameters)
{
	// The engine answers a refused request through its completion before ProcessRequest returns, and each transport
	// also answers when ProcessRequest says it failed: a caller would hear the same failure twice and act on it twice.
	// A request with no address is refused that way, once for each of the three transports.
	AddExpectedMessagePlain(TEXT("No URL was specified"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 3);
	{
		FFlockHttpModuleAdapter Adapter(5.f);
		FFlockHttpRequest Request;
		Request.Method = TEXT("GET");
		int32 Answers = 0;
		EFlockHttpResult Result = EFlockHttpResult::Success;
		Adapter.SendAsync(Request, [&Answers, &Result](FFlockHttpResponse Response)
		{
			++Answers;
			Result = Response.Result;
		});
		TestEqual(TEXT("the JSON transport answers once"), Answers, 1);
		TestTrue(TEXT("and as a failure"), Result != EFlockHttpResult::Success);
	}

	const FString Folder = TestFolder();
	IFileManager::Get().MakeDirectory(*Folder, /*Tree*/ true);
	{
		const TSharedRef<IFlockAssetDownloader> Downloader = FlockCreateHttpAssetDownloader(MakeShared<FFlockNullLogger>());
		const FString Target = FPaths::Combine(Folder, TEXT("download.bin"));
		int32 Answers = 0;
		Downloader->DownloadToFile(FString(), Target, 5.f, nullptr, [&Answers](FFlockAssetDownloadOutcome)
		{
			++Answers;
		});
		TestEqual(TEXT("the download transport answers once"), Answers, 1);
		TestFalse(TEXT("and leaves no file"), IFileManager::Get().FileExists(*Target));
	}
	{
		const TSharedRef<IFlockFileUploader> Uploader = FlockCreateHttpFileUploader(MakeShared<FFlockNullLogger>());
		const FString Source = FPaths::Combine(Folder, TEXT("upload.bin"));
		FFileHelper::SaveStringToFile(TEXT("bytes"), *Source);
		int32 Answers = 0;
		bool bUploaded = true;
		Uploader->UploadFile(FString(), Source, TEXT("video/webm"), 5.f, nullptr, [&Answers, &bUploaded](FFlockFileUploadOutcome Outcome)
		{
			++Answers;
			bUploaded = Outcome.IsUploaded();
		});
		TestEqual(TEXT("the upload transport answers once"), Answers, 1);
		TestFalse(TEXT("and not as uploaded"), bUploaded);
	}
	IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
	return true;
}

// ── A game that ends waits for its requests on their way, whatever else is turned on ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockHttpShutdownWaitsWheneverTheGameEndsTest, "Flock.Http.Shutdown.WaitsWheneverTheGameEnds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockHttpShutdownWaitsWheneverTheGameEndsTest::RunTest(const FString& Parameters)
{
	// With analytics off and no multiplayer seat, a purchase or a save on its way lost its answer with the process.
	TestTrue(TEXT("the game ending waits, with nothing of the SDK's own to send"), FlockShouldFinishRequestsBeforeShutdown(true, false));
	TestTrue(TEXT("a session end just sent is waited for"), FlockShouldFinishRequestsBeforeShutdown(false, true));
	TestFalse(TEXT("a shutdown mid-game with nothing to send does not hold the game thread"), FlockShouldFinishRequestsBeforeShutdown(false, false));
	return true;
}

// ── An upload that sends nothing and hears nothing for the stall time is given up, rather than holding its file for good ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockHttpUploadStallTest, "Flock.Http.Transports.AnUploadThatStopsMovingIsGivenUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockHttpUploadStallTest::RunTest(const FString& Parameters)
{
	// A socket that listens and never reads: the connection is made, the first megabytes fill its buffers, and then nothing moves.
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	FSocket* Listener = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("Flock stalled upload test"), false);
	if (!TestNotNull(TEXT("Precondition: a socket"), Listener))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Listener->Close();
		SocketSubsystem->DestroySocket(Listener);
	};
	const TSharedRef<FInternetAddr> Address = SocketSubsystem->CreateInternetAddr();
	bool bValidIp = false;
	Address->SetIp(TEXT("127.0.0.1"), bValidIp);
	Address->SetPort(0);
	if (!TestTrue(TEXT("Precondition: it listens"), bValidIp && Listener->Bind(*Address) && Listener->Listen(1)))
	{
		return false;
	}
	Listener->GetAddress(*Address);

	const FString Folder = TestFolder();
	IFileManager::Get().MakeDirectory(*Folder, /*Tree*/ true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
	};
	const FString Source = FPaths::Combine(Folder, TEXT("recording.bin"));
	TArray<uint8> Bytes;
	Bytes.SetNumZeroed(64 * 1024 * 1024);
	FFileHelper::SaveArrayToFile(Bytes, *Source);

	const TSharedRef<IFlockFileUploader> Uploader = FlockCreateHttpFileUploader(MakeShared<FFlockNullLogger>(), /*StallSeconds*/ 2.f);
	bool bAnswered = false;
	FFlockFileUploadOutcome Answer;
	const double Started = FPlatformTime::Seconds();
	Uploader->UploadFile(FString::Printf(TEXT("http://127.0.0.1:%d/upload"), Address->GetPort()), Source, TEXT("video/webm"),
		/*TimeoutSeconds*/ 0.f, nullptr, [&bAnswered, &Answer](FFlockFileUploadOutcome Outcome)
		{
			bAnswered = true;
			Answer = Outcome;
		});
	while (!bAnswered && FPlatformTime::Seconds() - Started < 45.0)
	{
		FHttpModule::Get().GetHttpManager().Tick(0.02f);
		FTSTicker::GetCoreTicker().Tick(0.02f);
		FPlatformProcess::Sleep(0.02f);
	}

	TestTrue(TEXT("The upload is given up, not left waiting"), bAnswered);
	TestTrue(TEXT("Soon after it stopped moving"), FPlatformTime::Seconds() - Started < 30.0);
	// The engine names a stall a connection failure; either way no answer came.
	TestTrue(TEXT("As no answer"), Answer.Result == EFlockHttpResult::ConnectionError || Answer.Result == EFlockHttpResult::Timeout);
	TestFalse(TEXT("And not as uploaded"), Answer.IsUploaded());
	return true;
}

// ── The one answer a request gets is claimed once, whichever of a transport's two sites asks first ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockHttpTransportsClaimAnswerOnceTest, "Flock.Http.Transports.ATokenClaimsItsAnswerOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockHttpTransportsClaimAnswerOnceTest::RunTest(const FString& Parameters)
{
	FFlockCancelToken Token;
	TestFalse(TEXT("Nothing has answered yet"), Token.bAnswered);
	TestTrue(TEXT("The first claim is granted"), Token.TryClaimAnswer());
	TestTrue(TEXT("And recorded"), Token.bAnswered);
	TestFalse(TEXT("The second is refused"), Token.TryClaimAnswer());
	TestFalse(TEXT("However often it is asked"), Token.TryClaimAnswer());
	return true;
}

#endif // WITH_AUTOMATION_TESTS
