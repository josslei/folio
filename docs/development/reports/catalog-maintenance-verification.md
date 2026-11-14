# Consistency Check and Catalog Refresh verification

**Status:** implementation verification snapshot, 2026-11-10; not a guarantee for
future revisions. The [maintenance contract](../plans/catalog-refresh.md) owns
semantics and deferred decisions.

## Implemented scope

- Check and Refresh share protocol-v1 admission and final responses. Following
  Frontend consolidation, `BackendConnectionHandle` exposes them asynchronously
  with a result callback; blocking IPC executes on the session Request Worker.
- Raw Normal direct-entry counts use Catalog membership counts and fixed layout
  overhead; Browse remains Catalog-only. Check never starts Refresh.
- Scoped scans reconcile only selected membership. Immediate Roots retain
  unscanned Directory members; Recursive Roots replace their full module.
- Candidates validate before synchronized publication. Scoped publication retains
  readiness, including Inconsistent; Full rebuild may restore Ready. Neither
  writes the Index or clears the Session Marker.
- Maintenance runs on the shared Access pool, excludes Mutations, and leaves
  ordinary Access eligible. Coordination does not wait synchronously for a writer.
- Pending/running Full Refresh requests share one execution through uniformly
  associated request tickets. Each request retains its own admission accounting,
  completion and connection delivery. Completed work is not cached.

## Verification

macOS / Darwin arm64, Debug, native Apple Clang:

```sh
cmake --build build --parallel
ctest --test-dir build -C Debug -LE resource_limits --parallel --output-on-failure
python3 scripts/check_docs.py
git diff --check
```

All **435 regular tests passed**. New tests cover raw count match/mismatch/error,
Immediate/Recursive/Directory scope, retained views, malformed sibling isolation,
duplicate UUID rejection, Full-Refresh-required ambiguity, failed publication
rollback, global readiness, Marker/Index safety, concurrent capture/publication,
maintenance reservation and failure release, concurrent Access, Mutation deferral,
Full Refresh sharing/resource admission, codecs and real-process IPC.

The sandbox prevented Backend IPC startup in both a new and an existing process
integration test. Those tests and the full suite were subsequently run outside the
sandbox, where local sockets/child processes functioned. The final full-suite
result above includes those integration tests.

Linux, Windows, ThreadSanitizer, power-loss behavior and the opt-in resource-limit
suite were not verified in this change. Existing Query, Mutation, Edit Lock,
transport and lifecycle regression tests are included in the regular suite.

## Deliberate limits

No Push/UI integration, Recovery, automatic Refresh/replay, general anomaly service,
concurrent Mutations, cancellation, receipt journal or general priority framework
was added. CONS-03 explicitly prioritizes future priority-system design: Refresh
must have highest priority. Check/Refresh use ordinary eligible scheduling for now; no dedicated priority
branch was added. Pending Refresh still defers Mutations as an execution constraint. A request arriving after a previous
Full Refresh completes may cause a redundant scan before future Push reaches it.
