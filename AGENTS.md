## Project Overview

For detailed project overview, features, and usage instructions, please refer to [README.md](README.md).
Update README.md when changes affect features, usage instructions, or implementation status.

## Testing

Run available tests relevant to the changed behavior and wait for them to finish. Fix failures caused by the change before completing the task.
For bug fixes, add a regression test when practical. For new behavior, test normal cases and relevant edge cases.
If automated tests are unavailable or hardware is required, document the manual verification steps and report what was verified and what remains unverified.
Documentation-only changes do not require runtime tests.

## Branch Strategy

Stable branches use the name `v<major>` to preserve compatibility with users' west.yml files.
For breaking changes, create a stable branch with the next major version. Keep existing stable branches compatible.
For non-breaking changes, create a branch from the target stable branch and use its name as the prefix: `v<major>-<description>`.
