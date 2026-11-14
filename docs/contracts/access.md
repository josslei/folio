# Access operation contract

Mutation result and Mutation ID contracts are documented separately in
[Mutation contracts](mutations.md); Normal Mutation execution and Edit Locks are implemented in Phase 4.3a.

Implemented for Normal Entries. Frontends use `BackendConnectionHandle` through
IPC; they never own a Catalog or pass physical Entry locators. Business methods
return `AccessResult<T>`, without private Storage or transport errors.

## Principles and invariants

Metadata navigation reuses the Catalog without body reads or implicit verification
scans. Complete content loading remains separate and uses current known locators.
Logical Root navigation does not expose filesystem paths or fabricate identities.

## Operations

- `browse(DirectoryRef, RecordOrder)`: immediate Records and Directories.
  DirectoryRef accepts a RecordType for a logical Root or an Id for a user
  Directory. Both are directories for navigation; Roots have no fabricated UUID
  and are not persisted Directory Entries. User Directories have no children.
- `query_records(RecordQuery)`: metadata over explicit Root/Directory/All scope.
- `list_directories(DirectoryKind)`: names ascending, UUID ascending on ties.
- `load_log/memo/todo(Id)`: complete existing Core Snapshot, at its current
  Catalog locator; no implicit discovery or Refresh.

Loads accept [compatible Entry extensions](storage.md#entry-compatibility-and-version-domains),
but readable does not imply writable. Public Load returns `Annotated<Snapshot>`; `EntryReadOnly` warns that
the Backend-owned Catalog assessment prohibits editing. Complete UI handling is
tracked as ENTRY-COMPAT-01.

BrowseResult has separate `records` and `directories` vectors. All metadata
responses use `Annotated<T>` with `.value` and warnings in `.annotation`.
This Application alias specializes `core::Annotated<T, Annotation>` with
`std::vector<ApplicationWarning>`; Storage supplies its own compatibility annotation. Public metadata
contains no filesystem locator or Record body. Core owns the shared RecordType
and the canonical metadata structs in `folio/core/metadata.hpp`. Storage pairs
those values with private locator information using `Located<Metadata>`;
Application responses reuse Core values instead of maintaining duplicate fields.

Queries reuse MetadataCatalog. Filters combine with AND; title/tag matching is
ASCII case-insensitive substring matching, with other UTF-8 bytes exact. Tags
apply only to Logs/Memos. Date/time bounds are inclusive. Invalid ranges and
contradictory type/filter/scope requests are rejected. Default Record order is
Created ascending; UUID ascending breaks ties. Missing sort keys remain last in
either direction. Date due values sort before Datetime values, as in Catalog.

## Errors and consistency

Application errors provide a broad category, specific reason, safe explanation,
and available actions. They translate underlying errors without forwarding raw
parser diagnostics, filesystem locators, or sensitive contents. Missing Catalog
identity is TargetUnavailable; stale authoritative access is a distinct
AuthoritativeDataProblem. Unavailable Catalog state is ApplicationUnavailable.
A usable Inconsistent Catalog still supplies metadata with a warning; Index
publication remains blocked by Coordinator. No query performs verification I/O.

**Implemented Backend contracts:** a separate generic Consistency Check after
navigation Browse/display, and explicit unified Scoped/Full Refresh. Frontend
navigation/UI integration remains deferred. Browse stays Catalog-only;
ordinary requery/sort does not itself trigger a Check. See the authoritative
[4.3b design](../development/plans/catalog-refresh.md#consistency-check-and-navigation).
Neither Check-triggered Refresh nor replay is automatic. Stale-location errors
offer explicit Refresh; `FullRefreshRequired` identifies scoped reconciliation
that requires the user to choose Full Refresh. Recovery remains deferred. ErrorAction describes a user
choice; it never triggers a retry or filesystem modification itself.

The three questions remain: what failed, what happened to the operation, and
what can the user do? This read-only phase implements causes/actions. Mutation
outcomes are implemented in the [Mutation contract](mutations.md#results-and-evidence); persistent reconciliation remains Recovery work.

## Transport and limits

Typed Frontend methods submit asynchronously; the Request Worker exchanges one
request at a time on each connection, with a
bounded admission deadline followed by a separate Accepted acknowledgment and
final result/error. Accepted queue waiting has no short completion deadline;
transfer stalls and disconnections remain errors. Frontends schedule these
blocking calls outside their UI event thread. No automatic reconnect or replay occurs. Transport/protocol failure closes
the affected handle; confirmed Application errors leave it usable.

The private version-1 protocol sends one complete length-prefixed request or
response. MessagePack payloads remain positional arrays independent of persisted
schemas. The transport handles partial OS reads/writes internally; there are no
protocol chunks. A message body is bounded to 64 MiB (including version/kind),
with collections limited to 100,000 elements. Exceeding limits returns an explicit
error rather than partial results. No public pagination is introduced. See the
[unified IPC protocol](ipc.md) for limits and transfer handling.
RequestCoordinator dispatches queries and Loads to the same bounded Access pool.
AccessOrchestrator on the worker captures the query view or resolves a current
locator; Load reservations are established before dispatch and cover the read. Mutations remain sequential; conflicting Loads wait in scheduling,
not in filesystem workers. IPC workers do not access live Storage. See
[execution safety](ipc.md#execution-safety-and-bounds).

## Deferred

The [Catalog/concurrent Access plan](../development/plans/catalog-concurrency.md)
records the implemented concurrent baseline and later Push handoff design.
Query extraction is deferred separately in that plan's linked Application TODO.

Vault, locked placeholders and Directory visibility, persistent Recovery, revision
checking and full-text search are not implemented. Push invalidation transport is implemented; UI refresh handling remains deferred. Normal Mutation
outcomes and Edit Locks are implemented separately from read access.
Complete unlocked Vault metadata may eventually use the same complete types;
locked fields must never be faked with defaults. Snapshot loading does not grant
permission to save or imply conflict-free editing. Frontend buffer preservation
and bounded reconnection remain presentation integration work; the existing
welcome TUI has not been expanded by this phase.

Automatic navigation `check_consistency()` attempts are shared per logical scope
for the Backend session. Pending/running calls share completion; repeated calls
reuse the historical outcome, including failure, without filesystem work. This is
not a freshness guarantee. See the [session-local Check contract](../development/plans/catalog-refresh.md#session-local-navigation-check-suppression).
Explicit recheck semantics remain deferred to TUI design.
