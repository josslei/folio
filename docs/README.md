# Documentation index

Read the [compact baseline](architecture/overview.md) for shared architectural
context. Then follow the impact of the task, including connected modules; do not
load every detailed document for a local cosmetic change. The
[principles index](architecture/principles.md) links high-risk decisions to their
contract owners, code and verification.

## Understand the system

| Document | Purpose and audience |
| --- | --- |
| [Architectural baseline](architecture/overview.md) | Compact human-readable working context for everyone, especially agents |
| [System architecture](architecture/system.md) | Human-oriented component diagrams, model and responsibility explanation |
| [Principles](architecture/principles.md) | Concise contract/evidence navigation; not independent rule definitions |
| [Decision register](architecture/decisions.md) | Accepted rationale, consequences and unresolved extensions |

## Find the affected contract

| Task involves… | Authoritative detail |
| --- | --- |
| Startup, process/connection ownership, shutdown, errors or draining | [Backend lifecycle](contracts/backend-lifecycle.md), including the [thread/call-flow diagram](contracts/backend-lifecycle.md#backend-lifecycle-call-flow--implemented) |
| IPC messages, admission, queueing or executor boundaries | [IPC](contracts/ipc.md) |
| Browsing, metadata queries or loading content | [Access](contracts/access.md) |
| Create/Update/Move, locks, failure outcomes or confirmation | [Mutations and editing](contracts/mutations.md), [Storage](contracts/storage.md) |
| Entry layouts, Catalog/Index consistency or Session Marker | [Storage](contracts/storage.md) |
| Factory defaults, schema or effective paths | [Configuration](contracts/configuration.md) |
| Process diagnostics, privacy or logging failures | [Logging](contracts/logging.md) |
| Generic bytes, paths or atomic publication/durability | [I/O](contracts/io.md) |

Contracts describe implemented Normal behavior and mark planned extensions
locally. They favor precise, context-efficient prose while remaining human-readable;
illustrations remain where they clarify an important contract. They are not AGENTS.md
files and are not necessarily paired with duplicate human versions.

## Work on the project

| Document | Purpose / status |
| --- | --- |
| [Coding](engineering/coding.md) | Existing C++/organization conventions and code-documentation responsibilities |
| [Build](engineering/build.md) | Dependency, CMake, compilation and run details |
| [Testing](engineering/testing.md) | Isolation, focused/full validation, concurrency, resource limits and examples |
| [Security](engineering/security.md) | Sensitive-data and dependency/security working constraints |
| [Harness](engineering/harness.md) | Existing facilities versus pending tooling/discovery checks |

Root [AGENTS.md](../AGENTS.md) contains global instructions and routes to distinct
[Frontend](../app/AGENTS.md), [Backend](../server/AGENTS.md) and
[shared-code](../lib/AGENTS.md) constraints. Consult all affected scopes explicitly
for cross-directory tasks; automatic discovery is not assumed verified.

## Current state, future plans and history

- [Current status](development/status.md): implemented capabilities, known limits,
  scoped verification evidence and platform release policy.
- [Roadmap](development/roadmap.md): future sequencing and gates, not authorization.
- [Documentation/harness plan](development/documentation-harness-plan.md): agreed
  audiences, knowledge ownership, sequence and pending cleanup work.
- Active plans: [Application integration](development/plans/application.md),
  [Catalog views and concurrent Access](development/plans/catalog-concurrency.md)
  (agreed architecture; implementation review pending; not implemented),
  [Recovery](development/plans/recovery.md), [Vault](development/plans/vault.md),
  [Git Sync](development/plans/git-sync.md). Accepted direction is distinguished
  from open decisions; these are not implemented capabilities.
- Historical reports: [failure-containment audit](development/reports/backend-failure-containment-audit.md),
  [4.3a verification](development/reports/normal-mutations-verification.md),
  [completed Phase 4 milestones](development/reports/phase-4-implementation.md),
  [former document locations](development/reports/documentation-migration.md).

## Authority and status

Headers define current signatures; implementation/tests provide evidence of actual
behavior; approved contracts define required semantics. A discrepancy is an
inconsistency to investigate, not automatic proof that either code or prose is
correct. Give each contract one owner, allowing linked summaries elsewhere.

Distinguish **Implemented**, **Approved but not implemented**, **Open/Deferred**,
and **Historical/Superseded** as appropriate to a document or section. Historical
verification totals are not live counts or guarantees. Preserve rationale and
future TODOs when updating current contracts. Additional indexes or reference
sections should solve a real navigation problem, not create another required hop.
