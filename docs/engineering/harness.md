# Harness inventory and next work

**Status:** documentation structure established; harness completion remains pending.
This page distinguishes existing facilities from future tool choices. It introduces
no new commands, enforcement mechanisms or application functionality.

## Available today

| Facility | Entry point / authority | Limits |
| --- | --- | --- |
| Build and dependency checks | [Build guide](build.md), [CMake](../../CMakeLists.txt) | Explicit role targets and include visibility; no general architecture proof |
| Unit/integration verification | [Testing guide](testing.md), [registration](../../tests/CMakeLists.txt) | Results depend on executed configuration/platform |
| Heavy bounds verification | Opt-in resource_limits label, --parallel 1 | Excluded by default; not a benchmark |
| Global/local instructions | Root AGENTS.md plus app/server/lib AGENTS.md | Cross-directory discovery must still be verified; root explicitly routes task scopes |
| Knowledge discovery | [Documentation index](../README.md), [baseline](../architecture/overview.md), [principles](../architecture/principles.md) | Contracts still require impact-driven review |

## Pending work

- **Deferred Implementation — harness work:** select formatter settings and any
  focused static-analysis checks after reviewing existing code/conventions. No
  formatter, clang-tidy configuration or automated style gate is established yet.
- **Deferred Implementation — harness work:** verify instruction discovery for
  root-launched and cross-directory tasks. Do not assume a rule was automatically
  loaded merely because it applies to the edited directory.
- **Deferred Implementation — harness work:** choose repeatable validation entry
  points, including documentation link/status checks where useful. Ad hoc checks
  performed during this refactor are not an installed CI/harness command.
- **Deferred Implementation — code-documentation cleanup:** review interface and
  implementation comments after harness conventions are established. This document
  migration does not complete that source-level audit.

For each adopted rule identify its authoritative source, practical automated
check and remaining human/agent review obligation. Keep mechanical changes separate
from semantic changes. Follow the [approved sequence](../development/documentation-harness-plan.md)
and do not silently implement unrelated tooling or later feature phases.
