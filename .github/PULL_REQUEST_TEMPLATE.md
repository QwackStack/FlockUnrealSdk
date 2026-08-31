## Summary

<!-- What changes, and why. If anything breaks for callers, say so here and how they migrate. -->

## Linked issue

<!-- Open an issue first for anything beyond a small fix, then link it here. Use "Fixes #123". -->

## How this was tested

<!--
Name what you actually ran. CI has no engine on it, so it never builds or runs a single test — this
section is the only record that any of this was executed.

  Build (close any running editor first; it holds the Live Coding lock):
    "<UE>\Engine\Build\BatchFiles\Build.bat" <YourProject>Editor Win64 Development ^
      -project="<path>\<YourProject>.uproject" -WaitMutex

  Editor context — every Flock test:
    "<UE>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "<path>\<YourProject>.uproject" ^
      -ExecCmds="Automation RunTests Flock.; Quit" -unattended -nullrhi -NoSplash -log

  Non-editor context — the same command with -game added. Only tests declaring ClientContext run
  there, so a lower count than the editor pass is expected; zero is a failure, not a pass. Editor-only
  testing cannot see a defect that depends on the editor being absent.

  Results are the "Test Completed. Result={...}" lines in <YourProject>/Saved/Logs/<YourProject>.log.

Quote both counts. If part of the change was reasoned about rather than run, say which part, and say
what still needs a live backend or dashboard-authored data to confirm.
-->

## Checklist

- [ ] Both test contexts pass locally — editor and `-game` — with the counts quoted above
- [ ] Enveloped vs raw verbs chosen from each endpoint's OpenAPI response schema, and fixtures mirror the real wire shape — an enveloped fixture hides a bare-response bug until it hits a live backend
- [ ] Provider completion lambdas capture shared refs, weak object pointers or values — never `this`
- [ ] Copyright header on every new source file; generated files deliberately carry none
- [ ] `README.md` and the matching `Documentation/` page updated if the public API changed
- [ ] Scope is tight — the requested change and its direct dependencies, nothing bundled

<!--
Checked automatically on every pull request, so there is nothing to tick by hand for these: VersionName
semver validity, a CHANGELOG entry for it, UFlockSubsystem::SdkVersion agreeing with the manifest,
VersionName ahead of main when Source/ changed, the absent EngineVersion key, engine-range agreement
across the manifest, README and compat header, engine-version guards outside FlockEngineCompat.h, and
every wire error code having an enum member.
-->
