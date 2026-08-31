# Contributing to the Flock Unreal SDK

Thanks for your interest in the SDK. Bug reports and pull requests are welcome —
here's how to make them land smoothly.

## Reporting bugs and requesting features

Use the [issue templates](https://github.com/QwackStack/FlockUnrealSdk/issues/new/choose).
For security problems, follow [SECURITY.md](SECURITY.md) instead of opening a public issue.

## Before writing code

For anything beyond a small fix, **open an issue first** and wait for a maintainer
to confirm the direction. Changes to the public API surface are weighed carefully
against what already ships — an early heads-up avoids wasted work.

## Development setup

This repo **is** the plugin. It has no standalone build: it compiles as part of a
host project.

1. Create or open a **C++** Unreal project. A Blueprint-only project has no
   `Source/*.Target.cs`, which the codegen refuses and the plugin cannot build against.
2. Clone this repo into that project's `Plugins/` folder, so it lands at
   `YourProject/Plugins/FlockUnrealSdk/`.
3. On Windows you need Visual Studio 2022 with the **Game development with C++**
   workload and its **Unreal Engine installer** optional component. On macOS, Xcode.
4. Regenerate project files and build:

   ```
   "<UE>\Engine\Build\BatchFiles\Build.bat" <YourProject>Editor Win64 Development ^
     -project="<path>\<YourProject>.uproject" -WaitMutex
   ```

   A running editor holds the Live Coding lock and will block this — close it first.
   Prefer a full rebuild over Live Coding whenever a change alters a C++ class layout:
   Live Coding patches functions but leaves already-constructed instances on the old one.

Supported engines are **UE 5.5 to 5.8**. Below the floor the plugin refuses to compile
with a single clear error; above the ceiling it compiles and emits a notice.

## Running the tests

**CI never runs these.** GitHub-hosted runners have no Unreal Engine, so every workflow in
this repo checks text files only. The test suite is a local gate, and the pull request
template is where you record having run it.

Editor context — every Flock test:

```
"<UE>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "<path>\<YourProject>.uproject" ^
  -ExecCmds="Automation RunTests Flock.; Quit" -unattended -nullrhi -NoSplash -log
```

Non-editor context — **the same command with `-game` added**. Only tests declaring
`ClientContext` run there, so a lower count is expected; **zero** is a failure. Run both.
Editor-only testing cannot see a defect that depends on the editor being absent, which is
exactly how a plugin-loading bug once survived a green suite.

Read results from the `Test Completed. Result={...}` lines in
`<YourProject>/Saved/Logs/<YourProject>.log`.

`Flock.SelfTest` is a separate narrated smoke-run against a live backend, not a substitute
for the suite — guards and edge cases belong in automation tests.

## Pull requests

`main` is protected — all changes go through a fork + pull request, reviewed by a
maintainer. Keep each PR scoped to one change; the template checklist is enforced in
review. House rules that trip people up most often:

- **Copyright header on every source file**, tabs for indentation. Generated files carry
  **no** header — they are written into the user's project from their own schema.
- **Wire-model bools have no `b` prefix** (`Success`, `Revoked`); non-wire Blueprint structs
  use normal UE style (`bAlreadyRegistered`).
- **Provider completion lambdas never capture `this`** — shared refs, weak object pointers
  or values only, so teardown with requests in flight stays safe.
- **Enveloped vs raw verbs are read off the OpenAPI response schema**, never guessed from the
  path prefix. Mirror the real wire shape in fixtures: an enveloped fixture passes while a
  bare-response bug waits to surface against a live backend.
- **Whether a route needs auth is read off the spec and nothing else** — don't infer it from
  an observed 401, and don't add a sign-in guard the spec doesn't call for.
- **Never log request bodies.** A login carries a password and every other route a bearer.
  A *failure* response body is fair game — it's the server's own error document.
- **Engine-version guards live only in `Source/Flock/Public/Misc/FlockEngineCompat.h`.** CI
  fails a PR that puts one anywhere else. Where an engine API moves, move onto the portable
  API rather than adding a guard.
- **Never hand-edit the generated tree** in a consuming project — it is codegen output and is
  wiped on every sync. Generation logic lives in `Source/FlockEditor/Private/Codegen/`.

## Releases

Maintainers cut releases by tagging `v<version>` matching `VersionName` in `Flock.uplugin`;
CI attaches the plugin archive to the
[GitHub release](https://github.com/QwackStack/FlockUnrealSdk/releases). Merging to `main`
publishes nothing.

Before tagging, a maintainer runs `Tooling/Build-AllEngines.ps1` locally. It cleans, builds
and runs both test contexts against every installed engine in the supported range, and exits
non-zero if either end went unverified. That is the gate behind the engine range this SDK
claims, and it cannot run in CI — it needs the engines installed.
