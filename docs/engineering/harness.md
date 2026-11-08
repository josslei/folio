# Harness inventory and next work

**Status:** documentation structure established; harness completion remains pending.
This page distinguishes existing facilities from future tool choices. Validation
is explicitly invoked; there are no completion hooks or Git hooks.

## Available today

| Facility | Entry point / authority | Limits |
| --- | --- | --- |
| Build and dependency checks | [Build guide](build.md), [CMake](../../CMakeLists.txt) | Explicit role targets and include visibility; no general architecture proof |
| Unit/integration verification | [Testing guide](testing.md), [registration](../../tests/CMakeLists.txt) | Results depend on executed configuration/platform |
| Heavy bounds verification | Opt-in resource_limits label, --parallel 1 | Excluded by default; not a benchmark |
| Global/local instructions | Root AGENTS.md plus app/server/lib AGENTS.md | Cross-directory discovery must still be verified; root explicitly routes task scopes |
| Knowledge discovery | [Documentation index](../README.md), [baseline](../architecture/overview.md), [principles](../architecture/principles.md) | Contracts still require impact-driven review |

## Explicit validation command

Invoke `$validate` in Codex (repository skill under `.agents/skills/validate/`),
or run the same command directly from the repository root:

```sh
python3 scripts/validate.py
python3 scripts/validate.py --fix
python3 scripts/validate.py --checks whitespace docs
python3 scripts/validate.py --resource-limits
```

The default checks documentation links, C++ formatting/static analysis and
staged/unstaged diff whitespace, configures the existing `build/`
tree with tests enabled, builds incrementally with `--parallel`, and runs normal
CTest tests with `--parallel`, excluding resource_limits. CTest 3.29+ is required
for automatic test concurrency. `--build-dir` and `--config` select another build
and configuration. Do not run multiple validations against the same build tree.
`--resource-limits` additionally configures/builds the dedicated resource tree and
runs its tests with `--parallel 1`. Tests fail if no matching tests are registered.
Configuration/build failure blocks dependent tests.

By default the command does not modify source. Explicit `$validate-fix` or
`--fix` formats all first-party C++ files before running the selected checks;
review the diff afterward. This can change pre-existing formatting, including
files with local edits. It never stages changes or applies clang-tidy fixes.
Analyzer findings still require investigation. Neither mode modifies Git. It writes
per-command output and a JSON summary under ignored `build/validation/<UTC-time>/`.
Any failed or blocked requested check returns a nonzero exit status. Whitespace
checks cover Git diffs, not untracked file content. Documentation-only work still
needs separate semantic consistency review.

Formatting and analysis use the repository `.clang-format` and `.clang-tidy`.
Install `clang-format` and `clang-tidy` on PATH (for example from LLVM); missing
tools block validation rather than skip it. Use matching tool versions across
machines. Tool-version compatibility still requires user verification. No tools
are downloaded or installed by the script.

Formatting checks first-party C++ headers/sources without rewriting them. The
configuration follows existing two-space indentation, attached braces, 100-column
wrapping and preserves include order. Existing formatting differences are reported;
this harness change does not reformat them or grandfather violations.

Tidy enables selected bugprone and core/C++ analyzer checks, not broad style or
modernization rules. It checks first-party translation units present in the
compilation database, excluding vendored sources and disabled resource tests.
Enable the resource target separately to analyze it. Headers are analyzed through
including translation units. Use a compilation-database-capable generator such
as Ninja/Makefiles; Xcode/Visual Studio need a separate supported analysis build.
Diagnostics fail validation; no automatic fixes are applied.

The `docs` check validates local inline Markdown links and heading fragments in
README, repository instructions and docs. It ignores fenced examples and external
URLs. Reference-style links, full Markdown/HTML syntax and semantic consistency
remain review obligations; it is not a general Markdown parser.

The skill is explicit-only, leaves `commit-msg` unchanged, and does not run after
each edit or conversation turn. Restart/reload Codex if the new skill is not yet
listed; discovery in the active app still needs verification.

## Pending work

- **User verification pending:** tool availability/version compatibility, formatter
  output, selected analyzer diagnostics and skill discovery.
- **Deferred Implementation — harness work:** verify instruction discovery for
  root-launched and cross-directory tasks. Do not assume a rule was automatically
  loaded merely because it applies to the edited directory.
- **Review obligation:** documentation status and semantic consistency are not
  established by link checking.
- **Deferred Implementation — code-documentation cleanup:** review interface and
  implementation comments after harness conventions are established. This document
  migration does not complete that source-level audit.

For each adopted rule identify its authoritative source, practical automated
check and remaining human/agent review obligation. Keep mechanical changes separate
from semantic changes. Follow the [approved sequence](../development/documentation-harness-plan.md)
and do not silently implement unrelated tooling or later feature phases.
