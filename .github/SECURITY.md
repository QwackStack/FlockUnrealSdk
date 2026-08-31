# Security Policy

## Reporting a vulnerability

Please do **not** open a public issue for security problems.

Use GitHub's private vulnerability reporting: go to the repository's **Security** tab →
**Report a vulnerability** (or open
https://github.com/QwackStack/FlockUnrealSdk/security/advisories/new).
You'll get a response there, and a fix will be coordinated before any public disclosure.

If you can't use GitHub advisories, email <support@qwacks.com> instead and say in the subject
line that it is a security report, so it isn't triaged as an ordinary support ticket.

## Scope

This repository is the client plugin. Report backend or dashboard issues through the same
channels — say which surface you were hitting, since a finding in the API is usually more
urgent than the same finding in a client that talks to it.

Note that the plugin stores auth tokens on disk (encrypted, keyed to the machine, user and
game) and caches responses under the project's `Saved/` directory. Anything that lets one
player read another's cached data, or that leaks a bearer token into a log, is in scope.

## Supported versions

Only the latest release on the
[releases page](https://github.com/QwackStack/FlockUnrealSdk/releases) receives security fixes.
