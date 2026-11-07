# Security and data-safety conventions

**Status:** current working constraints. Domain-specific security requirements
remain owned by [Logging](../contracts/logging.md#records-errors-and-privacy),
[I/O](../contracts/io.md), [Storage](../contracts/storage.md) and the approved
[Vault direction](../development/plans/vault.md).

Never commit passwords, private keys, tokens, or real personal records. Use synthetic data in tests and examples; never log sensitive record contents or cryptographic secrets. Do not implement custom cryptographic algorithms; use established libraries when encryption is designed. Treat encryption, key handling, and privacy boundaries as security-critical. Avoid silent data loss and destructive overwrites; preserve user data during migrations and synchronization. GitHub Private repositories do not provide end-to-end encryption.

Tests use isolated temporary paths and synthetic Entry data, including Config and
Logging initialization. Never use the developer's real Folio roots as fixtures.
Do not log raw Entry/IPC payloads, parser text, secrets or arbitrary exception
messages; error routing and logging do not justify disclosing another Frontend's
sensitive context. Diagnostic success never authorizes clearing a Session Marker
or marking a mutation saved.

Privacy enums and plaintext fixtures are not encryption. Vault cryptography and
recovery key distribution require later design/security review; the approved
in-memory/durable boundary does not authorize implementing them now.
