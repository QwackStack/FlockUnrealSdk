# Qwack Unreal Engine SDK - Internal Documentation

**Technical documentation for the Qwack Flock SDK architecture and implementation.**

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Data Transformation Pipeline](#data-transformation-pipeline)
3. [Layer Breakdown](#layer-breakdown)
4. [FlockApi Template System](#flockapi-template-system)
5. [Request/Response Flow](#requestresponse-flow)
6. [Project Structure](#project-structure)
7. [Implementation Guide](#implementation-guide)

---

## Architecture Overview

The SDK uses a **6-layer architecture** with clear data transformation boundaries. Each layer transforms data from one representation to another:

```
Game Code (UStructs)
    ↓ Serialize
JSON Strings
    ↓ HTTP
Network Bytes
    ↓ Parse
HTTP Response
    ↓ Deserialize
UStructs
    ↓ Callback
Game Code
```

### Design Principles

1. **Layered Architecture**: Each layer has single responsibility
2. **Type Safety**: USTRUCTs prevent runtime errors
3. **Async-First**: Non-blocking network operations
4. **Template-Based**: Generic FlockApi<T> handles transformation
5. **Envelope Pattern**: Backend wraps responses in error/response/result structure

---

## Data Transformation Pipeline

### The FlockApi<ResponseType> Template

Core transformation utility in `Utils/QwackSDKUtils.h` (lines 131-247):

```cpp
template <typename ResponseType>
struct FlockApi {
    // Delegate for response inspection
    DECLARE_DELEGATE_OneParam(FInspectorCallback, ResponseType&);
    
    // Main API call entry point
    template<typename RequestType, typename BlueprintCallbackDelegate, typename CppDelegate>
    static void CallAPI(
        USHTTPClient* HttpClient,
        RequestType RequestStruct,              // Input: USTRUCT
        FString& ContentString,                 // Output: Debug string
        FSQwackFlockEndpoints Endpoint,         // Endpoint definition
        const TArray<FStringFormatArg>& InOrderedArguments,  // URL params
        const TMultiMap<FString, FString>& QueryParams,      // Query string
        const BlueprintCallbackDelegate& BlueprintCallback,
        const CppDelegate& OnCompletedRequest,
        const FInspectorCallback& ResponseCallback,
        TMap<FString, FString> CustomHeaders
    );
};
```

### Transformation Stages

**Stage 1: Request Serialization**
```cpp
// QwackUtilities::UQStructToJsonString<RequestType>()
USTRUCT → JSON String
FFlockCreateAchievementRequest → {"name":"...", "platforms":[...], "game_id":"..."}
```

**Stage 2: URL Construction**
```cpp
// FString::Format with InOrderedArguments
"/achievement/{achievement_id}" → "/achievement/abc123"
// Query params appended
"/achievement" → "/achievement?page=1&limit=10"
```

**Stage 3: HTTP Request**
```cpp
// USHTTPClient::SendRequest()
JSON String + Headers → IHttpRequest → Network
```

**Stage 4: HTTP Response**
```cpp
// IHttpResponse → FQwackHTTPResponse
Network → {success, StatusCode, FullText}
```

**Stage 5: Response Deserialization**
```cpp
// FJsonObjectConverter::JsonObjectStringToUStruct<ResponseType>()
JSON String → USTRUCT
{"result": {...}} → FFlockAchievementSingleResponse
```

**Stage 6: Callback Execution**
```cpp
// FlockApi::CreateLambda()
Executes: ResponseCallback → BlueprintCallback → OnCompletedRequest
```

---

## Layer Breakdown

### Layer Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Layer 6: GAME CODE                                         │
│  Data: UStructs (FFlockCreateAchievementRequest)           │
│  ↓ Calls subsystem methods                                  │
├─────────────────────────────────────────────────────────────┤
│  Layer 5: SUBSYSTEM (QwackFlockSubsystem)                  │
│  Data: UStructs → JSON preparation                          │
│  ↓ Validates, serializes, calls FlockApi<T>                │
├─────────────────────────────────────────────────────────────┤
│  Layer 4: TRANSFORMATION (FlockApi<T> + Schemas)           │
│  Data: UStructs ↔ JSON Strings                             │
│  ↓ Serialization/Deserialization pipeline                   │
├─────────────────────────────────────────────────────────────┤
│  Layer 3: ENDPOINTS (QwackGameEndpoints)                   │
│  Data: URL templates + HTTP methods                         │
│  ↓ Provides endpoint definitions                            │
├─────────────────────────────────────────────────────────────┤
│  Layer 2: HTTP CLIENT (SHTTPClient)                        │
│  Data: JSON Strings → Network Bytes → JSON Strings         │
│  ↓ IHttpRequest/IHttpResponse wrapper                       │
├─────────────────────────────────────────────────────────────┤
│  Layer 1: FOUNDATION (Config + Utils + Module)             │
│  Data: Configuration, JWT tokens, helpers                   │
│  ↓ Infrastructure services                                  │
└─────────────────────────────────────────────────────────────┘
```

---

## FlockApi Template System

### Overview

The `FlockApi<ResponseType>` template struct is the **core transformation engine** that handles all data conversions between game code and network layer.

**Location**: `Utils/QwackSDKUtils.h` lines 131-247

### Key Components

#### 1. FInspectorCallback Delegate
```cpp
DECLARE_DELEGATE_OneParam(FInspectorCallback, ResponseType&);
```
- Optional callback for response inspection/logging
- Executes before Blueprint/C++ callbacks
- Used for debugging and telemetry

#### 2. CreateLambda() - Response Transformation
```cpp
template<typename BlueprintCallbackDelegate, typename CppDelegate>
static FResponseCallback CreateLambda(
    const BlueprintCallbackDelegate& BlueprintCallback,
    const CppDelegate& OnCompletedRequest,
    const FInspectorCallback& ResponseCallback
)
```

**What it does**:
1. Creates a lambda that captures all three callback types
2. When HTTP response arrives:
   - Deserializes JSON → ResponseType USTRUCT
   - Copies HTTP metadata (StatusCode, success, FullText)
   - Executes callbacks in order: Inspector → Blueprint → C++

**Why three callbacks?**
- **Inspector**: Internal logging/debugging
- **Blueprint**: Visual scripting support
- **C++**: Game code callbacks

#### 3. CallAPI() - Request Transformation
```cpp
template<typename RequestType, typename BlueprintCallbackDelegate, typename CppDelegate>
static void CallAPI(
    USHTTPClient* HttpClient,
    RequestType RequestStruct,                          // Input USTRUCT
    FString& ContentString,                             // Debug output
    FSQwackFlockEndpoints Endpoint,                     // URL + Method
    const TArray<FStringFormatArg>& InOrderedArguments, // {achievement_id}
    const TMultiMap<FString, FString>& QueryParams,     // ?page=1&limit=10
    const BlueprintCallbackDelegate& BlueprintCallback,
    const CppDelegate& OnCompletedRequest,
    const FInspectorCallback& ResponseCallback,
    TMap<FString, FString> CustomHeaders
)
```

**Transformation steps**:
```cpp
// 1. Serialize request struct to JSON
FString JsonBody = QwackUtilities::UQStructToJsonString<RequestType>(RequestStruct);

// 2. Format URL with arguments
FString URL = FString::Format(*Endpoint.EndPoint, InOrderedArguments);
// "/achievement/{achievement_id}" + ["abc123"] → "/achievement/abc123"

// 3. Append query parameters
// "/achievement" + {page:1, limit:10} → "/achievement?page=1&limit=10"

// 4. Add custom headers (auth, API keys, etc.)
CustomHeaders.Add("Authorization", "Bearer " + Token);

// 5. Create response callback using CreateLambda()
FResponseCallback Callback = CreateLambda(...);

// 6. Send HTTP request
HttpClient->SendRequest(URL, HttpMethod, JsonBody, Callback, CustomHeaders);
```

#### 4. CallAPIUsingJson() - Alternative Entry Point
```cpp
template<typename BlueprintCallbackDelegate, typename CppDelegate>
static void CallAPIUsingJson(
    USHTTPClient* HttpClient,
    const FString& JsonString,  // Pre-serialized JSON
    // ... same parameters
)
```
- Used when JSON is already constructed
- Skips struct serialization step
- Useful for custom/dynamic requests

### QwackUtilities Namespace

Helper functions for struct/JSON conversion:

```cpp
namespace QwackUtilities {
    // Check if JSON is empty object
    static bool IsEmptyJson(const FString& JsonStr);
    
    // Convert USTRUCT to JSON string
    template<typename RequestType>
    static FString UQStructToJsonString(RequestType RequestStruct) {
        FString ContentString;
        FJsonObjectConverter::UStructToJsonObjectString(
            RequestType::StaticStruct(), 
            &RequestStruct, 
            ContentString, 
            0, 0
        );
        return IsEmptyJson(ContentString) ? FString() : ContentString;
    }
}
```

**Engine version handling**:
- UE4: Always serializes
- UE5: Checks for `FQwackEmptyRequest` type to skip serialization

### Envelope Parsing

Backend responses follow this structure:
```json
{
    "error": {...},      // Error details (if any)
    "response": {...},   // Metadata
    "result": {...}      // Actual data
}
```

**Parsing function** (`QwackSDKUtils.cpp` lines 148-168):
```cpp
bool UQwackSDKUtils::ParseFlockEnvelopeResponse(
    const FString& FullText, 
    FFLockApiEnvelope& OutResult
) {
    TSharedPtr<FJsonObject> Root;
    FJsonSerializer::Deserialize(Reader, Root);
    
    OutResult.error_json    = ToString(Root->TryGetField("error"));
    OutResult.response_json = ToString(Root->TryGetField("response"));
    OutResult.result_json   = ToString(Root->TryGetField("result"));
    
    return true;
}
```

**Usage in subsystem**:
```cpp
FFLockApiEnvelope Envelope;
ParseFlockEnvelopeResponse(HttpResponse.FullText, Envelope);

// Extract result and deserialize to specific type
FJsonObjectConverter::JsonObjectToUStruct<FFlockAchievement>(
    Envelope.result_json, 
    &OutAchievement
);
```

---

## Layer Details

### Layer 1: Foundation (Configuration & Utilities)

**Files**: 
- `Qwack_ue_Sdk.h/cpp` - Module lifecycle
- `Config/QwackConfig.h/cpp` - Settings
- `Utils/QwackSDKUtils.h/cpp` - Helpers
- `Utils/Schemas.h` - Data structures

**Responsibilities**:
- Module initialization/shutdown
- Configuration management (API keys, URLs, dev mode)
- JWT token utilities (decode, validate, expiration check)
- JSON parsing helpers
- Steam ticket validation
- Struct ↔ JSON conversion

**Key Classes**:
- `FQwack_ue_SdkModule` - Module interface
- `UQwackConfig` - UCLASS(Config=Game) settings
- `UQwackSDKUtils` - Static utility functions
- `QwackUtilities` - Template helpers

**Configuration Properties** (`QwackConfig.h`):
```cpp
FString QwackAPIKey;        // API key
FString GameBaseURI;        // Flock API base URL
FString AuthBaseURI;        // Auth service URL
bool DevelopmentMode;       // Dev mode flag
FString TestingPlayerID;    // Test player ID
FString TestingToken;       // Test auth token
```

**Utility Functions** (`QwackSDKUtils.h/cpp`):
```cpp
ConvertStructArrayToJsonString<T>()  // Array serialization
ParseFlockEnvelopeResponse()         // Parse envelope
DecodeJwtPayload()                   // JWT decode
IsTokenExpired()                     // Token validation
GetValueFromQwackJson()              // JSON extraction
IsValidSteamTicket()                 // Steam validation
```

**Build Dependencies** (`Qwack_ue_Sdk.Build.cs`):
- HTTP, Json, JsonUtilities
- SSL, OpenSSL
- ModularGameplay (for subsystems)

---

### Layer 2: HTTP Client

**Files**: `HTTPClient/SHTTPClient.h/cpp`, `HTTPClient/HTTPResponse.h`

**Purpose**: Wraps Unreal's IHttpRequest/IHttpResponse for async HTTP communication.

**Key Method**:
```cpp
void SendRequest(
    const FString& Endpoint,                    // Full URL
    const FString& RequestType,                 // GET/POST/PUT/DELETE
    const FString& Data,                        // JSON body
    const FQwackFlockResponse& Callback,        // Async callback
    TMap<FString, FString> CustomHeaders        // Headers
);
```

**Response Structure**:
```cpp
struct FQwackHTTPResponse {
    bool success;           // HTTP success
    int StatusCode;         // 200, 404, 500, etc.
    FString FullText;       // Raw JSON response
};

DECLARE_DYNAMIC_DELEGATE_OneParam(FQwackFlockResponse, 
                                   const FQwackHTTPResponse&, Response);
```

**Features**:
- Async/non-blocking
- Auto User-Agent header (UE version)
- Instance identifier for tracking
- Custom header support

---

### Layer 3: Endpoint Registry

**Files**: `Endpoints/QwackGameEndpoints.h/cpp`

**Purpose**: Central registry of all Flock API endpoints (70+ endpoints across 20+ categories).

**Structure**:
```cpp
struct FSQwackFlockEndpoints {
    FString EndPoint;              // Full URL
    EQwackSDKHTTPType RequestType; // GET, POST, PUT, DELETE, etc.
};

enum class EQwackSDKHTTPType : uint8 {
    GET, POST, DELETE, PUT, HEAD, CREATE, OPTIONS, PATCH, UPLOAD
};
```

**Example**:
```cpp
// Static definitions
static FSQwackFlockEndpoints CreateAchievement;
static FSQwackFlockEndpoints GetAchievementByID;

// Implementation
FSQwackFlockEndpoints UQwackFlockGameEndpoints::CreateAchievement = 
    SetupEndpoints("/achievement", EQwackSDKHTTPType::POST);
FSQwackFlockEndpoints UQwackFlockGameEndpoints::GetAchievementByID = 
    SetupEndpoints("/achievement/{achievement_id}", EQwackSDKHTTPType::GET);
```

**Endpoint Categories** (70+ total):
- Achievements, Assets, Bans, Game Config, Currencies
- Documents, Error Reporting, Game Commands/Events/Versions
- Leaderboards, Patches, Player Management/Data
- Segments, Shops/Shop Items, Studio Management
- Third-Party Integrations, User Assets

**URL Templating**: Supports `{param}` placeholders replaced at runtime

---

### Layer 4: Data Schemas

**Files**: `Utils/Schemas.h`

**Purpose**: Strongly-typed USTRUCTs for type-safe API contracts.

**Generic Structures**:
```cpp
// Flock API envelope (backend response wrapper)
struct FFLockApiEnvelope {
    FString error_json;      // Error details (if any)
    FString response_json;   // Metadata
    FString result_json;     // Actual data
};

// Operation result (success/failure info)
struct FFlockOpResult {
    bool bSuccess;           // Operation succeeded
    int32 StatusCode;        // HTTP status code
    FString ErrorMessage;    // Error description
    FString ResultJson;      // Raw result data
};

// Platform definition
struct FFlockPlatform {
    FString id;              // Platform ID
    FString name;            // Platform name (Steam, Epic, etc.)
    FString external_id;     // External platform ID
};
```

**B. Feature-Specific Schemas** (Example: Achievements):
```cpp
// Request to create achievement
struct FFlockCreateAchievementRequest {
    FString name;                    // Achievement name
    TArray<FFlockPlatform> platforms; // Supported platforms
    FString game_id;                 // Target game
};

// Request to update achievement
struct FFlockUpdateAchievementRequest {
    FString name;
    TArray<FFlockPlatform> platforms;
};

// Achievement data
struct FFlockAchievement {
    FString id;                      // Unique ID
    FString name;                    // Display name
    TArray<FFlockPlatform> platforms;
    FString game_id;
    FString created_at;              // Timestamp
    FString updated_at;              // Timestamp
};

// Paginated achievement list
struct FFlockAchievementPage {
    TArray<FFlockAchievement> items; // Achievement list
    int32 total;                     // Total count
    int32 page;                      // Current page
    int32 limit;                     // Items per page
};

// Single achievement response
struct FFlockAchievementSingleResponse {
    FFlockOpResult Meta;             // Request metadata
    FFlockAchievement Achievement;   // Achievement data
};

// Paginated achievement response
struct FFlockAchievementPageResponse {
    FFlockOpResult Meta;
    FFlockAchievementPage AchievementPage;
};
```

**Schema Design Patterns**:
1. **Request/Response Separation**: Separate structs for input and output
2. **Metadata Wrapping**: All responses include `FFlockOpResult` for error handling
3. **Blueprint Compatibility**: All structs use `USTRUCT(BlueprintType)`
4. **JSON Mapping**: Property names match backend JSON fields

**Why This Layer Exists**:
- Compile-time type safety
- Auto-completion in IDE
- Blueprint visual scripting support
- Clear API contracts
- Prevents typos and runtime errors

---

### Layer 5: Game API Subsystem

**Files**: `GameAPI/QwackFlockSubsystem.h/cpp`

**Purpose**: Main API interface for game developers. Orchestrates requests, manages auth, parses responses.

**Key Class**: `UQwackFlockSubsystem : public UGameInstanceSubsystem`

**Lifecycle**:
```cpp
virtual void Initialize(FSubsystemCollectionBase& Collection) override;  // Auto-init
virtual void Deinitialize() override;  // Auto-cleanup
```

**Authentication**:
```cpp
void SetAccessToken(const FString& InAccessToken);
FString GetAccessToken() const;
UPROPERTY(BlueprintAssignable) FFlockOnAuthChanged OnAccessTokenChanged;
```

**API Method Pattern**:
```cpp
DECLARE_DYNAMIC_DELEGATE_OneParam(FFlockAchievementSingleCallback, 
                                   const FFlockAchievementSingleResponse&, Response);

UFUNCTION(BlueprintCallable, Category="Flock|Achievements")
void CreateAchievement(
    const FFlockCreateAchievementRequest& Request,
    const FFlockAchievementSingleCallback& Callback
);
```

**Implementation Pattern**:
1. Validate (HttpClient exists, AccessToken not empty)
2. Serialize request USTRUCT → JSON
3. Get endpoint from registry
4. Add auth headers
5. Call HttpClient->SendRequest()
6. In callback: Parse envelope → Deserialize → Execute game callback

**Helper Methods**:
```cpp
TMap<FString, FString> MakeAuthHeaders() const;
FFlockOpResult MakeMetaFromHttp(const FQwackHTTPResponse& R);
bool TryParseAchievementFromEnvelope(...);
bool TryParseAchievementPage(...);
```

---

### Layer 6: Game Code Layer

**Purpose**: Your game's Blueprints and C++ code that uses the SDK.

#### Usage Patterns:

**1. Get Subsystem Reference**:
```cpp
// C++
UQwackFlockSubsystem* FlockSDK = 
    GetGameInstance()->GetSubsystem<UQwackFlockSubsystem>();

// Blueprint
Get Game Instance -> Get Subsystem (QwackFlockSubsystem)
```

**2. Set Authentication**:
```cpp
FlockSDK->SetAccessToken("your_jwt_token_here");
```

**3. Make API Calls**:
```cpp
// Prepare request
FFlockCreateAchievementRequest Request;
Request.name = "First Victory";
Request.game_id = "my_game_id";

// Add platform
FFlockPlatform Platform;
Platform.name = "Steam";
Platform.external_id = "steam_achievement_id";
Request.platforms.Add(Platform);

// Call API
FlockSDK->CreateAchievement(Request, 
    FFlockAchievementSingleCallback::CreateLambda(
        [](const FFlockAchievementSingleResponse& Response) {
            if (Response.Meta.bSuccess) {
                UE_LOG(LogTemp, Log, TEXT("Achievement created: %s"), 
                       *Response.Achievement.id);
            } else {
                UE_LOG(LogTemp, Error, TEXT("Failed: %s"), 
                       *Response.Meta.ErrorMessage);
            }
        }
    )
);
```

---

## Request/Response Flow

### Complete Request Lifecycle

Detailed trace of an API call through all layers with data transformations:

```
┌─────────────────────────────────────────────────────────────────┐
│ LAYER 6: GAME CODE                                              │
├─────────────────────────────────────────────────────────────────┤
│ 1. Game calls subsystem method                                  │
│    SDK->CreateAchievement(Request, Callback)                    │
│    Data: FFlockCreateAchievementRequest (USTRUCT)              │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ LAYER 5: SUBSYSTEM                                              │
├─────────────────────────────────────────────────────────────────┤
│ 2. Validate prerequisites                                       │
│    - Check HttpClient != nullptr                                │
│    - Check AccessToken not empty                                │
│                                                                  │
│ 3. Get endpoint definition                                      │
│    Endpoint = UQwackFlockGameEndpoints::CreateAchievement       │
│    Data: {EndPoint="/achievement", RequestType=POST}            │
│                                                                  │
│ 4. Prepare auth headers                                         │
│    Headers["Authorization"] = "Bearer " + AccessToken           │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ LAYER 4: TRANSFORMATION (FlockApi<T>)                          │
├─────────────────────────────────────────────────────────────────┤
│ 5. Serialize request USTRUCT → JSON                            │
│    QwackUtilities::UQStructToJsonString<RequestType>()          │
│    Input:  FFlockCreateAchievementRequest                       │
│    Output: {"name":"...", "platforms":[...], "game_id":"..."}   │
│                                                                  │
│ 6. Format URL with parameters                                   │
│    FString::Format("/achievement/{id}", Args)                   │
│    Append query params: ?page=1&limit=10                        │
│                                                                  │
│ 7. Create response callback                                     │
│    FlockApi<T>::CreateLambda(BP, CPP, Inspector)               │
│    Captures all three callback types                            │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ LAYER 2: HTTP CLIENT                                            │
├─────────────────────────────────────────────────────────────────┤
│ 8. Create IHttpRequest                                          │
│    HttpRequest = FHttpModule::Get().CreateRequest()             │
│                                                                  │
│ 9. Configure request                                            │
│    SetURL(Endpoint)                                             │
│    SetVerb("POST")                                              │
│    SetHeader("Content-Type", "application/json")                │
│    SetHeader("Authorization", "Bearer ...")                     │
│    SetHeader("User-Agent", "X-UnrealEngine-Agent/5.x")          │
│    SetContentAsString(JsonBody)                                 │
│                                                                  │
│ 10. Send request (async, non-blocking)                          │
│     HttpRequest->ProcessRequest()                               │
│     Data: JSON String → Network Bytes                           │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ NETWORK / BACKEND                                               │
├─────────────────────────────────────────────────────────────────┤
│ 11. HTTP POST to Flock backend                                  │
│ 12. Backend validates, processes, stores                        │
│ 13. Backend returns JSON response                               │
│     {"error":{...}, "response":{...}, "result":{...}}           │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ LAYER 2: HTTP CLIENT (Response)                                │
├─────────────────────────────────────────────────────────────────┤
│ 14. IHttpResponse received                                      │
│     Data: Network Bytes → JSON String                           │
│                                                                  │
│ 15. Create FQwackHTTPResponse                                   │
│     {                                                            │
│       success = (StatusCode == 200),                            │
│       StatusCode = Response->GetResponseCode(),                 │
│       FullText = Response->GetContentAsString()                 │
│     }                                                            │
│                                                                  │
│ 16. Execute callback delegate                                   │
│     OnProcessRequestComplete.ExecuteIfBound(FQwackHTTPResponse) │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ LAYER 4: TRANSFORMATION (FlockApi<T> Callback)                 │
├─────────────────────────────────────────────────────────────────┤
│ 17. Deserialize JSON → ResponseType USTRUCT                    │
│     FJsonObjectConverter::JsonObjectStringToUStruct<T>()        │
│     Input:  {"result": {...}}                                   │
│     Output: FFlockAchievementSingleResponse                     │
│                                                                  │
│ 18. Copy HTTP metadata                                          │
│     Response.StatusCode = HttpResponse.StatusCode               │
│     Response.success = HttpResponse.success                     │
│     Response.FullText = HttpResponse.FullText                   │
│                                                                  │
│ 19. Execute callbacks in order                                  │
│     - InspectorCallback.ExecuteIfBound(Response)                │
│     - BlueprintCallback.ExecuteIfBound(Response)                │
│     - CppCallback.ExecuteIfBound(Response)                      │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ LAYER 5: SUBSYSTEM (Envelope Parsing)                          │
├─────────────────────────────────────────────────────────────────┤
│ 20. Parse Flock envelope (if needed)                            │
│     ParseFlockEnvelopeResponse(FullText, Envelope)              │
│     Extract: error_json, response_json, result_json             │
│                                                                  │
│ 21. Deserialize result to specific type                         │
│     FJsonObjectConverter::JsonObjectToUStruct<FFlockAchievement>│
│     (Envelope.result_json, &OutAchievement)                     │
│                                                                  │
│ 22. Create final response object                                │
│     FFlockAchievementSingleResponse Response;                   │
│     Response.Meta = MakeMetaFromHttp(HttpResp);                 │
│     Response.Achievement = OutAchievement;                      │
│                                                                  │
│ 23. Execute game callback                                       │
│     Callback.ExecuteIfBound(Response)                           │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ LAYER 6: GAME CODE (Callback)                                  │
├─────────────────────────────────────────────────────────────────┤
│ 24. Handle response                                             │
│     if (Response.Meta.bSuccess) {                               │
│         // Success: Update UI, save data, etc.                  │
│         DisplayAchievement(Response.Achievement);               │
│     } else {                                                     │
│         // Error: Show message, retry, log                      │
│         ShowError(Response.Meta.ErrorMessage);                  │
│     }                                                            │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Data Transformations Summary

| Step | Layer | Input | Transformation | Output |
|------|-------|-------|----------------|--------|
| 1-4 | Game/Subsystem | USTRUCT | Validation | Endpoint + Headers |
| 5 | FlockApi | USTRUCT | Serialize | JSON String |
| 6 | FlockApi | URL Template | Format | Full URL |
| 7-10 | HTTP Client | JSON + URL | HTTP Request | Network Bytes |
| 11-13 | Backend | Request | Process | JSON Response |
| 14-16 | HTTP Client | Network Bytes | Parse | FQwackHTTPResponse |
| 17-19 | FlockApi | JSON String | Deserialize | ResponseType USTRUCT |
| 20-22 | Subsystem | Envelope JSON | Parse + Deserialize | Final Response |
| 23-24 | Game Code | Response USTRUCT | Handle | UI Update |

### Error Handling Flow

```
[ERROR OCCURS]
    ↓
[HTTP CLIENT]
    ↓ Detects network error / HTTP error code
    ↓ Sets success = false
    ↓ Includes error message
[SUBSYSTEM]
    ↓ Checks Meta.bSuccess
    ↓ If false, propagates error
    ↓ Logs error details
[GAME CODE]
    ↓ Checks Response.Meta.bSuccess
    ↓ Displays error to player
    ↓ Implements retry logic (optional)
```

---

## Project Structure

```
Qwack_ue_Sdk/
├── Binaries/                    # Compiled plugin binaries
├── Content/                     # Plugin content (if any)
├── Intermediate/                # Build intermediates
├── Resources/                   # Plugin resources
├── Source/
│   └── Qwack_ue_Sdk/
│       ├── Config/              # LAYER 1: Configuration
│       │   ├── QwackConfig.h
│       │   └── QwackConfig.cpp
│       ├── Endpoints/           # LAYER 3: Endpoint Definitions
│       │   ├── QwackGameEndpoints.h
│       │   └── QwackGameEndpoints.cpp
│       ├── GameAPI/             # LAYER 5: Game API Subsystem
│       │   ├── QwackFlockSubsystem.h
│       │   └── QwackFlockSubsystem.cpp
│       ├── HTTPClient/          # LAYER 2: HTTP Communication
│       │   ├── SHTTPClient.h
│       │   ├── SHTTPClient.cpp
│       │   ├── HTTPResponse.h
│       │   └── HTTPResponse.cpp
│       ├── Utils/               # LAYER 1 & 4: Utilities & Schemas
│       │   ├── QwackSDKUtils.h
│       │   ├── QwackSDKUtils.cpp
│       │   └── Schemas.h
│       ├── Qwack_ue_Sdk.h       # Module header
│       ├── Qwack_ue_Sdk.cpp     # Module implementation
│       └── Qwack_ue_Sdk.Build.cs # Build configuration
├── Qwack_ue_Sdk.uplugin         # Plugin descriptor
└── README.md                     # This file
```

---

## Getting Started

### Installation

1. **Copy Plugin to Project**:
   ```
   YourProject/Plugins/Qwack_ue_Sdk/
   ```

2. **Enable Plugin**:
   - Open your project in Unreal Editor
   - Edit → Plugins
   - Search for "Qwack"
   - Check "Enabled"
   - Restart editor

3. **Configure Settings**:
   - Edit → Project Settings
   - Search for "Qwack"
   - Enter your API key and base URLs

### Configuration

**Project Settings** (Edit → Project Settings → Qwack SDK):
```
Qwack API Key: [Your API key from Qwack dashboard]
Game Base URI: https://api.qwack.com/
Auth Base URI: https://auth.qwack.com/
Development Mode: [✓] Enable for testing
Testing Player ID: [Optional test player ID]
Testing Token: [Optional test token]
```

**Alternative: Config File** (`Config/DefaultGame.ini`):
```ini
[/Script/Qwack_ue_Sdk.QwackConfig]
QwackAPIKey=your_api_key_here
GameBaseURI=https://api.qwack.com/
AuthBaseURI=https://auth.qwack.com/
DevelopmentMode=True
TestingPlayerID=test_player_123
TestingToken=test_token_abc
```

---

## Usage Examples

### Example 1: Create Achievement (C++)

```cpp
#include "GameAPI/QwackFlockSubsystem.h"

void AMyGameMode::CreateNewAchievement()
{
    // Get subsystem
    UQwackFlockSubsystem* SDK = GetGameInstance()
        ->GetSubsystem<UQwackFlockSubsystem>();
    
    // Set auth token (from login)
    SDK->SetAccessToken(PlayerAuthToken);
    
    // Prepare request
    FFlockCreateAchievementRequest Request;
    Request.name = "Master Explorer";
    Request.game_id = "my_game_id";
    
    // Add platforms
    FFlockPlatform SteamPlatform;
    SteamPlatform.name = "Steam";
    SteamPlatform.external_id = "ACH_MASTER_EXPLORER";
    Request.platforms.Add(SteamPlatform);
    
    // Call API
    SDK->CreateAchievement(Request, 
        FFlockAchievementSingleCallback::CreateLambda(
            [this](const FFlockAchievementSingleResponse& Response) {
                if (Response.Meta.bSuccess) {
                    UE_LOG(LogTemp, Log, TEXT("Achievement ID: %s"), 
                           *Response.Achievement.id);
                    // Update UI
                    OnAchievementCreated(Response.Achievement);
                } else {
                    UE_LOG(LogTemp, Error, TEXT("Error: %s"), 
                           *Response.Meta.ErrorMessage);
                    // Show error message
                    ShowErrorDialog(Response.Meta.ErrorMessage);
                }
            }
        )
    );
}
```

### Example 2: Get Achievements (Blueprint)

```
1. Get Game Instance
2. Get Subsystem (QwackFlockSubsystem)
3. Get Achievements
   - Game ID: "my_game_id"
   - Page: 1
   - Limit: 10
   - Name Filter: ""
4. On Response:
   - Branch on Meta.bSuccess
   - TRUE: Loop through AchievementPage.items
   - FALSE: Print Meta.ErrorMessage
```

### Example 3: Update Achievement (C++)

```cpp
void AMyGameMode::UpdateAchievement(const FString& AchievementID)
{
    UQwackFlockSubsystem* SDK = GetGameInstance()
        ->GetSubsystem<UQwackFlockSubsystem>();
    
    FFlockUpdateAchievementRequest Request;
    Request.name = "Updated Achievement Name";
    
    SDK->UpdateAchievement(AchievementID, Request,
        FFlockAchievementSingleCallback::CreateLambda(
            [](const FFlockAchievementSingleResponse& Response) {
                if (Response.Meta.bSuccess) {
                    UE_LOG(LogTemp, Log, TEXT("Updated: %s"), 
                           *Response.Achievement.name);
                }
            }
        )
    );
}
```

### Example 4: Delete Achievement (C++)

```cpp
void AMyGameMode::DeleteAchievement(const FString& AchievementID)
{
    UQwackFlockSubsystem* SDK = GetGameInstance()
        ->GetSubsystem<UQwackFlockSubsystem>();
    
    SDK->DeleteAchievement(AchievementID,
        FFlockOpCallback::CreateLambda(
            [](const FFlockOpResult& Result) {
                if (Result.bSuccess) {
                    UE_LOG(LogTemp, Log, TEXT("Achievement deleted"));
                } else {
                    UE_LOG(LogTemp, Error, TEXT("Delete failed: %s"), 
                           *Result.ErrorMessage);
                }
            }
        )
    );
}
```

### Example 5: Listen for Auth Changes (C++)

```cpp
void AMyPlayerController::BeginPlay()
{
    Super::BeginPlay();
    
    UQwackFlockSubsystem* SDK = GetGameInstance()
        ->GetSubsystem<UQwackFlockSubsystem>();
    
    // Bind to token change event
    SDK->OnAccessTokenChanged.AddDynamic(
        this, &AMyPlayerController::OnTokenChanged
    );
}

void AMyPlayerController::OnTokenChanged(const FString& NewToken)
{
    UE_LOG(LogTemp, Log, TEXT("Auth token updated"));
    // Refresh UI, retry failed requests, etc.
}
```

---

## API Reference

### Available API Categories

Currently implemented:
- ✅ **Achievements**: Full CRUD operations

Planned (endpoints defined, implementation pending):
- 🚧 **Assets**: Game asset management
- 🚧 **Bans**: Player ban system
- 🚧 **Game Config**: Runtime configuration
- 🚧 **Currencies**: Virtual currency
- 🚧 **Documents**: Document storage
- 🚧 **Error Reporting**: Error tracking
- 🚧 **Game Commands**: Server commands
- 🚧 **Game Events**: Event analytics
- 🚧 **Game Versions**: Version management
- 🚧 **Leaderboards**: Ranking systems
- 🚧 **Patches**: Game patching
- 🚧 **Player Management**: Auth & profiles
- 🚧 **Player Data**: User data storage
- 🚧 **Segments**: Player segmentation
- 🚧 **Shops**: In-game stores
- 🚧 **Studio Management**: Team management
- 🚧 **Third-Party Integrations**: External services
- 🚧 **User Assets**: Player inventory

### Achievement API Methods

```cpp
// Create new achievement
void CreateAchievement(
    const FFlockCreateAchievementRequest& Request,
    const FFlockAchievementSingleCallback& Callback
);

// Get achievement by ID
void GetAchievementById(
    const FString& AchievementId,
    const FFlockAchievementSingleCallback& Callback
);

// Get paginated achievement list
void GetAchievements(
    const FString& GameId,
    int32 Page,
    int32 Limit,
    const FString& NameFilter,
    const FFlockAchievementPageCallback& Callback
);

// Update achievement
void UpdateAchievement(
    const FString& AchievementId,
    const FFlockUpdateAchievementRequest& Request,
    const FFlockAchievementSingleCallback& Callback
);

// Delete achievement
void DeleteAchievement(
    const FString& AchievementId,
    const FFlockOpCallback& Callback
);
```

---

## Advanced Topics

### Custom Error Handling

```cpp
void HandleResponse(const FFlockAchievementSingleResponse& Response)
{
    if (!Response.Meta.bSuccess) {
        switch (Response.Meta.StatusCode) {
            case 401:
                // Unauthorized - refresh token
                RefreshAuthToken();
                break;
            case 404:
                // Not found
                ShowMessage("Achievement not found");
                break;
            case 500:
                // Server error - retry
                RetryRequest();
                break;
            default:
                ShowMessage(Response.Meta.ErrorMessage);
        }
    }
}
```

### Token Management

```cpp
void AMyGameMode::LoginPlayer(const FString& SteamTicket)
{
    // Authenticate with backend
    AuthenticateWithSteam(SteamTicket, [this](const FString& Token) {
        // Store token
        UQwackFlockSubsystem* SDK = GetGameInstance()
            ->GetSubsystem<UQwackFlockSubsystem>();
        SDK->SetAccessToken(Token);
        
        // Token is now used automatically in all API calls
    });
}
```

### Batch Operations

```cpp
void CreateMultipleAchievements(const TArray<FString>& Names)
{
    UQwackFlockSubsystem* SDK = GetGameInstance()
        ->GetSubsystem<UQwackFlockSubsystem>();
    
    for (const FString& Name : Names) {
        FFlockCreateAchievementRequest Request;
        Request.name = Name;
        Request.game_id = GameID;
        
        SDK->CreateAchievement(Request,
            FFlockAchievementSingleCallback::CreateLambda(
                [Name](const FFlockAchievementSingleResponse& Response) {
                    if (Response.Meta.bSuccess) {
                        UE_LOG(LogTemp, Log, TEXT("Created: %s"), *Name);
                    }
                }
            )
        );
    }
}
```

---

## Troubleshooting

### Common Issues

**1. "HTTP Client not initialized"**
- **Cause**: Subsystem not properly initialized
- **Solution**: Ensure you're calling APIs after `BeginPlay()`

**2. "Access Token is empty"**
- **Cause**: No authentication token set
- **Solution**: Call `SetAccessToken()` after login

**3. "Failed to parse response"**
- **Cause**: Backend response format changed
- **Solution**: Check backend API version compatibility

**4. Network timeout**
- **Cause**: Slow connection or server issues
- **Solution**: Implement retry logic with exponential backoff

### Debug Logging

Enable detailed logging:
```cpp
// In DefaultEngine.ini
[Core.Log]
LogTemp=Verbose
LOG_FLOCK_GAME_SDK=Verbose
LOG_QWACK_SDK=Verbose
```

---

## Contributing

### Adding New API Categories

1. **Define schemas** in `Utils/Schemas.h`
2. **Add endpoints** in `Endpoints/QwackGameEndpoints.h/cpp`
3. **Implement methods** in `GameAPI/QwackFlockSubsystem.h/cpp`
4. **Test** thoroughly
5. **Document** usage examples

### Code Style

- Follow Unreal Engine coding standards
- Use `UFUNCTION(BlueprintCallable)` for public APIs
- Add detailed comments for complex logic
- Include usage examples in header files

---

## License

Copyright 2022, Qwack. All Rights Reserved.

---

## Support

- **Documentation**: [Qwack Developer Portal](https://developers.qwack.com)
- **API Reference**: [Flock API Docs](https://api.qwack.com/docs)
- **Discord**: [Qwack Community](https://discord.gg/qwack)
- **Email**: support@qwack.com

---

**SDK Version**: 1.0 (In Development)  
**Unreal Engine**: 5.0+  
**Last Updated**: December 2024
