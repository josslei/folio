# Documentation migration record

**Status:** historical location map from the documentation cleanup. New contracts
were not created by changing behavior; existing decisions were regrouped. The
[documentation index](../../README.md) is the live navigation entry point.

| Former document | Current home |
| --- | --- |
| architecture.md | architecture/overview.md (compact baseline), architecture/system.md (explanation), contracts/storage.md (detailed state/lifecycle safety) |
| application-design.md | architecture/system.md (hierarchy), contracts/backend-lifecycle.md (ownership/states/draining), contracts/ipc.md (connection setup), development/plans/application.md (future integration) |
| application-access.md / application-mutations.md | contracts/access.md / contracts/mutations.md |
| ipc-protocol.md / storage-design.md | contracts/ipc.md / contracts/storage.md |
| configuration.md / logging.md / io.md | corresponding contracts/ documents |
| design-decisions.md | architecture/decisions.md |
| roadmap.md | development/roadmap.md (future), development/status.md (current), development/reports/phase-4-implementation.md (completed milestones) |
| recovery-design.md / vault-design.md / git-sync-design.md | development/plans/recovery.md / vault.md / git-sync.md |
| backend-failure-containment-audit.md | development/reports/backend-failure-containment-audit.md |
| Long root AGENTS.md | concise root index, role-local instructions and engineering guides; architecture rules link to contract owners |

Existing lifecycle/thread diagrams, MUT/IPC/BACKEND identifiers and scoped audit
results are preserved. Obsolete intermediate rollout instructions were replaced
by current scope or retained as explicitly historical milestones. Storage's old
paths/type names and completed lifecycle wiring statements were corrected; no
persistent format, operation ordering or durability policy was changed.
