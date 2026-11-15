# Contributing

This repo holds the maintainer's own features for Skald. For now I accept:

- **Bug fixes** to existing features
- **Compatibility updates** when Skald's API changes (mostly will be done by me)
- **Docs and typo fixes**
- **New Features**

## Rules for changes
1. **Public API only.** Use `skapi.h`. Don't include or use core internals.
2. **One feature per folder:** this isnt a limit but its cleaner that way.
3. **Clean shutdown.** Join threads, free memory, and release resources in
   `SKALD_FEATURE_SHUTDOWN`. Features get reloaded often, so leaks add up.
4. **No surprises.** No network calls, telemetry, or hardcoded personal paths(will be checked for).
5. **Match the existing style** and keep changes small and single-purpose.

By contributing, you agree your code is licensed under this repo's MIT License.
