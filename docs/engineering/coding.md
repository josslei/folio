# Coding and maintenance conventions

**Status:** existing repository conventions, with check-only formatter and static-analysis configuration. Both humans and agents use these rules. Architecture-specific
requirements remain in their [contract owners](../architecture/principles.md).

## C++ conventions

Use standard C++20 without compiler extensions. Follow existing two-space indentation, `snake_case` functions and files, `PascalCase` test suites, and `folio::core` namespaces. Mechanical formatting is defined by `.clang-format`; selected correctness checks
are defined by `.clang-tidy`. Run `$validate` explicitly before committing; see
[harness coverage](harness.md#explicit-validation-command).

Prefer RAII, standard library facilities, explicit ownership and lifetimes, and composition over unnecessary inheritance. Avoid raw owning pointers. Keep public interfaces minimal and implementation details out of public headers where practical. Use English identifiers, comments, diagnostics, and error messages. Keep CMake target-based, with explicit dependency relationships and appropriate visibility.

## Namespaces and initialization

Use folio::core for domain models, folio::core::storage for Backend persistence,
folio::core::storage::codec for Storage serialization and
folio::core::storage::internal for Storage implementation/testing helpers.
Backend process implementation uses folio::application::internal. Do not mechanically
move namespaces to match directories. Core stays independent of presentation.

Call folio::core::ensure_sodium_initialized() before using libsodium in Core code.
Id::generate() already calls the shared one-time helper; avoid duplicate ID-generation
paths or custom randomness/cryptography.

## File organization and dependency visibility

Every top-level module owns include/ declarations and matching module-relative
src/ implementations. include/ does not mean public: lib/include/folio is shared;
app/include and server/include are role-private and have no folio/ wrapper.
Header-only declarations/templates need no empty source. Ordinary headers do not
live under src/; intentional colocated private helpers and test seams use internal/.
Use responsibility directories, not generic application/ wrappers in app/server.

Keep target-based CMake dependencies and implementation dependencies PRIVATE where
possible. Each target exports only its own permitted headers/shared requirements.
No production target receives another module's complete src/ include path; explicit
test-only access is different. Storage remains part of folio_backend, not its own
public library. See [runtime-role boundaries](../architecture/system.md#runtime-role-module-structure--implemented).

## Code documentation

Put correct-use facts near interfaces, including private ones: inputs/results,
preconditions, ownership, lifetime, execution-thread requirements, side effects and
failure semantics. Explain non-obvious implementation choices near the code.
Cross-module rationale and detailed contracts belong in linked documentation;
do not duplicate full designs in headers or comment obvious statements.

Use `///` immediately above declarations for API documentation, including private
interfaces. Use ordinary `//` comments for implementation rationale and consecutive
`//` lines for longer explanations; avoid decorative banners. Add Doxygen tags
such as `@param`, `@return`, `@pre` and `@throws` only when they clarify a contract,
not as mandatory boilerplate. Inline field comments may remain `//`. Existing
useful comments need not be rewritten solely to change their delimiter.

Describe current guarantees accurately. Deferred capabilities need traceable TODOs,
not comments implying they exist. Tests provide evidence rather than replace a
written contract. For priorities and the human/agent distinction, see the
[cleanup plan](../development/documentation-harness-plan.md#code-documentation-and-enforcement).

## Workflow

Inspect relevant code and conventions before editing. Keep changes focused on the request; avoid unrelated features and refactoring. For nontrivial work, briefly explain the approach before implementation. Resolve ordinary implementation decisions using existing conventions without requiring routine approval. Preserve backward compatibility where applicable unless a breaking change is requested. Report what changed, what was actually verified, and any verification limitations.

Inspect the relevant module instructions and contracts before editing. Resolve
ordinary implementation details using established conventions; ask about unresolved
product/design choices rather than silently freezing them. A phase plan is not
authorization to implement later work.

## Documentation and change review

Keep one authoritative contract per topic; summaries link to it. Update affected
README/guides when behavior, architecture or commands change. Keep long implementation
explanations outside AGENTS.md. Preserve existing user edits and staging, including
during structural refactors. Exclude generated build output from version control.
If a commit message or PR description is requested, use concise imperative subjects
and explain concrete behavior plus actual verification. Include terminal output
or screenshots where useful for TUI changes. Root authorization restrictions apply.

## Enforcement

Formatting and selected static-analysis checks are configured in the
[harness](harness.md); analyzer findings and toolchain compatibility still require
verification. CMake already expresses role/visibility boundaries;
compilation and tests supply partial enforcement. Ownership, failure semantics and
design intent still require review. Do not infer that every existing code pattern
is an approved convention, or mix broad formatting changes into semantic cleanup.
