# System architecture

**Purpose:** human-oriented explanation of the implemented structure and its
rationale. For a compact working baseline, read [overview](overview.md); precise
requirements live in the linked [principle owners](principles.md).

```text
                         folio executable
                        (shared dispatcher)
                         /              \
               Frontend process       Backend process
                 app/                   server/
                   |                       |
                   +------ local IPC ------+
                   |                       |
                   +---- shared lib/ ------+

Runtime authority: Backend only
Persistent authority: Entry files, not Catalog or Index
```

Both roles link shared code; neither role links the other's implementation as an
API dependency. Shared deployment does not authorize direct Frontend Storage access.

## Domain baseline — Implemented

```text
Entry
├── Record
│   ├── Log
│   ├── Memo
│   └── Todo
└── Directory
```

These categories imply no C++ inheritance. All four independent classes are first-class Entries. Directory's distinct fields and layout justify specialized operations, but never a lower conceptual status. `RecordType` names only Log/Memo/Todo; `DirectoryKind` is Memo/Todo. Within a Normal Catalog all four share one UUID namespace. Normal/Vault are storage/privacy domains, not new identities; future transfers must preserve identity. Cross-domain enforcement is not implemented.

[Public headers](../../lib/include/folio/core) define:

| Model/value | Existing contract |
| --- | --- |
| Id | UUIDv4 generation with libsodium randomness and stduuid parsing; canonical strings; identity is independent of path |
| Snapshot / restore | Typed complete state; restore preserves UUID and creation data without generating another identity |
| TimestampContext | Absolute system-clock timestamp, original timezone string, optional UTF-8 location |
| DateContext | Calendar date, timezone string, optional location; distinct from an instant |
| Log | UUID, created/modified contexts, title, Markdown content, Log date, ordered tags, privacy; defaults Protected |
| Memo | UUID, created/modified contexts, title, Markdown content, tags, optional directory_id, privacy; defaults Normal |
| Todo | UUID, created/modified contexts, title, notes, status, priority, optional DueDate/directory_id, privacy, ordered embedded subtasks; defaults Normal |
| Directory | UUID, Memo/Todo kind, UTF-8 name; no body, privacy field, timestamps, parent_id, or owned member list |
| DueDate | DateContext or TimestampContext; optional on Todo |
| Subtask | Todo-owned content and completion flag; not independently persisted or indexed |

Todo status is Pending/Completed and priority None/Low/Medium/High. Privacy is Normal/Protected/Hidden and has no ordinary post-creation setter. These values alone provide no encryption. NormalRepository accepts only Normal privacy; the default Protected Log therefore needs an explicit Normal choice for current plaintext persistence.

Core validation is deliberately limited: restore validates enum values and applicable calendar dates; Directory validates kind, Todo validates due dates/status/priority and subtask indices, and Log validates Log dates. Core does not verify filesystem membership, timezone databases, or persistence consistency. Empty Todo title/notes are allowed; localized placeholders belong to Frontends. Do not infer uniform validation on all constructors/setters from restore validation. See [domain implementations](../../lib/src/core).

Logs have no user Directory. Memos/Todos support one level, optional membership on the Record, and no nested Directories. The Catalog derives reverse membership; Directory never duplicates it.

## Current component and build boundary — Implemented

[Root CMake](../../CMakeLists.txt) builds three static libraries: `folio_shared`
(`lib/`), `folio_frontend` (`app/`) and `folio_backend` (`server/`). Both roles
depend on shared code; neither role depends on the other. The single `folio`
executable is their shared dispatcher. Only lib/include propagates shared headers; app/include and server/include remain
role-private, with explicit dispatcher access; only internal tests receive private source paths.
FTXUI belongs to the Frontend and CLI11 to the dispatcher. Dependencies remain
pinned submodules; no database or JSON serialization is introduced.

Private process code uses `folio::application::internal`. Domain/Storage namespaces are `folio::core`, `folio::core::storage`, `folio::core::storage::codec`, and Storage `internal` atomic helpers. The shared core::Result<T, E> is a variant; subsystem signatures select their own error contracts.

| Component | Responsibility / evidence |
| --- | --- |
| [NormalRepository](../../server/include/storage/normal_repository.hpp) | Authoritative Normal Entry operations; targeted identity verification, explicit UUID discovery, scoped/full scan and direct-entry counts |
| [MetadataCatalog](../../server/include/storage/metadata_catalog.hpp) | Four typed maps, derived UUID membership sets, non-recursive listing and Record metadata search; no filesystem I/O |
| [CatalogCoordinator](../../server/include/storage/catalog_coordinator.hpp) | Borrows owner-supplied Catalog; startup, full Refresh, Index shutdown persistence, stored synchronization callbacks and consistency status |
| [CatalogMaintenance](../../server/include/operations/catalog_maintenance.hpp) | Check/Refresh handling, Index/Marker I/O and candidate reconstruction; managed publication through Coordinator |
| [IndexCodec](../../server/include/storage/index_codec.hpp) | Pure deterministic MessagePack encode/decode; temporary validated Catalog, no file I/O |
| [I/O](../../lib/include/folio/io/filesystem.hpp) | Generic byte reads, atomic create/replace, and durability operations |
| [Storage common types](../../server/include/storage/common.hpp) | Storage-specific errors, mixed Repository results, and effective-path validation |

There is no separate BinaryIO, IndexStore or IndexBuilder. Frontend launching
lives under `app/src/`; Backend ownership/lifecycle under
`server/src/runtime/`. The [dispatcher](../../app/src/main.cpp) selects the role;
only the Backend initializes Catalog lifecycle and finalizes its Index/Marker.
The historical `binary_io_test.cpp` filename does not denote a production module.

## Backend responsibility overview

**Implemented.** This tree groups responsibilities, not runtime order,
ownership or strict dependency layers. Parentheses identify classes/structs;
brackets identify logical modules. Prepared operation dispatch routes Access requests
to AccessOrchestrator::handle() and mutation/editing requests to MutationOrchestrator.

```text
[Backend]
|
+-- [Lifecycle and exclusive ownership]
|   +-- Backend entry: run_backend() -- free function
|   +-- Lifecycle transition enforcement (BackendStateMachine)
|   +-- [Lifecycle state actions]
|   |     Bootstrap / Serve / Stopping / Report / Finish
|   +-- Resource ownership and lifecycle data (BackendContext)
|   +-- Exclusive Repo and IPC listener ownership (RepositoryOwnership)
|
+-- [IPC]
|   +-- Connections, admission and response delivery (IpcServer)
|   +-- Connection identity and lifetime (ConnectionSession)
|
+-- [Execution]
|   +-- Shared bounded admission, scheduling and completion (RequestCoordinator)
|   +-- Shared bounded Access workers and one Mutation worker (ExecutionPool)
|
+-- [Operation handling]
|   +-- [Operation dispatch]
|   |     Routes to the appropriate peer below; no dispatcher class required
|   +-- Access orchestration (AccessOrchestrator)
|   |     Browse / Metadata Query / Directory Listing / Record Loading
|   +-- Mutation orchestration and editing (MutationOrchestrator)
|         Create / Update / Move / Directory Rename / Todo status changes
|         Edit Lock acquisition, release and ownership checks
|         Owns Edit Lock map; selects Repository calls/Coordinator callbacks
|
+-- [Storage]
    +-- Authoritative Normal Entry persistence (NormalRepository)
    +-- Metadata projection and queries (MetadataCatalog)
    +-- Catalog/session consistency coordination (CatalogCoordinator)
    +-- [Persistence support]
          Entry codecs / Index codec / locators / path validation
```

Access and Mutation handling are peers and both use Storage. AccessOrchestrator borrows its Storage dependencies and owns no mutable editing
state; MutationOrchestrator owns editing state.
Edit Lock requests are editing-control operations supporting mutation workflows,
rather than authoritative data mutations or ordinary read-only Access.

BackendContext owns the resource instances; BackendStateMachine enforces
lifecycle transitions. BackendContext owns IpcServer and RequestCoordinator as peers. Operation handlers
borrow Catalog, Repository and Coordinator through C++ references. Coordinator
borrows Catalog. Repository retains persistence sequencing and reports results
through matching Coordinator callbacks; Coordinator synchronizes Catalog.
ConnectionSession is Backend-private and is retained by workers/accepted work,
while Edit Locks reference it weakly. No additional controller or lifecycle
framework is implied.

### Backend module responsibilities

Application operation categories and execution resources are different dimensions.
Access versus Mutation describes business effects, not a required worker hierarchy.
Edit Lock acquisition/release is editing control. Worker allocation and conflict
scheduling are execution policy; they do not define new domain operation types.

| Directory under `server/include/` and `server/src/` | Responsibility and placement rule |
| --- | --- |
| `runtime/` | Backend lifecycle, resource assembly and lifetime, Repo ownership and safe finalization; not a container for all threaded code |
| `ipc/` | Connections, protocol handling, admission acknowledgments and response delivery |
| `execution/` | RequestCoordinator, scheduling eligibility, execution capacity and worker/completion coordination; one shared queue, short coordination and bounded execution workers |
| `operations/` | Application business rules and orchestration through AccessOrchestrator, MutationOrchestrator and CatalogMaintenance: prerequisites, lower-level call sequencing and public result interpretation |
| `storage/` | Authoritative persistence, Catalog projections and Coordinator consistency mechanisms |

Put a future operation helper under `operations/` when it implements Application
semantics by coordinating lower-level components. Delete or Trash orchestration could fit
there once separately authorized. One operation does not
require one class/file. Worker pools, queue accounting, codecs, filesystem
primitives and Core models have other owners. Repository still owns persistence
sequencing and invokes Coordinator callbacks; operation orchestration must not
duplicate that synchronization path.

```text
runtime/       Assembles components; owns startup, draining and finalization
    |
    +-- ipc/ --> execution/ --> operations/ --> storage/
                 When/where     What rules       Persistence and
                 work runs      work follows     Catalog mechanisms
```

The arrows summarize responsibilities, not mandatory class dependencies. IpcServer
borrows RequestCoordinator; BackendContext owns both runtime services.
The [combined Steps 5–6 plan](../development/plans/catalog-concurrency.md#execution-modules-and-combined-steps-56)
separates short coordination from costly execution. Concurrent worker groups are implemented; business categories and execution
resources remain separate dimensions.

## Ownership and boundaries

```text
CLI Frontend / TUI Frontend (many)
            ↕ public requests, responses, events over local IPC
Backend (one active owner for this user's configuration)
    ├── MetadataCatalog
    ├── CatalogCoordinator (borrows that Catalog)
    ├── NormalRepository / future VaultRepository
    ├── shared request queue / concurrent Access / sequential Mutations
    └── connection-owned Entry Edit Lock registry
```

Backend owns operations, lifecycle, metadata, event publication, and later Recovery/Trash/Vault/Git integration. Frontends own navigation, tabs, focus, and editing buffers. They do not own the authoritative Catalog or invoke private Storage APIs.

Formal Application APIs begin here; private Storage classes/errors/callbacks/handles must not simply become public. Public DTOs, request/error semantics, protocol versioning, and compatibility require explicit design. Domain APIs remain independent of presentation. Do not introduce an unnecessary Repository hierarchy or expose private Catalog types.

## Runtime-role module structure — Implemented

```text
app/                         Frontend-only code
  include/frontend/          Backend launcher
    runtime/                 BackendConnectionHandle, ConnectionSession
  include/tui/               TUI presentation and interaction
  src/                       Frontend implementations
  src/main.cpp               Shared dispatcher (explicit ownership exception)
server/                      Backend-only code
  include/                   Role-private headers; matching responsibility dirs
  src/                       Backend entry point and implementations
  src/runtime/               Lifecycle, state machine and ownership implementations
  src/ipc/                   Backend connection management
  src/execution/             Request scheduling and execution coordination
  src/operations/            Access and Mutation business orchestration
  src/storage/               Storage implementations (headers in include/storage)
lib/                         Shared code; no dependency on either runtime role
  include/folio/             Core, Config, Logging, I/O and shared contracts
  src/                       Implementations and private helpers/test seams
```

`folio -> {folio_frontend, folio_backend} -> folio_shared`. There is still one
executable and separate Frontend/Backend processes at runtime. Presentation never
calls Storage directly. Frontend reuse across TUI, CLI or GUI does not make a
component shared between runtime roles: connection/launch code stays in app/.

Each module owns its headers. include/ separates declarations from sources; it does not itself promise an exposed API. The folio/ include prefix is reserved for lib/include/folio; app/include and server/include have no folio/ wrapper. Only lib/include is shared between roles; app/include and server/include are private to their owners, with explicit dispatcher/test access. Shared codec/transport headers under
`lib/include/folio/ipc/` are implementation-facing cross-module
interfaces, not a second business API. I/O filesystem/path interfaces are shared
through `lib/include/folio/io/`; atomic-write internals and test hooks stay private.
Wire value types no longer require a Backend handler header. Process error values
are shared; Frontend launching and Backend execution have separate headers.
No production target receives another module's private source path. Internal
tests explicitly receive the private paths they exercise. CMake visibility is a
build boundary, not language-level access control.

The earlier Phase 4.1a.5 physical layout is superseded. This role-based refactor
preserves the runtime responsibilities described in the [IPC contract](../contracts/ipc.md).

## Catalog-centric flow

```text
MutationOrchestrator: preconditions + current authoritative Snapshot
  → NormalRepository: authoritative persistence
    → matching CatalogCoordinator callback
      → MetadataCatalog: metadata + locator + reverse membership

AccessOrchestrator → Catalog metadata / targeted authoritative content reads
Lifecycle owner → CatalogCoordinator → Index save and eligible Marker cleanup
```

The [Storage contract](../contracts/storage.md#catalog-centric-flows--implemented-internal-capabilities)
defines callback ordering, consistency state and persistence guarantees.

## Runtime and future boundaries

See the [lifecycle call flow](../contracts/backend-lifecycle.md#backend-lifecycle-call-flow--implemented)
for thread creation, connection workers, executor draining, reporting and finalization.
The [IPC contract](../contracts/ipc.md#scheduling-and-lifecycle) defines admission and
execution; the [mutation contract](../contracts/mutations.md) defines field ownership,
Edit Locks and evidence. [Current status](../development/status.md) distinguishes
implemented capabilities from the [roadmap](../development/roadmap.md).

## Paired IPC session — implemented

```text
Frontend session                         Backend session
  Request caller / future UI worker        Request I/O worker
          |                                       |
          +----------- Request connection --------+
          |                                       +--> RequestCoordinator
          |                                              | Access pool
          |                                              | Sequential Mutation worker
  Push receiver                            Push I/O worker
          +------------- Push / ACK -------------+
          |                                       ^
  Retained invalidations                    Bounded recipient queue
          |                                       ^
  Future serialized UI consumer             Catalog / Edit Lock / lifecycle changes
```

Both connections share one lifetime and editing identity. Push I/O and receipt ACKs
remain independent of Request transfers and UI processing. Request-time events are
retained until result delivery; UI integration must apply the result before retained
invalidations. Report attempts sanitized fatal availability delivery with a bounded
window; Finish terminates sessions before Catalog finalization. The
[IPC contract](../contracts/ipc.md#push-notifications-and-session-ownership) owns
pairing, payload, bounds, ordering and failure details.

### Runtime ownership and handoff

```text
BackendContext (lifecycle resource owner)
  |-- FatalErrorReporter                 retained causes + lifecycle wake signal
  |-- RequestCoordinator                 coordination thread + execution pools
  |     |-- PrepareOperation callback --> operation orchestrators / maintenance
  |     `-- fatal-report callback ------> FatalErrorReporter
  `-- IpcServer                          Request / Push connection workers
        |-- borrowed RequestCoordinator -> admit_operation() / ticket result
        `-- borrowed FatalErrorReporter -> fatal report / wait wakeup
```

Serve constructs RequestCoordinator before IpcServer; the coordinator starts its
own threads. IpcServer has no business Handler or Preparer dependency. It supplies
response encoding while the coordinator owns operation preparation/dispatch.
Stopping/Report close IPC admission, drain the coordinator, then wait for final
response delivery. Report resolves Push delivery before Finish closes IPC workers.
The coordinator is drained even if IPC initialization fails; Storage finalization
follows execution termination. Destruction order keeps borrowed services alive.

## Frontend and TUI organization

**Implemented baseline and approved extension boundaries:** reusable Frontend
facilities are those a completely different GUI would actually reuse, principally
Backend communication, Application exchanges, Push receipt and transport handling.
They remain in `app/`, never shared runtime `lib/`. TUI navigation, history,
module selection, cached display state, logical cursor/focus, keyboard commands,
sorting presentation and responsive layout belong under `app/tui/` (in practice,
matching `app/include/tui/` and `app/src/tui/`), even without FTXUI calls.

| Category | Responsibility |
| --- | --- |
| `logic/` | Substantial TUI-specific state and behavior, independent of rendering: Browser query selection, relevance, invalidation and deferred reload |
| `shell/` | Application-wide composition: active page, persistent Bottom Bar, global Quit handling and future overlays; no Record-specific rules |
| `pages/` | Major navigation contexts and their local state/controls: Browser and Record; Settings is a future Browser Section |
| `layouts/` | Reusable composition/arrangement rules extracted when independently useful; no navigation ownership implied |
| `components/` | Reusable localized UI elements; elements used only by one page stay beside that page |

```text
TuiShell
  Active Page: BrowserPage
    Browser-specific TopBar
    EntriesDisplay
    LoadingStatus (reusable TUI component)
  BottomBar
  Dialog Overlay (future; no placeholder implementation)
```

Code organization does not define thread organization. `logic/` runs on the TUI
Event Loop thread; moving a controller introduces no worker, event loop, service
or queue. Presentation depends on TUI logic, which uses the shared Frontend runtime
for Backend Application requests. Components need not pass through every layer.
The communication runtime remains independent of FTXUI and UI state.

`BrowserQueryController` owns derived query/cache identity, cached results, relevance
generations and deferred reload intentions. Navigation Manager owns the active Section. It lives under `tui/logic/browser/`.
Navigation, Browser and future Editor/Draft state belong to the TUI, with one
owner rather than copies synchronized with the communication runtime. Push
relevance and reload decisions are TUI behavior, not transport responsibilities.
Finish outstanding requests while controller completion targets remain alive.

Organize by responsibility, not inheritance alone: substantial nonvisual TUI
behavior belongs in `logic/`; small tightly coupled state stays with its visual
owner. Page composition, Shell, layouts and presentation components retain their
existing responsibilities. LoadingStatus remains a visual timing/presentation
component. This separation serves current maintainability, not a speculative GUI
framework; it adds no MVC/MVVM infrastructure, event bus or dependencies.

Browser is one **Page Type** with Logs/Memos/Todos module contexts. A **Page
Instance** is a concrete page and its state, while a **context** identifies the
module or Record. Record viewing/editing are modes of one Record Page Type.
These distinctions do not impose inheritance or identical layouts on variants.
The Browser header switches modules; the future Record header provides Back,
Title and Metadata Drawer control. They need no universal Top Bar abstraction.

Layouts are introduced selectively: an `hbox` or `vbox` does not justify another
class/file. The source tree follows responsibility and locality, not a one-to-one
mapping to the FTXUI component tree. There are currently no extracted reusable
Layouts, so `layouts/` has no empty directory. Existing target boundaries remain
unchanged; no shared UI framework or new build target is introduced.

### Navigation — Phase 1 concrete Browser integration

The navigation subsystem lives in `folio::tui::navigation`. Callers qualify its
types (`navigation::Anchor`, `navigation::Manager`, `navigation::Description`,
`navigation::Context`, `navigation::Result`) rather than importing the whole
namespace. Concrete Pages and components remain in `folio::tui`.

The navigation vocabulary distinguishes identity, position and action:

- **Anchor:** an owner-interpreted, stable reference to one path step, optionally
  carrying metadata/factory access. It is not a live tree-node instance. Identity
  comparison ignores construction capabilities.
- **Location:** a complete vector of Anchors describing the encountered structural
  path. It remains meaningful after temporary UI objects are destroyed.
- **Target:** an owner-declared transition from a source Location to a destination
  Location. Structural targets request Down; Cross targets also carry their
  declared Push/Switch mode. An Anchor alone is not a navigation request.
- **Description:** class-owned relationship metadata, not a navigable object.
- **Context:** a borrowed Location and owner-prefix depth for one discovery or
  transition call, not another stored Current Location.
- **INavigable / NavigableComponent:** the live relationship/hosting contract and
  its shared FTXUI integration, respectively.
- **Manager:** the coordinator outside the hierarchy, owning Current Location,
  History and transition commitment rather than owning UI components.

These navigation Anchors are unrelated to Storage's filesystem locators. The
permanent History anchor is a complete initial Location, not a single Anchor.

A Cross declaration pairs a destination with its `CrossMode::Push` or
`CrossMode::Switch`. The originating UI passes that complete action to
`Manager::cross(const Target&)`; it does not select or supply the mode again.
Two declared actions may share a destination while using different modes.
The Manager applies the declared mode's History behavior. Structural Down remains
separate through `Manager::down(const Target&)`.

**Implemented:** `navigation::INavigable` separates discovery, lookup and hosting:

```cpp
std::vector<navigation::Target> children(const navigation::Context&) const;
std::vector<navigation::Target> cross_targets(const navigation::Context&) const;
std::shared_ptr<navigation::INavigable>
resolve_child(const navigation::Anchor&, const navigation::Context&) const;
void host(const std::shared_ptr<navigation::INavigable>& destination,
          const navigation::Context&);
```

Discovery and lookup remain pure virtual; hosting defaults to unsupported for
objects that do not host UI. Visual classes inherit `navigation::NavigableComponent`,
which combines FTXUI implementation and this interface once. Its constructor
copies class-owned `navigation::Description` data and binds session factories for
its declared children. Only live navigable objects implement the interface. Each description
contains its own anchor, visitability, child-description getters and an optional
destination factory. There is no separate child-description or child-activation type.
Shell, Browser Page, Top Bar and the actual browser components expose navigation. The concrete navigation hierarchy is:

```text
TuiShell
  BrowserPage
    TopBar
      LogsBrowser
      MemosBrowser
      TodosBrowser
```

Each type-specific browser owns its browsing presentation and inherits common
cursor/frame behavior from `RecordBrowser`. Lists are their current presentation,
not their identity. The FTXUI composition still arranges Top Bar above Entries
Display; the navigation hierarchy need not mirror that layout component tree.
Navigation declarations live under `app/include/tui/logic/navigation/`:
`navigation.hpp` groups semantic anchors, static description data, paths, targets, transition results and
`navigation::INavigable` as the common navigation contract. `navigation_manager.hpp` declares
transition/history coordination. `navigable_component.hpp` groups the shared
FTXUI adapter with metaprogramming that generates the class-owned descriptions. The adapter remains
FTXUI-dependent; placing it here groups navigation integration without making all
TUI Logic dependent on presentation. The matching Manager implementation is
`app/src/tui/logic/navigation/navigation_manager.cpp`.

Owning headers declare `using NavigationChildren = navigation::Children<A, B, C>` using the
actual child class names. Class types identify static children, so there are no
additional child enums or `NavigationChild` declarations. Visitability belongs to
the destination: it defaults to true; Browser Page declares
`navigation_visitable = false` because only its descendants are visitable.
`navigation::Children<...>` is a compile-time type list; `std::vector<navigation::Target>` is the runtime list
returned by discovery. `navigation::NavigableComponent::describe<UI>()` generates relationship
data from the class-owned declarations. These declarations do not implicitly
declare Cross targets. The class-specific `navigation::description<UI>()` accessor
provides process-lifetime immutable data without constructing UI.
Forward declarations allow the concrete browsers to remain source-private.
Descriptions contain no live component pointers, captured UI state or mutable
current position. Future dynamic subtree capabilities use the same `navigation::INavigable`
operations with the supplied logical position; they are not implemented yet.

Requests use declaration-issued `navigation::Target` values rather than arbitrary
Anchors. The protected `target<Owner, Child>(context)` helper requires Child to
belong to Owner's declared `NavigationChildren` at compile time. For example,
Top Bar's `target(BrowserKind, Context)` dispatches to that constrained helper.
Dynamic implementations declare runtime relationships with the protected
`INavigable::target()` helpers, issuing the same Target type, optionally supplying
a destination factory. Cross declarations issue a Target containing an absolute
destination and Push/Switch mode. These helpers prevent accidental undeclared use;
they are not a security boundary against deliberately incorrect custom classes.
Manager trusts issued relationships and never enumerates children or Cross targets
to authorize a transition. A stale source context or wrong operation mode is a
programming error. Target construction is private; `down(Anchor)` is not an API.

Manager retains weak object references alongside the durable Location path.
During transitions it locks and retains shared references, including every child
returned by lookup, so releasing an owner's reference cannot destroy an object
being used. Only the stable Shell is borrowed by reference. Down resolves the current
object's child. Up uses the saved ancestor context. Cross and History restoration
reuse a live common prefix and prepare only the missing destination suffix.
Registered Anchors carry direct metadata access, so neither Down nor restoration
searches child-description lists for a factory. Plain `Anchor::of<T>()` identifies
a type; `Anchor::registered<T>()` also attaches its class-owned metadata accessor.
Dynamic positions remain subject to live lookup and may carry an owner-supplied
factory instead of static metadata. Dynamic implementations may omit
metadata or override the common interface's discovery/lookup behavior using
`navigation::Context`; actual Directory virtual subtrees remain deferred.
Top Bar requests Down directly from the Manager; Browser Page receives the
post-commit change notification for query and presentation updates.
Destination construction is bound by UI-owned registration code using
`navigation::Factory = std::function<std::shared_ptr<INavigable>()>`.
`navigation::NavigableComponent::describe<UI>()` generates static relationships;
`bind_child<Owner, Child>(factory)` supplies a session-bound factory only for a
declared child. Issued targets retain that callable in their destination anchor.
Factories capture their explicit session-lived services and settings, never a
temporary parent UI. Navigation interfaces and Manager know none of those types
or values: there is no common UI dependency bag or service registry. Static
relationships remain class-owned and are not redefined by the bindings.
Browser Page owns its separate composition factory, which can adopt an already
prepared destination. Shell retains that UI-specific callable for remounting;
it is not passed into Manager or general navigation interfaces.

For a registered missing destination, Manager invokes its factory directly before
asking for missing visible ancestors. An already-hosted immediate child is reused.
The Manager passes the prepared destination explicitly to `host()`, which retains
that exact instance. `navigation::Context` contains only a borrowed `const Location&`
and the owner's prefix depth; it carries no construction or activation payload. The Shell constructs Browser Page using its
own registered factory only when that visible composition is missing; Browser
Page can adopt an independently prepared Top Bar, and Entries Display can adopt an
independently prepared record browser. Hosts retain ownership, while the Manager
retains only weak resolved references after commit. Factories returning null or a
destination with mismatched metadata violate the programming contract. Exceptions
propagate before Current/History commit.
The separate Shell-specific Page factory callback has been removed.

The three record browsers are now lazy rather than eagerly constructed together.
Displaying a child while focus remains at Top Bar uses the same registered
factory. Construction itself submits no Backend request. Only ancestors needed for
visible composition are constructed; this is separate from resolving a hierarchy
through static data. No global factory registry, general dependency-injection
framework, or additional lifecycle service is introduced. Dynamic locations still
resolve through the same `navigation::INavigable` operations when static factories
do not describe them; Directory virtual subtrees are not implemented here.

`navigation::Anchor` holds an owner-defined, copyable equality-comparable value through
`std::any` and a typed equality function. `navigation::Anchor::of<UI>()` creates a
static class identity without constructing UI; `is<UI>()` tests it. It also accepts
owner-defined by-value positions for future dynamic subtrees. `BrowserKind`
remains presentation/query context, translated at the Browser boundary; it is
not the navigation identity. Navigation identity contains
no `RecordType`, strings, globally assigned node IDs, global node-type variant or
persistent UI pointers. The Browser translates its Section to `RecordType` only
when requesting the existing Backend query. `Location` is a vector of anchors,
owned by Navigation Manager along with History. No global owner registry is needed:
Shell interprets the first anchor, Browser Page the next, and Top Bar the third. These actual
owners define the relationships, not another hierarchy tree in the Manager.
`std::any` may allocate; there is no allocation-free path guarantee.

`children()` is discovery. `resolve_child()` looks up one known child without
construction, mounting, selection changes, or enumeration of dynamic collections.
It returns a retained shared pointer, or an empty pointer when no instance exists.
A missing instance can still have a valid registered destination factory. The
Manager uses that factory directly; lookup never calls constructors. One object
may resolve multiple logical positions, with `visitable(Context)` deciding whether
each position can be visited. Directory virtual subtrees themselves remain deferred.

The transition pipeline is explicit: read-only resolution; direct destination
construction if needed; History/storage allocation; hosting; Current Location and
History commit; post-commit notification. `host()` composes missing visible UI and
retains the prepared destination, without choosing a new Current Location. Success
must leave the full destination path resolvable to that exact object. Lookup uses
pre-reserved storage afterward; no deferred activation callbacks remain. A broken
host contract raises a programming error before navigation-state commit. Resolver,
factory and host exceptions preserve Current Location and History. Normal navigation
results are only `Changed` and `Unchanged`; there is no Invalid/Unavailable status.
Factory exceptions
propagate before hosting. Host implementations must leave current presentation
intact on failure. Current Browser hosts add only missing composition/children;
replacement of other Page types remains outside this phase.

After commitment, the Shell notifies the active Browser Page to update its query,
feedback and cursor context. Backend queries remain asynchronous. A query failure
is an operation outcome, not evidence that the navigation destination is invalid.

The Shell either hosts Browser or obtains it from Browser Page's registered factory. Browser Page is structural but not
visitable; Top Bar and its three browsers are visitable. Each browser represents
its semantic Section Root. Active content reads the committed Location. The Manager's
change callback drives query/feedback updates and restores saved entry cursor
state. Top Bar requests structural Down to its browser child; switching from an
already-current browser ascends to Top Bar with Up before Down to its sibling.
Moving the Logical Cursor does not activate a browser. Enter activates the requested browser but keeps keyboard
focus on Top Bar. Down (or Tab) enters the displayed browser. Mouse activation
preserves the Logical Cursor. Active destination and keyboard focus remain
separate: after tab activation Current may identify the activated browser while
focus stays on its Top Bar ancestor.

Cross targets use the same `navigation::Target` as structural children, with an
absolute destination and a Push/Switch mode. They describe jumps outside structural Up/Down traversal. Top Bar has
no Cross targets: its browsers are structural children. Browser Page exposes only
Top Bar, not a second list of browsers. Manager trusts targets issued by these owners;
it does not independently define or rediscover these relationships. Current Cross discovery is
small and concrete; virtual-location positions and large dynamic target sets
are not designed or implemented here.

Only Shell is assumed stable. Missing path suffixes are resolved through actual owners; Manager does not retain child pointers in Current, History or
restoration state. Shell's change callback accesses its currently owned Page,
not a captured permanent Browser pointer. Page and browser ownership stays with the
existing TUI objects; Top Bar retains the Entries Display that hosts its browsers.
Production starts without a Page; the initial target is constructed directly and
then adopted into the composition hosted by Shell. Other Page types and their
unmounting policies remain deferred.

Browser Page factories bind child callbacks after shared construction. The callbacks
hold weak Page references, lock them only during invocation, and expose no active
browser context after Page destruction. Retained child components therefore cannot
call a destroyed Page; Page teardown also unbinds Top Bar's borrowed Manager.
BrowserQueryController is shared-owned and asynchronous Query completions hold only
weak references to it. Destruction abandons UI delivery, not the Backend request;
live completions still apply generation checks on the TUI thread. Backend and theme
services remain session-lived dependencies and must outlive components borrowing them.
Manager restoration reads return an owned `std::any` snapshot (one payload copy),
not a pointer into its growable position storage. Change callbacks borrow this local
snapshot, so saving another position cannot invalidate it.

History is a `std::deque<Location>` and an index cursor, separate from each vector-based
structural path. After every committed transition, `history[cursor] == current_location`.
History Start is a separate immutable fallback declaration, currently Browser / Top Bar /
Logs Root; it is not a protected slot in the mutable History sequence. The application
declares its semantic path as `static const`; session factories are attached to a copy,
never stored in process-wide data. Existing `std::any`/`std::function` path values are
not constexpr-compatible. The Manager retains that bound copy as const.

| Operation | History side effect after successful activation |
|---|---|
| Down / Up | Replace current slot; discard Forward; do not append |
| Cross Push | Preserve actual source in current slot; discard Forward; append destination and advance cursor |
| Cross Switch | Replace current slot; discard Forward; no new return level |
| History Back / Forward | Restore adjacent saved location and move cursor; preserve Forward |
| Semantic focus movement | Update restoration snapshot only; preserve History and Forward |
| Rejected/failed transition | No History or Current Location commit |

An unchanged Cross Switch also discards Forward. An Up with no visitable parent is a
no-op. Browser content → Top Bar is structural Up (including Escape/Tab), not a
focus-only change; ordinary Top Bar cursor movement remains focus-only.
Contextual Back inherits the policy of the actual operation it performs, not a
global History Back policy. Section switching creates no artificial Back step.
Contextual Back is exclusively the user's Escape behavior, not a navigation
capability registered or invoked by UI components. Existing TUI event routing
handles the innermost interaction first; a consumed Escape must not propagate.
Shell receives Escape and calls `Manager::contextual_back()`. Manager centrally
selects and executes the contextual action; Pages do not independently dispatch
Escape into navigation operations.
Buttons and mouse actions request their explicit operations independently; they
do not invoke or configure Contextual Back. Its explicit Manager entry point is
reserved for Shell's user-Escape handling, not component navigation controls.
Deferred Escape priority is: active Dialog/overlay consumes the key first; otherwise
an Editor requests safe exit to Viewer (confirming unsaved Drafts); otherwise a
Record Viewer returns through its Cross Push History; otherwise Browser content
performs structural Up, including Root to Top Bar under the current design;
otherwise Top Bar consumes Escape as a no-op. Dialog/Editor/Viewer handling awaits
those components; it is not implemented by hypothetical navigation scaffolding.
History Start remains available independently even when slot zero is replaced.

Deque supports indexed traversal and end growth without relocating all entries;
future front retention is possible. It is a pragmatic choice, not a claim of universal
speed superiority or constant-time middle erasure. Structural paths remain vectors.
History is prepared before hosting and committed only after hosting succeeds;
hosts must preserve presentation if they throw. No durable History identity owns UI
pointers. Confirmed-invalid destination skipping and automatic History Start recovery
remain deferred until dynamic destinations have an explicit validity contract.
Back/Forward currently attempt the adjacent entry; exceptions preserve History and
must not be interpreted as confirmed deletion or swallowed as fallback triggers.
No History keyboard shortcuts or controls are added.

Structural Up uses saved visitability of the path's ancestors, skipping
non-visitable containers, and reuses the live resolved ancestor when available.
It needs neither reverse parent links nor a global parent registry. Escape from
browser content performs actual Up to Top Bar and moves focus there. At Top Bar,
Escape does nothing; it never implicitly invokes History Back. Up on the first
entry and Tab also synchronize the transition back to Top Bar. Returning from
browser content focuses that browser's corresponding tab, rather than restoring
an unrelated previously focused tab. The browser's entry position is retained
independently; mouse activation still does not move the Logical Cursor.

Manager currently retains provisional per-Location entry cursor snapshots (UUID plus fallback index).
The Top Bar location also retains its displayed `BrowserKind` in Manager-owned
restoration context, so Up preserves the content instead of defaulting to Logs.
Returning Down restores the entry cursor without issuing another query. Startup
preserves Top Bar focus and starts Current and its History slot at Top Bar, while
History Start remains the full Browser / Top Bar / Logs Root path. Ordinary focus movement
adds no Cross History level. No independent active-browser field is kept in Page,
Top Bar or the communication runtime. FTXUI retains low-level focus/event routing.
On returning to a browser, its cursor reconciles against query results, using the
nearest remaining index if the UUID vanished.
**Deferred Implementation — TODO(Navigation):** With virtual-subtree navigation,
replace the parallel `BrowserPosition` / `std::any` navigation snapshots with
Locations representing scope and targeted entry. Manager should remember each
browser's last Location; browser-local state should retain presentation details
such as scroll offset, not a competing navigation position. Do not expand the
provisional snapshot mechanism before that replacement.
The query controller retains generation checks, cached results, deferred reloads,
Push handoff and the one-request-slot behavior. Queries remain non-blocking.

**Current limitation:** a Section represents semantic Root navigation, but its
existing query still uses `AllScope`, which includes all Records of that type.
This is not Directory-aware Root browsing. Directory Browse, Browser virtual positions,
Record Pages, Settings destinations and Breadcrumb/History presentation are Phase
2 or later work. No synthetic Root node or extra Root anchor is added.

**Implemented foundation — composed virtual subtrees:**
`navigation::VirtualSubtree<Position>` is a nonvisual member capability, not an
FTXUI base, Provider interface, separate tree, or second Navigation Manager. It
implements `INavigable`; static components continue using `NavigableComponent`.
Owners supply relationship callbacks and optional local visitability rules.
`child(position, context)` issues a known structural target without enumeration;
`cross(destination, mode, context)` issues a source-declared Cross action with its
Push/Switch policy already attached. These helpers declare actions; they do not
perform transitions or consult a global registry.

An owner-specific, copyable/equality-comparable `Position` value identifies a
virtual step in the existing `vector<Anchor>` Location. `position(context)` reads
the step at the supplied prefix depth. There is no mutable current-node field.
Different positions can resolve to the same capability without constructing a
component for each position. `INavigable::represents()` separates semantic position
recognition from static class registration during destination reconstruction.

The owner publishes its member with `VirtualSubtree::retained(owner, &Owner::member)`.
The returned aliasing `shared_ptr<INavigable>` owns the UI's control block, so a
retained capability keeps its containing UI alive; its internal self-reference is
weak and cannot form an ownership cycle. Reconstruction factories return this
alias on a newly created owner, capturing session services rather than the old UI.
Hosts remain responsible for mounting that owner/capability in presentation.
Manager pins transition references and sees only the common interface, including
when several path levels refer to the same implementation object.

This increment supplies and tests the reusable capability only. Existing browser
queries, cursor snapshots and visible navigation remain unchanged. Connecting
Browser scope/entry Positions, replacing provisional snapshots, Directory-aware
Browse, and future Record Page/reference jumps remain separate integration work.
No synchronous Backend validation, virtual-node registry, new worker, or event
loop is introduced. Discovery callbacks may enumerate available children only
when requested; direct issuance/resolution does not need that enumeration.

**Deferred Implementation — TODO(Navigation), NAV-ANCHOR:** Revisit the permanent
Browser/Logs/Root History anchor when Main Page is designed; evaluate Main as the
permanent initial location. The matching code TODO is at Manager initialization.

**Deferred Implementation — TODO(Navigation), NAV-PREFIX:** If paths become
substantially deeper or vector copying becomes a measured cost, evaluate
persistent/shared-prefix Location Paths. Use vectors now; the matching code TODO
is beside `Location` in the Navigation header.

Responsive layout is a presentation concern. The Shell observes available terminal
dimensions and owns the global minimum-size fallback; it does not define Page
columns or their priorities. Each Page specifies its content, mandatory elements,
minimum widths and visibility priorities. A Layout may provide reusable measuring,
allocation, hiding and truncation mechanisms without knowing the Page's domain.
Visual order and retention priority are independent. Implement these mechanisms
incrementally as Pages need them, rather than introducing a breakpoint framework.

The initial Shell minimum is 80 columns by 24 rows. Below that size in either
dimension a temporary message is centered within the Page Host. The Shell and its
Bottom Bar remain visible, including Quit guidance. This is a presentation fallback,
not a navigable Page; the existing Page,
cursor and asynchronous operations remain alive. Resizing does not navigate or
change application data. Individual Pages still need their own responsive policies.

The Shell provides a one-character horizontal inset for Page content and Bottom
Bar text, plus a blank row at the top and bottom. The Bottom Bar highlight spans
the terminal width. Pages may add spacing for their own layouts.

See the [Application plan](../development/plans/application.md#tui-ownership--implemented)
for current files, feedback ownership and remaining increments. “Bottom Bar” is
the region's project term; status feedback is one function, and command input is
only a deferred possibility.

TUI visual elements use `ftxui::ComponentBase` with `OnRender()` and child
Component composition by default, including noninteractive displays. Exceptions
need a concrete reason. Nonvisual request and timing helpers do not require
component inheritance merely because they live under `tui/`.

### Catalog coordination and maintenance boundary

There is one Backend-private `CatalogMaintenance` module. It owns Check/Refresh
handling, scope resolution and reconciliation, full candidate construction, Index
reading/writing and durable Session Marker I/O. Startup preparation inspects the
previous Marker and chooses Index loading or authoritative reconstruction, returning
an isolated candidate and initialization diagnostics. Its per-scope Check history and
Refresh observer belong to that same instance.

`CatalogCoordinator` owns that maintenance instance and coordinates complex
`initialize()` sequences maintenance preparation, session establishment and safe
publication. It also coordinates full Refresh and finalization, synchronizes active publication
and readiness, and supplies Repository mutation-result callbacks. It performs no
filesystem or codec work. Independent/reserved Check and scoped Refresh operations
use the maintenance instance directly, passing Coordinator explicitly when they
need a coherent Catalog view or managed publication. Maintenance retains no
Coordinator reference; the headers use a forward declaration rather than a second
adapter/module or a callback framework.

Coordinator is not a mandatory gateway or forwarding API for every Catalog method.
Direct operations remain available when their caller does not need to enforce a
complex cross-module contract.

### Initial Frontend Runtime

**Responsiveness rule:** pending Backend work must not delay navigation, input, or
rendering. Render the destination page immediately, with empty Entries while its
query is pending. Ordinary rendering work scales with visible content, not total
data. Worker separation alone is insufficient evidence of responsiveness; main-thread
submission and completion work must also remain small. See the authoritative
[Frontend responsiveness rule and outstanding verification](../development/plans/application.md#frontend-responsiveness--approved-rule).

UI-independent session, connection and launch code lives under matching
`app/include/frontend/` and `app/src/frontend/` directories. Their `runtime/`
subdirectories contain BackendConnectionHandle and ConnectionSession; worker code
remains inside the session implementation. Backend launching stays directly under
`frontend/`. Generic process helpers remain at the app root; presentation stays
under `tui/`.

`app/include/frontend/runtime/backend_connection_handle.hpp` defines UI-independent
`BackendConnectionHandle`, which owns one `ConnectionSession` and exposes all typed
asynchronous request APIs. The session owns both connections and workers. There is
no separate request-sender class or duplicate synchronous Application API.

The TUI calls, for example, `backend.query_records(query, completed)`. The handle
copies arguments and submits the actual blocking IPC operation to the Request
Worker. It retains the result until the main thread processes completion and
invokes `completed(result)`. A false return rejects submission without invoking
the callback. Application result and Mutation evidence contracts are unchanged.

```text
BackendConnectionHandle
└── ConnectionSession (exclusive ownership)
    ├── Request Worker
    │   ├── Request Connection
    │   ├── Single request/completion slot
    │   └── Request thread → blocking IPC operation
    ├── Push Notification Worker
    │   ├── Push Notification Connection
    │   ├── Retained notices and wakeup state
    │   └── Push Notification thread → receive, retain, ACK, wake
    └── Shared lifecycle and Request/Push handoff coordination
```

Application contracts remain under `lib/include/folio/application/`.
`ConnectionSession::connect()` and `connect_or_start_backend()` return an owned
session, transferred to `BackendConnectionHandle`. Callbacks borrowing targets must
keep them alive through delivery or explicitly guard their lifetime. Browser Query
completions use weak controller references and skip delivery after destruction.
Worker scheduling and lifecycle live in the session; the
handle owns typed request construction and IPC result interpretation. There is no
Frontend event-dispatch thread.

```text
Main thread / TUI                 Request Worker          Push Notification Worker
submit one request ------------> blocking IPC             receive + validate
consume completion <------------ store + wake             retain + ACK + wake
consume retained notices <------------------------------- bounded retention
update TUI state / render
```

The Request Worker and Push Notification Worker are sibling session-owned execution
resources. Session declarations and implementation live in
`app/include/frontend/runtime/connection_session.hpp` and its matching source.

A dedicated FTXUI communication Wakeup event consumes explicit completion, Push
Notification and session-change flags. Keyboard, mouse, loading timers and rendering
do not drain communication work. Several flags may be set together. Push work observed
during an outstanding Request remains deferred until the completion event permits
result-before-invalidation processing.

The request slot stays occupied until the main thread consumes its completion.
Completion callbacks run on that thread before retained Push Notifications are
drained. Wakeup only posts an FTXUI event; it carries no Application payload.
The handle owns communication state, while `BrowserQueryController` owns module selection,
result relevance and the latest deferred query intention. All Browser queries use
the handle; BrowserQueryController has no worker or alternative execution path. Integration
tests exercise navigation generations and retained invalidations.
Navigation generations discard obsolete query results.
Invalidations consumed after completion mark the displayed result stale and request
a new query. Record metadata and Catalog notices invalidate the Browser; availability
notices reach shared TUI feedback. Edit Lock notices currently have no Browser action.

Shutdown finishes local request work while completion targets remain alive, then
closes the session. Already Accepted Backend work is never cancelled or replayed.
**Not Guaranteed:** bounded shutdown under hung IPC/OS operations. **Deferred
Implementation:** reconnection, Backend cancellation, editor/background-save UI,
and finer view-specific invalidation. Typed requests include Mutations; their completions must be retained and consumed
even after navigation. No request FIFO or general event bus exists.

The Push Notification Worker stores received notices in preallocated bounded slots.
Consumption swaps in replacement storage under a short mutex; allocation and batch
conversion run on the consuming thread outside that mutex. Wakeup callbacks are
snapshotted with shared ownership and invoked outside session mutexes. The callback
contract permits only a short event-loop signal; no UI processing or backlog traversal.
These are algorithmic bounds, not hard real-time guarantees for the OS scheduler,
allocator, protocol decoding or event-loop implementation.

A submitted Request callback exception closes the session and is rethrown by
`process_completion()` on the main thread. The TUI catches it and reports the ended
connection. No fabricated Mutation outcome or retry results. Completion callback
exceptions likewise close the session and propagate to the caller. Public shutdown
can surface an unconsumed failure; destructors close/join safely without throwing.
