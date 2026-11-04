# Unified IPC protocol — Phase 4.2c

## Implemented boundary

Query and Mutation are categories of Application Operation, not C++ base classes.
Both use the existing per-user IPC connection, version-1 messages, positional
MessagePack encoding, bounded logical messages and sequential exchanges. The public
access point remains `BackendConnectionHandle`. No Mutation execution, public
Mutation submission methods, concrete request/success payloads or Mutation dispatch
are enabled by this phase.

The existing read payload arrays and alternative tags are unchanged. The former
`access_codec` implementation is now `application_codec`, backed by one shared
private `messagepack_codec.hpp`. The shared codec handles Core identifiers,
metadata, Snapshots, logical locations and Application errors alongside MutationId,
AuthoritativeChangeStatus, MutationStepEvidence, MutationEvidence, MutationFailure
and the templated confirmed MutationResponse. The template allows future approved
success types without fixing a speculative business contract today.

Mutation IDs are validated as UUIDs. Confirmed response decoding takes the expected
MutationId and reports Protocol failure on mismatch. A future exchange must
invalidate its connection on that error, just as current reads do for protocol
failures. There is no enabled end-to-end Mutation exchange yet. NoMutationResponse
and SubmissionState are local Frontend observations and have no wire codec.
The common request ID component can be encoded independently; the complete shared
Mutation request envelope remains deferred until its payload alternatives exist.
No opaque payload or fake handler fills that gap.

## Complete messages and bounded transfers

Every request or response is one length-prefixed message, regardless of size:

```text
Body length (4 bytes) | Protocol version (2 bytes) | Kind (2 bytes) | Payload
```

The length and header integers are big-endian. The message body is limited to
**64 MiB**, including the four bytes for version/kind; the maximum MessagePack
payload is therefore 64 MiB minus four bytes. The four-byte length prefix is
outside the body. This is a memory-safety policy, not an OS transfer limit.
There is no separate 64 KiB protocol limit or request/response chunk sequence.
`protocol_version` remains **1**, checked on every message. There is no
negotiation or separate Mutation schema version.

| Kind | Value | Meaning |
|---|---|---|
| Hello / Ready | 1 / 2 | Readiness handshake |
| Ping / Pong | 3 / 4 | Connection health |
| Reject | 5 | Protocol/lifecycle rejection followed by closure |
| Request / Response | 6 / 7 | One complete Application request / response |

`MessageTransport` owns partial-transfer bookkeeping. Its queue/flush/receive
methods progress nonblocking I/O; `exchange_message` uses the same methods to
provide a synchronous Frontend exchange. The Backend services each connection
incrementally, so a slow or large transfer does not block other connections.
A 64 KiB per-poll I/O budget is an implementation scheduling choice, never a wire
boundary. A complete message may require any number of OS reads/writes.

The header is validated before payload accumulation. Buffers grow with received
bytes, not a peer's advertised size. No request reaches a handler until the whole
message has arrived and its payload has been decoded/validated. EOF during a
message reports connection loss; malformed headers and unexpected trailing bytes
report Protocol failure. The Backend enforces three-second inactivity and
thirty-second total reception deadlines. Frontend exchanges have bounded deadlines.
Empty Request payloads are malformed complete Application requests, not partial
transport messages, and retain the existing confirmed ApplicationError behavior.

MessagePack collections are limited to 100,000 items, nesting to 32, and exact
field counts, types, enum values, UUIDs and variant tags are checked. Truncation
and trailing MessagePack content are rejected. Evidence validation is structural;
per-operation semantic combinations await execution design. Publication is never
interpreted as durability.

The former chunk producer/assembler and all chunk/end message kinds are removed.
There is no multiplexing, RequestId, replay or reconnection. The existing maximum
of 64 peers also bounds concurrent buffers; the message limit is not a global
process memory budget. Future incremental editing or streaming, if needed, needs
explicit message semantics rather than assuming OS transfer boundaries are
Application operations.

## Results and errors

Current malformed Query payloads retain their existing confirmed ApplicationError
behavior; malformed headers and invalid message sequencing reject the connection. Valid
Application errors leave a connection usable. Transport errors, malformed responses
and response-type mismatches invalidate the public connection handle.

Future Mutation exchanges must keep confirmed success, confirmed MutationFailure
(including a possible authoritative Unknown), and a locally unconfirmed response
separate. An ID mismatch is not a confirmed Mutation failure. Missing confirmation
must never fabricate NotApplied. A MutationId provides correlation, not execution
proof, safe replay, deduplication or exactly-once execution.

## Deferred decisions

### IPC-01 — OS event-driven waiting (Deferred Implementation; Phase 4 completion gate)

TODO: Replace the Backend's five-millisecond connection polling and the
Frontend exchange's one-millisecond polling with OS event-driven waits for
read/write readiness, disconnection, deadlines, and shutdown wakeups. Sleeping
between polls avoids busy-spinning but still wakes idle processes unnecessarily.
Use appropriate Linux, macOS, and Windows mechanisms; preserve partial-transfer
handling, bounded deadlines, multi-connection fairness, and lifecycle ordering.
The specific native mechanism remains an implementation decision.

This work is deferred from the current protocol simplification, but **must be
implemented and verified before Phase 4 is declared complete**. Tests must cover
idle waiting, readiness/disconnection wakeups, timeout expiry, and shutdown without
depending on recurring short sleeps. This TODO does not authorize implementation now.

The traceable MUT entries in [mutation contracts](application-mutations.md#traceable-follow-up-work)
track concrete payloads/dispatch in 4.3a, concurrency and Recovery, and the versioning
review after Minimum TUI. These are dependencies, not implementation authorization.
