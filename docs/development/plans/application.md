# Remaining Application design

**Status:** Push transport implemented; remaining UI work is approved direction, not implementation authorization.
Normal Access, Mutations and Edit Locks already exist. Their current contracts are
[Access](../../contracts/access.md), [Mutations](../../contracts/mutations.md),
[IPC](../../contracts/ipc.md) and [lifecycle](../../contracts/backend-lifecycle.md).

## Frontend integration and events

Public calls submit asynchronously. The Frontend session has continuous Push reception; its Request Worker owns blocking Request I/O
and returns completions to the main event loop. Preserve buffers on disconnection; suspend
Backend-dependent actions, use bounded reconnection and initialize current state
on the new connection. Do not replay unconfirmed Mutations or require a separate
manual Refresh solely because reconnection occurred. Full CLI grammar and editor
presentation remain deferred.

Push invalidation transport, publication hooks and bounded receipt delivery are
implemented. The [IPC contract](../../contracts/ipc.md#push-notifications-and-session-ownership)
is authoritative for pairing, payloads, ACK, handoff, limits and Report behavior.
The [Catalog/concurrent Access plan](catalog-concurrency.md#query-results-and-push-handoff)
explains the result/event requirement. Request-specific errors stay with their
originating Frontend; shared availability uses sanitized notifications.

### Paired Push and request connections — implemented

One logical session owns identity, lifecycle and Edit Locks. Independent Request
and Push I/O prevents either stream's transfers from occupying the other worker.
The Frontend's Request Worker performs blocking Request I/O; its Push Notification Worker ACKs
Push even when no Application request or UI consumer is active. The Backend has
two dedicated workers per paired session. See the
[thread diagram and limits](../../contracts/ipc.md#push-notifications-and-session-ownership).

### Receipt ACK and session failure — implemented

ACK confirms complete valid message reception, before UI work. No retransmission;
timeout, invalid receipt or overflow fails the logical session. Accepted actions
retain identity and execute under existing draining rules. No replay or guarantee
of UI processing, future delivery or Mutation success is implied.

### Cross-connection ordering and remaining decisions

Session-wide coverage starts before read capture. Request-time invalidations are
ACKed immediately and retained. The future UI must apply the returned result before
consuming retained invalidations; streams themselves have no shared arrival order.
The Frontend provides event-driven wait/drain APIs without implementing view refresh.

| ID | Status | Remaining work / revisit condition |
| --- | --- | --- |
| PUSH-01 | Implemented baseline | One-use secure pairing, bounded initialization, sequence receipts, timeout, independent I/O and resource limits are implemented. Revisit concrete defaults if operational measurements justify changing them. |
| PUSH-02 | Open Design Decision / Deferred Implementation | Recipient filtering/subscriptions and coalescing remain absent; reconsider when real UI traffic demonstrates need. Initially broadcast safe invalidations including to the initiator. No generic event bus. |
| PUSH-03 | Deferred Implementation | Complete serialized UI result/event handling, refresh decisions, Draft preservation, bounded reconnection and current-view initialization in 4.4b. The transport coverage/retention boundary is implemented; it must not be defeated by unordered UI callbacks. |
| PUSH-04 | Not Guaranteed | Report delivery has one bounded best-effort window. No persistent delivery, cross-restart replay, UI processing acknowledgment or exactly-once guarantee; revisit only with a separately justified persistence design. |
| PUSH-05 | Open Design Decision | Edit Lock invalidations expose an Entry ID without owner identity. Decide any additional read-only lock-status presentation/API when 4.4b requires it; acquiring still requires an explicit operation. |

## Navigation and verification

Catalog queries remain memory-only. The implemented Backend
[4.3b navigation and Consistency Check contract](catalog-refresh.md#consistency-check-and-navigation)
separates Browse/display from a navigation-triggered Check and user-authorized
Refresh. Its generic Backend Check initially counts all direct entries without
classifying files. That plan owns scope, count overhead and mismatch behavior;
do not add automatic scans to Browse or repeat the contract here.

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
reconciliation and the remaining [maintenance follow-up decisions](catalog-refresh.md#traceable-follow-ups). Existing
ownership, protocol-v1 and lock contracts are not reopened merely by moving this
plan. Native platform behavior still needs platform-specific verification.

See [roadmap](../roadmap.md), [IPC follow-ups](../../contracts/ipc.md#deferred-decisions)
and [Mutation follow-ups](../../contracts/mutations.md#traceable-follow-up-work).

## Minimum TUI — first vertical slice

The initial usable slice queries Records by selected Logs/Memos/Todos section
and displays titles, empty results, warnings and failures. One off-UI request
worker executes synchronous calls; navigation generations discard obsolete
results. Pending section selections replace pending queries, without cancelling
accepted Backend work. Browser query execution and presentation state live under
`app/{include,src}/tui/pages/browser/`. Backend communication remains reusable
outside the TUI; absence of FTXUI calls alone does not make UI state reusable.

This is not the completed section-specific Browse Page: Directory navigation, query
controls, selection/scroll interaction, date columns, editing, Push-driven
reloads and bounded reconnection remain subsequent vertical slices. CLI
Application operations are outside this phase. Closing the TUI joins an active
query; it does not cancel accepted work or guarantee bounded exit for a hung
Backend. Push receipt/ACK remains handled independently by the existing transport.

TUI shortcut labels use compact Emacs notation: `C-` Control, `M-` Alt/Meta,
`S-` Shift and `s-` Super (Command on macOS, when forwarded by the terminal).
Quit is `C-q`, displayed persistently in the Bottom Bar; plain `q` does not
quit. The bundled FTXUI disables POSIX XON/XOFF flow control during its loop.
The Bottom Bar is also the intended place for a future Help shortcut; no Help
binding is implemented or displayed yet.

TUI presentation follows the Shell/Page/Layout/Component boundaries described in
[System architecture](../../architecture/system.md#frontend-and-tui-organization).
Headers mirror source directories under `app/include/`. Browser-specific state
and controls stay together rather than forming a shared Frontend UI framework.
Each section retains its last displayed query outcome while requerying, avoiding
unnecessary clear-and-repopulate transitions. Cached results are provisional,
not a freshness guarantee; Push-driven invalidation remains a later slice.

The first query slice has no manual requery shortcut. The Top Bar remains stable while queries execute.

## Frontend UI terminology — approved

These names describe presentation regions, not Backend modules or new Core types.
A **section** is a product area such as Logs, Memos, Todos or Settings.

| Term | Meaning |
| --- | --- |
| Top Bar | Switches between product sections; absent on focused Record Pages |
| Browser Page | Browsing Page Type with Logs/Memos/Todos contexts, initially using Full View |
| Location Bar | Back, Forward and the current logical path; Back does not necessarily mean Parent |
| Query Controls | Filtering and sorting of the Entries Display without navigating to an unrelated page |
| Entries Display | The resulting Entries; not restricted to a list or one shared layout across sections |
| Bottom Bar | Persistent bottom region for shortcut reminders, feedback and potentially future command input |
| Record Page | Focused Record viewing/editing page; Back and title share a row |

**Navigation** means changing the current section, logical location or page.
Filtering, sorting, selection and scrolling are not navigation. In particular,
an ordinary requery or sort does not imply the navigation-triggered Consistency
Check. Existing Check semantics remain defined by the
[maintenance plan](catalog-refresh.md#consistency-check-and-navigation).

```text
Top Bar
------------------------------------------------------------
Browser Page
  Location Bar       (planned)
  Query Controls     (planned)
  Entries Display
------------------------------------------------------------
Bottom Bar                                              C-q Quit
```

Loading feedback appears in the fixed Bottom Bar; it never replaces the page or
changes Top Bar or Entries Display geometry. During idle normal use the Bottom
Bar shows only `C-q Quit`; request errors and warnings may also appear there.

**Deferred Implementation — UI-COMMAND-01:** Vim-style command input is a possible
future Bottom Bar mode, not an approved command grammar or implementation task.
Revisit its interaction and focus design when command support is authorized.
"Bottom Bar" names the region without restricting it to status messages.
These terminology decisions do not rename existing C++ interfaces by themselves.

### Delayed loading — implemented query slice

`LoadingFeedback` owns its timing, display text and FTXUI rendering. The page
provides a feedback element; the Bottom Bar only arranges it beside the Quit hint.

TUI LoadingFeedback constructor settings control the initial delay (100 ms) and completed
`Loaded` feedback duration (1 second); these are not configuration-file settings yet.
One condition-variable timer thread sleeps until the next show/hide deadline
or state change; it does not poll the UI. The Loaded duration starts when presentation
applies the completed result, not when Loading first appeared. Results are applied
immediately. On completion, visible `Loading...` changes to `Loaded` for at least
one second; completed results are never labeled as still loading. Navigation resets the old indicator
and its deadline. Fast requests never display Loading.

The Bottom Bar keeps its fixed geometry and `C-q Quit` hint throughout. Errors
and warnings take precedence over Loading. No spinner or reserved loading row
is added to the Entries Display. Cached results belong only to the selected
section and remain provisional during a request; there are currently no
interactive Entry actions. Timer destruction wakes and joins independently of
the request worker; existing accepted-request shutdown limitations remain.

### Browser field display — implemented query slice

The Entries Display renders the metadata returned by Query without loading Record
bodies. Logs show their logical `log_date`, creation time, and
title; adjacent equal dates omit the repeated date label. Memos show title, tags,
and modified date/time. Todos show completion, title, priority, and optional
due date/time. Date-only values retain their logical calendar date; timestamp
values use the system’s current local timezone by default, with the offset and
daylight-saving rules applicable to each timestamp, rather than the stored original
timezone.

**Deferred Implementation — BROWSER-FIELDS-01:** Todo Query metadata does not expose
subtask progress; the column displays `—` rather than fabricating zero or loading
all Todo bodies. Directory navigation/display and configurable timezone rendering
remain outside this query-only slice. Column headers and rows share their widths and one-cell scroll-indicator gutter.
Text, dates, and times are left-aligned; Todo checkboxes are centered in their
fixed-width column, and numeric subtask progress is right-aligned. Headers follow
their column alignment, with two-cell inter-column gaps.

### Logical Cursor — approved design

Each active Page or Dialog maintains one valid Logical Cursor targeting a keyboard-interactable element. Arrow keys and Tab move it; Enter activates the target. Cursor, FTXUI focus, selection and activation are distinct: moving over a Browser Top Bar item must not change the active module. Mouse activation must not move or reset the Logical Cursor. Page transitions and removal of targets must leave a valid target. Text editing uses a separate insertion caret, with arrow keys directed to that caret while editing.

Navigation relationships belong to each context under `app/tui/`; no global coordinate-based cursor is required. Integrate with FTXUI focus instead of creating competing focus systems. **Implemented:** the Browser Top Bar separates keyboard cursor movement from module activation; mouse activation preserves the keyboard cursor. The active module has a white background highlight, while brackets mark the keyboard cursor. **Deferred Implementation:** additional Page targets, dialogs and text editors.

## TUI ownership — implemented

`TuiShell` owns the active page composition, persistent `BottomBar` and global
Quit handling. It renders the page first, then reads its feedback through a small
callback and renders the Bottom Bar. No generic page/event framework is added.

`BrowserPage` owns its Browser-specific `TopBar`, `EntriesDisplay` and
`LoadingFeedback`. `BrowserQueryController` coordinates asynchronous Browser queries, module
selection, per-module result retention and relevance generations. It lives under
`tui/logic/browser/`, separate from presentation and shared Frontend communication. `tui.cpp` wires
Backend communication, query lifetime, screen and Shell.

```text
app/{include,src}/tui/
  tui.*
  shell/
    tui_shell.*
    bottom_bar.*
  logic/browser/
    browser_query_controller.*
  pages/browser/
    browser_page.*
    top_bar.*
    entries_display.*
  components/
    loading_feedback.*
```

`layouts/` is an explicit architectural category for reusable compositions and
arrangement rules. No current composition warrants a separate implementation,
so no empty directory or placeholder Layout is created. Record and Settings
Pages and global dialog overlays remain future increments, not placeholder classes.

Frontend communication, launcher and process helpers remain at their existing
`app/{include,src}/` paths. They are reusable communication facilities; this change
does not relocate them or extract a speculative generic request worker.

### Initial runtime integration

Implemented: UI-independent `BackendConnectionHandle` exclusively owns a `ConnectionSession`; the session owns both I/O paths and their
workers, with a single outstanding Request slot through completion consumption. The existing
Push Notification Worker signals the TUI directly through a short wakeup callback;
there is no additional notification waiter or event-dispatch thread. Dedicated
communication Wakeups carry consumable pending-work flags; ordinary input and
rendering never poll for completions or Push Notifications. The TUI event
adapter consumes completion before retained notices and owns deferred Browser reload
intentions. See [runtime ownership and flow](../../architecture/system.md#initial-frontend-runtime).
Cancellation, automatic reconnection and editor/background-save presentation remain
**Deferred Implementation**. Shutdown may wait for an outstanding synchronous call;
bounded completion under OS/IPC hangs is **Not Guaranteed**.

`BackendConnectionHandle` owns the session and exposes one typed asynchronous
Application API. It submits actual IPC operations to the session's Request Worker;
there is no separate synchronous request-sender class or matching wrapper API.
Methods accept arguments and one main-thread result callback. Session creation
returns ownership to transfer into the handle. Each worker groups its connection
and path state.

Runtime consolidation is implemented: BrowserQueryController contains only TUI state and a
latest deferred query intention, with no legacy execution worker. Integration tests
cover obsolete navigation results and Request/Push handoff through BackendConnectionHandle.
Receiver retention uses preallocated slots and wakeup callbacks execute outside
session mutexes. Request callback exceptions end the session and propagate through
main-thread completion consumption; they do not invent authoritative outcomes.
