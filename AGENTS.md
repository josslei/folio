# Folio repository instructions

Folio is a C++20 personal TUI application for Logs, Memos, Todos and Directories.

## Critical boundaries and authorization

- Preserve `folio → {folio_frontend, folio_backend} → folio_shared`: app/ is
  Frontend-only, server/ Backend-only, lib/ shared. app/src/main.cpp is the shared
  dispatcher exception. Frontends use IPC, never private Storage.
- Entry files are authoritative; Catalog/Index are projections. Preserve lifecycle
  ownership, executor-only Storage access and explicit safe finalization.
- Do not run git add, git commit or git push, or rewrite Git history, without
  explicit user authorization. Preserve existing edits/staging; do not perform
  destructive Git operations without authorization.
- Do not expose secrets or real personal data, log sensitive payloads, or silently
  overwrite/relocate authoritative data. See [security](docs/engineering/security.md).
- Work only within the requested scope. Roadmaps and TODOs do not authorize the
  next phase, new dependencies or vendored-source changes.

## Find the relevant guidance

Start with the [compact architectural baseline](docs/architecture/overview.md).
Use the [task index](docs/README.md) and [principles index](docs/architecture/principles.md)
for detailed contracts; broaden reading according to impact, not just edited paths.
For work affecting a role, explicitly read its additional local constraints:
[Frontend](app/AGENTS.md), [Backend](server/AGENTS.md), [shared code](lib/AGENTS.md).
Cross-directory tasks must consult every affected module; do not assume automatic
instruction discovery. No local file duplicates or overrides global authorization.

Follow [coding conventions](docs/engineering/coding.md),
[build instructions](docs/engineering/build.md) and [testing standards](docs/engineering/testing.md).
[Current status](docs/development/status.md) separates capabilities and verification
limits from the [future roadmap](docs/development/roadmap.md). The
[cleanup plan](docs/development/documentation-harness-plan.md) records ongoing harness work.

## Validation and unresolved information

Use parallel incremental compilation and task-relevant tests; the
[testing guide](docs/engineering/testing.md#commands-and-concurrency) supplies commands.
Run the full relevant suite when scope/instructions require it; run resource-limit
tests separately with --parallel 1. Documentation-only edits need link/consistency
review rather than compilation. Report actual checks, failures and platform limits.

Inspect code and contracts before editing. Headers describe actual signatures,
implementation/tests evidence behavior, and approved contracts required semantics.
If they conflict, investigate and surface the discrepancy; do not silently choose
one or redesign the contract. Resolve routine details using established conventions;
ask about unresolved requirements or changes to approved design. Report remaining
uncertainty instead of inventing missing guarantees.
