# Concurrent Access execution — Steps 5–6 verification

**Status:** initial implementation and verification record, 2026-11-10.
The worker allocation described below was superseded by the
[shared Access pool update](#shared-access-pool-follow-up). Current contracts
remain authoritative; these measurements describe one Debug build on macOS.

## Implemented boundary

RequestCoordinator owns one short coordination thread and the shared bounded
pending queue. It prepares and dispatches one Catalog-query worker, bounded
filesystem Load workers, and exactly one Mutation worker. ExecutionPool is private
worker support, not an operation hierarchy. Backend Serve supplies prepared work;
generic internal handlers without preparation remain explicitly serialized.

AccessOrchestrator captures status/view or a current targeted locator without I/O.
CatalogCoordinator::read_state() and managed callbacks share a short publication
mutex. Query evaluation retains immutable views without blocking publication.
MutationOrchestrator synchronizes editing ownership and immediate-operation
protection without holding its mutex across filesystems calls. Repository still
invokes its existing Coordinator callback; no second publication path was added.

Loads reserve their Entry through authoritative access. Conflicting Mutations wait,
later conflicting Loads cannot overtake a waiting writer, and Loads queued behind
a Mutation capture the resulting current locator. Mutations remain FIFO/sequential.
Current Directory Rename does not relocate member files and does not reserve every
member. Future operations require explicit execution/conflict classification.

The factory template adds `[performance] load_workers = 2`, integer range 1..16;
omitted settings inherit it in memory. Query and Mutation worker counts remain one.
Public blocking methods, IPC v1, bounded complete messages, Accepted/final distinction,
request-specific errors and accepted-session lifetime are preserved. Draining joins
all execution groups before safe finalization. Push, caching/forwarding, parallel
Mutations, Refresh, cancellation and Recovery were not implemented.

## Checks executed

- CMake Debug configure and parallel incremental build: passed, no compiler warnings
  in the final build log.
- Full regular native suite: **416/416 passed**, using `--parallel` and excluding
  the `resource_limits` label (final run: 4.96 seconds).
- Opt-in resource suite: **8/8 passed**, using `--parallel 1`.
- Eleven new concurrency tests repeated 25 times each: **275 executions passed**.
- Changed C++ files formatted using repository clang-format; documentation links and
  staged/unstaged whitespace checks passed.

The first sandboxed suite could not exercise native process/socket operations.
Native tests outside that sandbox passed; those sandbox failures are not unresolved
product failures. No Linux/Windows runtime test or ThreadSanitizer run is claimed.
No actual power-loss or arbitrary-hang guarantee is established.

Commands:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DFOLIO_BUILD_RESOURCE_LIMIT_TESTS=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug -LE resource_limits --parallel --output-on-failure
ctest --test-dir build -C Debug -L resource_limits --parallel 1 --output-on-failure
ctest --test-dir build -C Debug -R '^(ConcurrentExecution|ConcurrentOperations)\.' --repeat until-fail:25 --parallel --output-on-failure
python3 scripts/validate.py --checks whitespace docs
```

The default-off resource-test option is restored after verification. Synthetic
fixtures use temporary directories; no real Folio Repository or user logs are used.
No files were staged and no Git history operations were performed.

## Focused evidence

[Concurrent execution tests](../../../tests/application/concurrent_execution_test.cpp)
cover independent progress while a writer is held, thread separation, bounded Load
capacity, writer fairness, preparation/execution failures, old-view stability,
Load/Move/Load locator ordering, immediate ownership protection, publication failure,
actual IPC response routing, repeated capture/publication consistency, and accepted
ownership after simulated disconnect. Existing IPC/lifecycle tests still cover
admission rejection, pipelining, shutdown, lost delivery and response correlation.

[Configuration tests](../../../tests/application/configuration_test.cpp) cover
worker defaults, overrides and invalid settings. Testing caught TOML's permissive
boolean-to-integer conversion; the parser now requires an actual integer node.

## Catalog retention and responsiveness measurement

The opt-in [Catalog resource test](../../../tests/resource_limits/catalog_concurrency_resource_test.cpp)
uses 10,000 metadata Entries, 10,000 retained view handles, 512 updates and 32
retained changed versions while an old-view reader runs. One native Debug run:

| Measurement | Observed |
| --- | --- |
| Capture 10,000 view handles | 765 microseconds |
| RSS before retaining handles | 20,283,392 bytes |
| RSS after retaining handles | 20,742,144 bytes |
| RSS with changed versions/reader work | 33,931,264 bytes |
| RSS after releasing retained handles | 33,914,880 bytes |
| Update/reader interval | 659 milliseconds; four old-view queries completed |

Handle retention added about 0.44 MiB in this run rather than copying 10,000 whole
Catalogs. RSS with versions includes updates, query result copies and allocator
behavior, not isolated structural-sharing overhead. Allocators can retain freed
pages, so immediate RSS reduction is not a reclamation requirement. These are
observations, not performance thresholds or proof for arbitrary Catalog sizes.
Concurrency/resource bounds limit outstanding work but do not impose a hard total
RSS cap. The responsiveness tests prove independent completion while deliberately
held filesystem/mutation work remains unfinished; they are not UI latency benchmarks.

## Remaining scope

Push handoff still needs gap-free change coverage and result/event ordering in 4.4a;
Frontends do not automatically become current merely because a query completed.
UI/IPC worker integration remains 4.4b. Stronger durability, revision rules,
parallel Mutations, cancellation, Batch conflict scope and Recovery retain their
existing deferred/open status. Arbitrary hung operations can still prevent drain;
threads must not be forcibly terminated or Storage finalized underneath them.

## Shared Access pool follow-up

Queries and Loads now share one Access pool, configured by
`performance.access_workers` (default 4, positive representable integer), replacing `load_workers`.
The initial arbitrary maximum of 16 was subsequently removed; worker resource
policy belongs to the user. Configuration tests accept 17 and 1024 without
starting threads; the coordinator test exercises 17 workers and still rejects zero.
The draft old setting is rejected as an unknown setting; no compatibility alias
or persisted Entry/Index format change is introduced. Update an existing draft
configuration that explicitly set the old key before starting this build.

RequestCoordinator reserves capacity and Entry access before dispatch. Access
preparation then runs on that same Access worker: it captures readiness/view or
resolves the current locator through AccessOrchestrator. Only editing control and
Mutation protection remain coordination-thread preparation. Multiple queries can
run concurrently, and any Access worker may execute a query or Load. If the pool
is saturated, accepted Access requests remain queued; queries have no reserved
capacity. Control remains responsive, and Mutations retain their separate worker.

Follow-up verification: native macOS regular suite **418/418 passed**, serialized
resource suite **8/8 passed**, parallel build and documentation/whitespace checks passed. The new
checks cover shared query/Load capacity, queued queries during saturation,
preparation/execution thread identity and simultaneous queries. The earlier
Catalog measurements above remain evidence for unchanged structural sharing,
not a benchmark of the new worker allocation.
