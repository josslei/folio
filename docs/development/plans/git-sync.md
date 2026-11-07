# Git synchronization direction

**Status: Decided/Planned. No Git integration is implemented.** See [roadmap](../roadmap.md), [Vault](vault.md), and [Recovery](recovery.md).

```text
One Git repository
├── Normal branch / worktree: human-readable authoritative Entries
└── Vault branch / worktree: encrypted authoritative Entries and synchronized Vault indexes
```

UUIDs remain Entry identities; Git paths and renames are locators. Normal Index is local-only in Cache. Recovery state, process locks, runtime resources, drafts, and configuration are not automatically Git content. Define exclusions explicitly during setup.

Sync will own repository/worktree setup, branch management, status, synchronization, conflict detection/resolution, and Vault history maintenance/compaction. It must coordinate with Backend serialized operations, Catalog refresh, and recovery safety, without moving business logic into Frontends.

Textual Git merge is not automatically semantically correct for an Entry. Duplicate UUIDs, conflicting membership/locators, Directory metadata, and partially coordinated changes require explicit semantic handling. Do not silently overwrite authoritative data or treat Index inconsistency as proof of corrupt Entries.

Open/Deferred: remote/authentication UX, sync ordering and concurrency, conflict representation and resolution, encrypted Index/data coherence, key availability during sync, recovery of interrupted sync, history compaction policy and security implications, and cross-device schema compatibility. No algorithm or remote-provider-specific behavior is frozen here.
