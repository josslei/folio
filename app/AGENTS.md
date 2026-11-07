# Frontend working constraints

Applies to app/. Global defaults remain in the root instructions. For cross-module
work also consult the affected module instructions through the root index.

- Keep launching/discovery, handshake and BackendConnectionHandle reusable by TUI,
  CLI and future GUI here; Frontend reuse does not make them shared runtime code.
- Use launch_backend(); generic process/executable helpers stay role-private.
  Launch success is not Ready. Read the [startup contract](../docs/contracts/backend-lifecycle.md#process-startup-and-ownership)
  before changing process ownership or discovery.
- Public methods remain blocking and sequential per connection. UI/IPC worker and
  reconnection integration is deferred: do not turn the API asynchronous or replay
  unconfirmed Mutations. See [IPC](../docs/contracts/ipc.md) and
  [Frontend plan](../docs/development/plans/application.md).
- Keep presentation separate from business rules. Consult [Access](../docs/contracts/access.md)
  or [Mutations](../docs/contracts/mutations.md) for affected operations. Preserve
  buffers and outcome distinctions when changing future error presentation.
