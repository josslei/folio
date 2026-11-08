# Documentation and harness cleanup plan

**Status: Documentation migration and harness tooling implemented; interface-comment
review and documentation consistency review completed; automatic instruction
discovery remains unverified.**
The compact baseline, indexes, role instructions and engineering guides are in use.
Formatter/static-analysis configuration and explicit validation/fix commands are
established; remaining toolchain/static-analysis issues are not resolved by comments.
The comment review covered public Application/Core/Config contracts, Frontend
connection usage, Backend orchestration/scheduling/lifecycle, and Storage/I/O/IPC
boundaries. It clarified ownership, accepted-work lifetime, publication versus
durability and confirmed versus missing results without changing implementation.
This is a review baseline, not a guarantee that every code path needs no further
explanation. Continue documenting contracts as interfaces evolve.
This plan authorizes no new application feature or additional runtime guarantee.

## Agreed sequence

1. Design documentation and harness together: knowledge ownership, conventions
   and which rules automation should enforce.
2. Establish indexes and essential instructions, including a compact shared
   architectural baseline.
3. Implement the harness against those instructions; document actual tools,
   build/test entry points and enforcement limits.
4. Reorganize remaining documents and improve code documentation using that
   structure and tooling.
5. Verify links, navigation, instruction discovery and consistency between
   written rules and automated checks.

## Knowledge, audience and instruction scope

Documentation follows knowledge boundaries rather than mirroring app/server/lib.
Architecture explains relationships; contracts specify invariants and semantics;
engineering describes working conventions; development tracks current status,
future plans and history. Lookup/reference material can remain with its topic
until a separate reference group is useful.

Human-oriented material emphasizes diagrams, examples and rationale. Agent-oriented
material emphasizes compact, precise constraints and direct links, and remains
human-readable. It is not synonymous with AGENTS.md. Maintain one authoritative
contract per topic, allowing linked summaries and complementary presentations.
Do not force every topic into two documents or fragment coherent explanations.

Root AGENTS.md should become a small, stable entry point with critical boundaries,
authorization/data-safety constraints and validation/navigation links. Nested
instructions add only distinct local constraints; do not duplicate parent rules
or create files solely to complete a directory hierarchy. Verify cross-directory
instruction discovery rather than assume automatic loading.

Read the shared architectural baseline, then load details by change impact.
Cross-module changes may require several contracts even when only one directory
is edited. When impact is unclear, broaden review before implementation.
Indexes should explain when to read a document, its role/status, and how to reach
details. Add deeper indexes only when navigation needs them.

## Code documentation and enforcement

Document correct-use facts near interfaces: ownership, lifetime, execution-thread
requirements, preconditions, side effects and failure semantics, including private
interfaces. Explain non-obvious implementation rationale near the implementation.
Keep cross-module reasoning and detailed contracts in linked documents. Tests
provide evidence, not substitutes for contracts. TODOs must not imply future
capabilities are implemented.

Separate mechanical formatting, semantic C++ conventions and architectural rules.
Select formatter and static-analysis checks deliberately; existing patterns are
evidence rather than automatically approved rules. Record each rule's owner,
automated check where practical and remaining review obligations. Do not mix a
repository-wide formatting diff into substantive cleanup. The formatter and selected static-analysis checks are now defined in the
[harness guide](../engineering/harness.md); this plan does not authorize additional
tooling or repository-wide formatting passes.

## Status and preservation

Keep contracts, current capabilities, active future plans and historical reports
distinct. A plan may contain accepted decisions and open questions. Completed
plans retain rationale but must not be the sole home of current requirements.
Preserve existing MUT/IPC/BACKEND follow-ups rather than creating duplicate TODOs.
Resolve contradictions explicitly; do not let reorganization silently change
Storage semantics, runtime behavior or approved design.

The architecture/contracts/engineering/development layout is now in use; the
[migration record](reports/documentation-migration.md) locates former documents.
No empty reference tree or extra nested indexes were introduced. Continue keeping
source, build configuration, Git staging and unrelated working-tree changes intact
during documentation-only work. Remaining tooling and code documentation follow
the agreed sequence above.

## Consistency review — 2026-11-08

Reviewed root/role instructions, documentation indexes, architecture summaries,
current status/roadmap, engineering guidance, validation skills/script and the
Access/Mutation, lifecycle, Configuration, Storage and I/O boundaries against
representative current interfaces and implementations. Corrected stale pending
harness/comment-audit wording, obsolete future Application ownership wording,
atomic-I/O error attribution, and ambiguous default-root/lifecycle terminology.
No runtime behavior, API or future-phase scope changed.

Navigation was checked manually from the root baseline/task index to all three
role instructions and the relevant contract owners. Local Markdown links and
heading fragments were checked by `scripts/check_docs.py`; whitespace by Git diff
checks. This establishes explicit navigation, not automatic instruction loading by
Codex. Root-launched/cross-directory automatic discovery still needs an independent
session-level check. Static-analysis findings/toolchain compatibility and native
platform verification remain separate obligations; this review does not rerun or
certify runtime tests or exhaustively prove all code matches every contract.
