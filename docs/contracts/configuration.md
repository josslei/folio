# Configuration — Implemented in Phase 4.1c

Folio reads a startup snapshot: built-in defaults, optional user overrides, then
validation. Missing user files are materialized from the embedded factory template. Core models never parse
configuration. Configuration loading does not initialize Repositories, create
Vault data, move existing data, or read Entries.

## Factory template and user configuration

The authoritative defaults are [templates/folio_config.toml](../../templates/folio_config.toml).
CMake embeds its TOML in the library; no source-tree or working-directory access
is needed at runtime. `configuration_defaults()` parses that resource and returns
a result rather than duplicating default settings in C++.

The user file is `<application.config>/folio_config.toml`. `FOLIO_CONFIG_FILE`
may select another absolute filename. If absent, loading creates safe parent
directories and atomically creates the exact factory template without replacement.
A concurrent winner's file is loaded; directory-lock contention has bounded retries.
Other failures, including publication followed by failed durability, remain errors.
`ConfigurationError::publication_completed` preserves publication uncertainty.

```toml
schema_version = 1

[general.normal_storage]
use_default_location = true
# path = "/absolute/custom/normal"

[general.vault_storage]
use_default_location = true
# path = "/absolute/custom/vault"

[diagnostics]
log_level = "warn"

[performance]
access_workers = 4
```

Default locations resolve to `<application.data>/normal` and
`<application.data>/vault`.
A custom location requires `use_default_location = false` and an absolute string
`path`. Providing `path` with the flag true is rejected. `diagnostics.log_level` accepts lowercase `trace`, `debug`, `info`, `warn`,
`error`, or `critical`. Its typed effective value is applied after loading,
without replacing the process logger. Missing diagnostics inherit the template.
`performance.access_workers` sets the shared Backend Access pool size
(default 4, positive integer worker count). Catalog queries and authoritative Loads share it;
There is no application-imposed upper cap; the integer must fit the platform worker-count type.
Resource availability determines whether threads can be created at startup.
Mutations remain sequential. Omission uses the template default without write-back.
The earlier draft `performance.load_workers` key is replaced, not aliased; rename
it in any existing draft configuration that explicitly specified it.
Unknown settings, wrong
types, and schema versions other than integer 1 are rejected. Omitted settings
are filled from the template in memory; existing files are not rewritten.
The former flat root keys are rejected, not silently ignored or migrated.
The old `config.toml` filename is not imported or deleted; existing custom roots
must be specified in the new file before starting against those Entries.

`application_paths()` retains the platform directory resolver. Resolution alone
creates nothing. Tests inject temporary application directories.

On Linux and macOS, Config, Data, State and Logs default to
`~/.folio/config`, `~/.folio/data`, `~/.folio/state` and `~/.folio/logs`.
Normal and Vault roots therefore default to `~/.folio/data/normal` and
`~/.folio/data/vault`. Linux XDG Config/Data/State overrides are not used.
Cache remains `$XDG_CACHE_HOME/folio` (fallback `~/.cache/folio`) on Linux
and `~/Library/Caches/Folio` on macOS. Runtime retains secure OS-specific
resolution. The rebuildable Index lives in Cache, not State; lifecycle and
ownership files must not be treated as disposable cache.
Windows paths are unchanged, including `<LocalAppData>/Folio/Logs`.
No automatic migration or discovery of previous default locations is performed.
Existing authoritative data must be configured explicitly or relocated deliberately.

Resolution does not create the log directory or initialize Logging. See [Logging](logging.md). Logs must
remain disjoint from authoritative roots, like the other support directories.

Roots must be nonempty absolute paths without NUL bytes. Existing targets must
be directories. Normal and Vault must not overlap, including existing filesystem
aliases. Data may be a strict ancestor of either root, permitting the defaults;
Data must not equal or lie inside an authoritative root. Config, State, Cache, Logs,
and Runtime must remain disjoint from authoritative roots. Support directories
may share parents or nest where these rules permit.

The file is limited to 64 KiB. Configuration file paths reject symlink components,
dangling symlinks, and non-regular files. Existing parent components must be
directories. As with other startup path validation, this does not defend against
concurrent hostile external path replacement.

`ConfigurationErrorCode` distinguishes `SyntaxError`, `InvalidValue`,
`InvalidPath`, and `IoError`, retaining the configuration filename and available
system error. Missing files are created from validated factory defaults. Malformed existing files, permission failures, unsafe paths, and
oversized files never silently fall back to defaults.

## Startup snapshot and ownership

`load_configuration()` returns a public `folio::config::EffectiveConfig` containing validated
`StoragePaths`. Its overload taking application
directories and a filename supports deterministic injection. There is no mutable
configuration override or Application-owned test hook. Internal lifecycle tests
supply startup paths explicitly through the Backend context/runner, which applies
the same Config validation before ownership acquisition.

Backend Bootstrap loads once, acquires RepositoryOwnership, and initializes its
Repository/Catalog session using those effective paths. The Backend retains the
paths for its lifetime. Frontend connection setup uses the same loader to obtain
endpoint settings; it does not initialize writable Storage. Established
connections do not reparse the file. A newly connecting Frontend cannot change
the active Backend's configuration. Internal paths and IPC parameters remain
platform-derived rather than persistent user options.

Changes require stopping the Backend and starting it again. In particular, keep
authoritative roots and bootstrap path/environment inputs stable while a Backend
is active. Concurrent Frontends must use the same per-user bootstrap environment.
No hot reload, configuration revision protocol, or Preferences editing/writer API
exists; initial missing-file creation is the only automatic write.

Existing files specifying both roots remain valid, with no automatic relocation.
Removing a setting selects template defaults; removing the file recreates the template; it does
not discover or migrate the former roots. Restore the old explicit roots to use
that existing data. Changing roots deliberately selects different authoritative
locations, so complete Backend shutdown before doing so.

No Vault initialization, public CRUD/Query API, or data migration is
implemented by this configuration system.

The shared public interface is `<folio/config/configuration.hpp>`, implemented in
`lib/src/config/configuration.cpp`. It exposes configuration/path values and
errors without private Storage or IPC headers. Explicit-path connection settings
are shared in `lib/` through `folio/ipc/ipc.hpp`; the convenience overload
that loads configuration belongs to app/'s BackendConnectionHandle component. The file-local
`backend_endpoint_path(application_paths)` derives the endpoint. Callers refer explicitly to
`folio::config`; there are no forwarding Config aliases in Application. Storage aliases the shared path values for compatibility.

Config uses internal [File I/O](io.md) for bounded byte reads and generic path
predicates. Config owns effective-root policy through public `validate_effective_paths()`,
implemented in `config/configuration.cpp`:
absolute paths, existing directory types, alias-aware separation and the Data
ancestor exception. Storage reuses these configuration-path constraints through
its existing `validate_storage_paths()` adapter, then enforces Entry layouts,
locators and persistence safety in its own operations. Errors retain each caller's
established type. Config has no dependency on private Storage or Application.

## Push delivery resource settings

Optional `[ipc]` settings use factory defaults when omitted:

| Setting | Default | Meaning |
| --- | --- | --- |
| `backend_sessions` | 64 | Maximum logical sessions (two connections each) |
| `backend_requests` | 32 | Accepted request capacity |
| `backend_buffered_requests` | 4 | Maximum-size buffered exchanges |
| `frontend_push_queue_capacity` | 256 | Received notifications retained per Frontend session |
| `backend_push_queue_capacity` | 256 | Pending notifications per Backend recipient session |
| `push_ack_timeout_ms` | 30000 | Backend send plus receipt-ACK deadline in milliseconds |

Capacities and timeout must be positive integers representable by their runtime
types; timeout also must fit safe steady-clock deadline arithmetic. Overflow
terminates the affected session rather than dropping notifications. These are
startup settings, not hot-reloaded. Protocol payload-size bounds remain fixed.

**TODO CONFIG-UI-01 — Deferred Implementation:** decide settings presentation and
ordinary/advanced classification during settings UI design. No classification or
separate configuration hierarchy is established now.

Backend `RuntimeLimits` is constructed explicitly from validated configuration;
it has no independent defaults. Injected-path startup uses the same factory template.
