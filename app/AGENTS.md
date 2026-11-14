# Frontend working constraints

Applies to app/. Global defaults remain in the root instructions. For cross-module
work also consult the affected module instructions through the root index.

- Keep launching/discovery, handshake and BackendConnectionHandle reusable by TUI,
  CLI and future GUI here; Frontend reuse does not make them shared runtime code.
- Use launch_backend(); generic process/executable helpers stay role-private.
  Launch success is not Ready. Read the [startup contract](../docs/contracts/backend-lifecycle.md#process-startup-and-ownership)
  before changing process ownership or discovery.
- BackendConnectionHandle exposes asynchronous typed requests with main-thread result
  callbacks; its owned ConnectionSession executes blocking IPC on one Request Worker.
  Keep one outstanding request through completion consumption. Push reception/ACK
  remains independent. Do not replay unconfirmed Mutations. See [IPC](../docs/contracts/ipc.md)
  and [Frontend plan](../docs/development/plans/application.md).
- Keep presentation separate from business rules. Consult [Access](../docs/contracts/access.md)
  or [Mutations](../docs/contracts/mutations.md) for affected operations. Preserve
  buffers and outcome distinctions when changing future error presentation.

- Keep TUI page/navigation/display state under `tui/`, even without FTXUI calls.
  Only genuinely GUI-reusable communication belongs outside it. Follow the
  [Shell/Page/Layout/Component boundaries](../docs/architecture/system.md#frontend-and-tui-organization);
  keep page-specific headers and controls local rather than creating universal UI abstractions.

- Substantial TUI-specific nonvisual behavior belongs under `tui/logic/`; small
  presentation-local state may stay with its component. Code boundaries do not
  introduce threads or duplicate UI state in the communication runtime.

- TUI visual elements use `ftxui::ComponentBase` and compose child Components by default.
  Document a specific reason for exceptions; nonvisual helpers need no component base.
