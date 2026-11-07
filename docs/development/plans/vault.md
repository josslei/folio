# Vault design boundaries

**Status: Decided/Planned; Vault persistence and encryption are not implemented.** Libsodium currently supplies UUID randomness. Core privacy enum values and plaintext codec fixtures are not security features. See [architecture](../../architecture/system.md) and [Storage's existing direction](../../contracts/storage.md).

## Privacy contract

Normal data is plaintext. Protected and Hidden data belongs to Vault and uses the same cryptographic strength. Locked visibility: Protected Logs expose only the permitted Log date; Protected Memos/Todos expose existence placeholders; Hidden Entries are absent. Private titles, tags, timezone/location, and logical organization must remain encrypted. Unlocked views expose permitted full data. Ordinary MVP privacy mutation remains unavailable; cross-domain operations are later Recovery work.

Directory is a first-class Entry. Vault must have an authoritative persistent representation for Directory UUID and metadata; a rebuildable Index alone is insufficient. Directory has Memo/Todo kind, no parent_id, no nested organization. Exact Vault Directory representation and locked visibility need design.

## Intended components

Vault initialization, password-based master-key protection using Argon2id, master-key lifecycle, versioned authenticated envelopes, VaultRepository, Public/Private Indexes, lock/unlock, and visibility enforcement are planned. Existing Storage design records XChaCha20-Poly1305 direction; this consolidation does not finalize envelope, key wrapping, algorithms for draft encryption, or cryptographic parameters. Review the detailed Vault design before implementation; do not invent cryptography.

Repositories own authoritative data; Catalog remains a reusable metadata/query model. Share opaque atomic I/O and logical metadata where appropriate without exposing plaintext private metadata while locked. UUIDs identify Entries across privacy/storage changes; physical paths do not.

## Index synchronization

Unlike local-only Normal Cache, Vault Public and encrypted Private Indexes are intended to synchronize with Vault data. Private Index avoids decrypting every Entry simply to rebuild metadata after sync; Public Index enables allowed locked visibility before decryption. Both remain projections; encrypted authoritative Entries must allow reconstruction, including Directories. Exact paths, safe locator disclosure, Hidden-entry leakage analysis, consistency rules, and rebuild protocol are Open/Deferred.

## Frontend security boundary

Backend owns Vault decryption capabilities. Frontend may display/edit plaintext but must not own decryption keys. Durable Vault drafts must be encrypted before writing, including while Backend is unavailable. Encrypt-only Frontend recovery capability is mandatory; asymmetric scheme, distribution, rotation, and decryption protocol are deferred. See [Recovery](recovery.md).

## Required later review

Resolve encryption envelope/versioning, Argon2id parameters, master-key protection/rotation/lifetime, memory and process boundaries, Directory persistence, Public Index leakage, encrypted pending operations, and Git history implications. A private Git remote is not end-to-end encryption. Do not present Protected/Hidden workflows as secure before implementation and testing.
