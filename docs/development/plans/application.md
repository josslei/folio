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

Push events are approved direction, but names, payloads, revisions, subscriptions,
ordering and reconnect delivery remain open. Request-specific errors belong to
the originating Frontend; shared availability notifications need explicit safe
recipient rules. No temporary notification protocol is authorized.

## Navigation and verification

Catalog queries remain memory-only. Application directory entry may count immediate formal items against Catalog expectations. Count equality is practical confidence, not a guarantee against edits or one-for-one replacement. Count units for module roots including Directory Entries, mismatch responses, and targeted reconciliation API need design. Do not implement automatic global scans from ordinary queries/navigation.

## Design gates

Before implementing the corresponding feature, settle its open contracts: event
recipients/order, UI worker ownership/reconnection, revision checking and Recovery
reconciliation, or navigation count-check units and mismatch handling. Existing
ownership, protocol-v1 and lock contracts are not reopened merely by moving this
plan. Native platform behavior still needs platform-specific verification.

See [roadmap](../roadmap.md), [IPC follow-ups](../../contracts/ipc.md#deferred-decisions)
and [Mutation follow-ups](../../contracts/mutations.md#traceable-follow-up-work).
