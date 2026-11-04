# Public read access — Phase 4.2a

Mutation result and Mutation ID contracts are documented separately in
[Mutation contracts](application-mutations.md); mutation execution remains deferred.

Implemented for Normal Entries. Frontends use `BackendConnectionHandle` through
IPC; they never own a Catalog or pass physical Entry locators. Business methods
return `ApplicationResult<T>`, without private Storage or transport errors.

## Operations

- `browse(DirectoryRef, RecordOrder)`: immediate Records and Directories.
  DirectoryRef accepts a RecordType for a logical Root or an Id for a user
  Directory. Both are directories for navigation; Roots have no fabricated UUID
  and are not persisted Directory Entries. User Directories have no children.
- `query_records(RecordQuery)`: metadata over explicit Root/Directory/All scope.
- `list_directories(DirectoryKind)`: names ascending, UUID ascending on ties.
- `load_log/memo/todo(Id)`: complete existing Core Snapshot, at its current
  Catalog locator; no implicit discovery or Refresh.

BrowseResult has separate `records` and `directories` vectors. All metadata
responses use `ApplicationResponse<T>` with a value and warnings. Public metadata
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

TODO: explicit public Full Refresh and immediate Directory count checks. Neither
is silently performed. Refresh/Recovery actions are not advertised as currently
executable until their public capabilities exist. ErrorAction describes a user
choice; it never triggers a retry or filesystem modification itself.

The three questions remain: what failed, what happened to the operation, and
what can the user do? This read-only phase implements causes/actions. Mutation
outcomes and reconciliation contracts remain Phase 4.2b work.

## Transport and limits

Methods synchronously exchange one request at a time on each connection, with a
30-second read deadline. Frontends schedule these calls appropriately for their
UI. No automatic reconnect or replay occurs. Transport/protocol failure closes
the affected handle; confirmed Application errors leave it usable.

The private version-1 lifecycle protocol retains bounded 64 KiB frames. New read
messages use positional MessagePack arrays independent of persisted schemas.
Responses are chunked internally, so callers do not manage frames. A logical
response is bounded to 64 MiB and collections to 100,000 elements; exceeding
limits returns an explicit error rather than partial results. Query requests
fit one frame. No public pagination or mutation request identity is introduced.
The single Backend loop executes reads and owns all live Catalog state.

## Deferred

Vault, locked placeholders and Directory visibility, Recovery, mutation outcomes,
edit permission, revisions, push events and full-text search are not implemented.
Complete unlocked Vault metadata may eventually use the same complete types;
locked fields must never be faked with defaults. Snapshot loading does not grant
permission to save or imply conflict-free editing. Frontend buffer preservation
and bounded reconnection remain presentation integration work; the existing
welcome TUI has not been expanded by this phase.
