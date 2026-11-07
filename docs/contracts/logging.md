# Process diagnostic logging — Implemented in Phase 4.1d

Logging records internal diagnostics only. It does not decide operation outcomes,
replace typed errors, deliver IPC responses, finalize Catalog sessions, or provide
user-facing CLI/TUI output. It does not resolve the deferred Backend Output API.

## Dependency and ownership

Folio uses pinned spdlog v1.17.0 (`thirdparty/spdlog`, commit
`79524ddd08a4ec981b7fea76afd08ee05f83755d`). The shared `folio_shared`
static target links compiled `spdlog::spdlog`; the dependency propagates to
consumers because shared code uses spdlog directly. All six severity macros are
compiled in, with runtime filtering. Windows uses native wide-character filenames.
No logging server, module-specific logger tree, async queue, or worker is added.

The public initialization API is `<folio/logging/logging.hpp>`, implemented in
`lib/src/logging/logging.cpp`. Each process owns one synchronous default logger.
Backend and Frontend processes have separate registries and session files. Shared
library code uses the executing process's default logger without selecting a role.
Use `SPDLOG_WARN`, `SPDLOG_ERROR`, etc. to retain logging-call source locations.

`initialize(ProcessRole)` resolves `config::application_paths().logs`; it never
loads TOML or requires EffectiveConfig, StoragePaths, IPC, or Backend ownership.
The explicit-root overload supports isolated tests. Call initialization once after
role selection, before role-specific startup. Repeated initialization within an
active logging session returns the existing file. Initialization and shutdown are
entry-thread operations, not concurrent with logging threads.

## Locations and files

| Platform | Log root |
| --- | --- |
| Linux | `$XDG_STATE_HOME/folio/logs`, default `~/.local/state/folio/logs`; relative XDG overrides are ignored |
| macOS | `~/Library/Logs/Folio` |
| Windows | `<LocalAppData>/Folio/Logs`, resolved with Known Folders |

Log roots are platform-derived, not ordinary user-configured storage settings.
State remains separately resolved for Session Marker and Recovery. Resolution
creates nothing; explicit logger initialization creates its role directory.

Files are `<log-root>/backend/YYYYMMDDTHHMMSS.mmmZ-pid<PID>.log` or the equivalent
`frontend/` path. UTC and fixed-width milliseconds allow timestamp sorting. An
exclusive empty-file reservation prevents overwrite; a numeric suffix before
`.log` resolves an identical timestamp/PID. The ordinary spdlog file sink opens
only in append mode. Old files are never rotated, truncated, counted, or deleted.
Unsafe/symlink directory components are rejected by existing I/O primitives.
As elsewhere, concurrent hostile replacement of validated paths is outside the
cooperative filesystem contract. A failed initialization can leave an empty file.

## Configuration and startup

Before configuration loads, severity is Warn. The authoritative template contains:

```toml
[diagnostics]
log_level = "warn"
```

Accepted lowercase values are `trace`, `debug`, `info`, `warn`, `error`, and
`critical`. `EffectiveConfig.log_level` is a Config-owned typed enum, independent
of spdlog. Omitted settings inherit the template in memory. Wrong types, unknown
keys, and unsupported values fail validation. No size/retention settings are added.

Backend Bootstrap applies the effective level after successful loading. Frontend
connection setup does the same after loading configuration. This updates the
existing logger; it does not open another file. Failed loading leaves the initial
Warn level available. The Config parser itself does not initialize or mutate a
logger. No hot reload is supported.

## Records, errors, and privacy

Records use UTC time with milliseconds, severity, role/logger name, source basename,
line, function, and message. Source location is the logging call site, not necessarily
where the original error arose. Ordinary messages are one line. A caller may supply
a complete first-line title followed by indented details for a multiline diagnostic.
No new error-origin fields or serialization framework are required.

Backend failures are logged once when retained, including lifecycle phase,
primary/secondary classification, error category and numeric code, and available
nested Storage/I/O codes. Unexpected exceptions are Critical; expected competing
ownership is Warn. Startup recovery/excluded-object summaries are Warn. Finalization
errors are logged while the process logger still exists. Report continues existing
IPC rejection behavior but no longer prints the same internal errors to stderr.

Frontend configuration, terminal connection/startup/launch failures, and connection
loss are logged at handling boundaries. Expected connection retries are not logged
on every attempt. User-visible TUI errors remain on stderr separately.

Do not log Entry bodies, serialized objects, requests, passwords, keys, Vault
plaintext, or authentication secrets. Lifecycle logging deliberately omits raw error
messages, paths, exception text, and IPC rejection payloads: parser/codec messages
can embed user content. Full typed errors remain in existing result/diagnostic
objects; logging does not rewrite them. Future logging calls need the same review.

## Flush, failures, and cleanup

Error and Critical trigger synchronous spdlog flush. Warnings and lower levels
may buffer. The process entry point explicitly calls `shutdown()` after Backend
Finish or Frontend completion; it flushes/releases the file logger and leaves a
silent default for late calls. No destructor finalizes Catalog or Session Marker.
Flush is not per-message fsync and is not a power-loss durability guarantee.

Initialization failure returns no session file and installs a silent default where
possible. The executable entry point prints a fixed warning to stderr before
role-specific startup when initialization returns no file, then continues normally.
This precedes TUI terminal ownership. A detached Backend may have stderr redirected
to `/dev/null`, so the warning has no guaranteed Frontend delivery. The logging
module itself remains silent. spdlog's error handler silently drops sink failures without console
output, IPC, or UI notification. It never logs itself or prints the original sink
message. Boundary calls guard against logging
exceptions so original errors, cleanup eligibility, Index and Marker behavior, and
exit status remain independent of logging. Resource exhaustion that makes further
execution impossible is not covered by ordinary cleanup guarantees.

Tests inject temporary roots, reserve deterministic timestamps/PIDs for collisions,
and use a failing sink for cleanup tests. A test-only executable compiles the same
CLI entry source with an injected log root, keeping Windows tests away from real
Known Folders without adding a production path override. No test logs to real user
Folio directories. Native runtime verification is currently macOS only.

Deferred: rotation, quotas, retention, automatic deletion, hot reload, structured
error-origin tracking, Logging over IPC, and Backend diagnostic-delivery redesign.
