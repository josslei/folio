# Current development status

**Status:** living capability/limitation summary after Phase 4.3a. Update when
capabilities or verification change; milestone reports do not override this page.
The [Catalog/concurrent Access design](plans/catalog-concurrency.md) is agreed and
partially implemented: immutable Catalog states and retained read views now use Immer;
ownership-only Edit Lock acquisition is separate from authoritative Load.
Execution remains sequential; concurrent scheduling is not implemented. The
[implementation increments and review checkpoints](plans/catalog-concurrency.md#agreed-implementation-sequence)
are agreed; recording them does not authorize code changes. Complete
the focused refactor baseline in rounds before returning to remaining 4.3b, not
every later feature described in the plan. See its
[implementation timing](plans/catalog-concurrency.md#implementation-timing).
Push and Frontend integration retain their later roadmap placement. The
[cleanup sequence](documentation-harness-plan.md) records preceding harness work.

## Capability snapshot

| Area | Current state |
| --- | --- |
| Core and Normal Storage | Implemented independent models, authoritative files, Catalog/Index and durable Session Marker |
| Runtime | One Backend, multiple Frontends, ownership, event-driven IPC, shared bounded queue and sequential executor |
| Public Access | Browse, scoped metadata query, Directory listing, typed Record loading |
| Normal Mutations | Create/Update/Move, Directory Create/Rename and immediate Todo status changes |
| Editing control | Connection-owned Entry Edit Locks with accepted-work lifetime retention |
| Configuration/Logging/I/O | Shared factory-template Configuration, process spdlog and byte-oriented atomic file operations |
| Frontend | Welcome TUI and blocking connection API; functional editor and UI/IPC worker integration deferred |
| Not implemented | Public Full Refresh/count checks, Push, persistent Draft/compound Recovery, Delete/Trash, Vault and Git Sync |

## Verification evidence

Prior implementation sessions reported 400 regular and 7 opt-in resource-limit
tests passing on macOS after the AccessOrchestrator refactor. This is a prior
verification snapshot, not a fresh run or an automatically maintained count.
The [earlier 4.3a report](reports/normal-mutations-verification.md) records its
then-current 399/7 result; the [failure-containment audit](reports/backend-failure-containment-audit.md)
records its earlier scope and 378-test result. Counts describe different snapshots.
Documentation reorganization itself does not establish new runtime verification.
Linux/Windows native execution and actual power-loss verification remain pending.

## Safety audit completion

BACKEND-01 is completed for the reviewed Normal paths, with 4.3a mutation
integration follow-through. See the [audit report](reports/backend-failure-containment-audit.md)
for scope, evidence and limitations. Future operations require their own checks;
this is not proof against arbitrary hangs or resource exhaustion.

## BACKEND-02 — Preventable-hang review and agreed limitation

**Review performed / Not Guaranteed:** Inspection of current executor, IPC,
shutdown, Storage loops, and I/O progress handling found no confirmed
preventable-hang defect. Existing request coordination/IPC/lifecycle tests passed on macOS;
this is not proof that all hangs are excluded or native Linux/Windows verification.
Fix concrete preventable hangs when identified. Follow the
[agreed unpredictable-hang boundary](../contracts/backend-lifecycle.md#unpredictable-hangs--agreed-limitation):
operation completion and shutdown duration are not bounded, no unsafe thread
termination or finalization is permitted, and unconfirmed Mutations are not
replayed. Watchdogs, progress tracking, automatic termination, and new timeout
policies are not scheduled. This supersedes the earlier TODO proposing stall
detection in the next runtime subphase; reopen only for a concrete requirement
and design review.

## Platform release verification policy

Linux native runtime verification remains pending and should run when an
environment is available. Windows implementation may remain runtime-unverified
until release preparation. Native process/IPC/security/shutdown verification is
a gate for claiming Windows support, not a requirement to delay every platform's
first release. If Windows cannot be verified and corrected in time, omit Windows
support from the first release. No platform code is removed by this decision;
macOS results and cross-compilation do not establish native Windows correctness.

