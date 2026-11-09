# Remaining Application design

**Status:** approved direction with open details; not implementation authorization.
Normal Access, Mutations and Edit Locks already exist. Their current contracts are
[Access](../../contracts/access.md), [Mutations](../../contracts/mutations.md),
[IPC](../../contracts/ipc.md) and [lifecycle](../../contracts/backend-lifecycle.md).

## Frontend integration and events

Public calls remain synchronous. The future UI/IPC worker owns its connection and
returns completion events to the UI. Preserve buffers on disconnection; suspend
Backend-dependent actions, use bounded reconnection and initialize current state
on the new connection. Do not replay unconfirmed Mutations or require a separate
manual Refresh solely because reconnection occurred. Full CLI grammar and editor
presentation remain deferred.

Push events are approved direction. The
[Catalog/concurrent Access plan](catalog-concurrency.md#query-results-and-push-handoff)
records agreed notification ownership and result-before-event direction;
wire names/payloads, exact handoff, subscriptions and reconnect delivery remain
open. Request-specific errors belong to
the originating Frontend; shared availability notifications need explicit safe
recipient rules. No temporary notification protocol is authorized.

## Navigation and verification

Catalog queries remain memory-only. Application directory entry may count immediate formal items against Catalog expectations. Count equality is practical confidence, not a guarantee against edits or one-for-one replacement. Count units for module roots including Directory Entries, mismatch responses, and targeted reconciliation API need design. Do not implement automatic global scans from ordinary queries/navigation.

## Query responsibility follow-up

**TODO APP-QUERY-SEPARATION — Open Design Decision / Deferred Implementation:**
Revisit separating metadata filtering, sorting and query execution from
MetadataCatalog after the [Catalog concurrency design](catalog-concurrency.md) is
elaborated.
Catalog would retain metadata, membership/index invariants and state publication;
a query component could consume a stable view. The boundary, types and any
reuse with future content search remain undecided, so do not extract a generic
query framework now. Revisit when Catalog view APIs are settled or content-search
design establishes concrete shared requirements. Full-text search remains outside
the current refactor; do not duplicate Catalog data or assume both search kinds
need the same evaluation engine.

## Design gates

Before implementing the corresponding feature, settle its open contracts: event
recipients/order, UI worker ownership/reconnection, revision checking and Recovery
reconciliation, or navigation count-check units and mismatch handling. Existing
ownership, protocol-v1 and lock contracts are not reopened merely by moving this
plan. Native platform behavior still needs platform-specific verification.

See [roadmap](../roadmap.md), [IPC follow-ups](../../contracts/ipc.md#deferred-decisions)
and [Mutation follow-ups](../../contracts/mutations.md#traceable-follow-up-work).
