# Shared-code working constraints

Applies to lib/. Shared code depends on neither runtime role.

- Core is presentation-independent. Reuse canonical locator-free metadata from
  folio/core/metadata.hpp; keep filesystem locators in private Storage composition.
  See the [domain baseline](../docs/architecture/system.md#domain-baseline--implemented).
- Before using libsodium in Core, call ensure_sodium_initialized(); Id::generate()
  already does so. Follow [coding conventions](../docs/engineering/coding.md).
- Config owns template/default/schema and effective-path policy, without private
  Storage/Application dependencies or global test overrides. See
  [Configuration](../docs/contracts/configuration.md).
- I/O remains byte-oriented and domain-independent; keep atomic create/replace
  distinct. Use the [I/O contract](../docs/contracts/io.md) for errors/durability.
- Logging uses the process default spdlog logger without controlling lifecycle or
  errors. Follow [privacy and failure rules](../docs/contracts/logging.md).
- Shared IPC headers are implementation-facing cross-role facilities, not an
  alternative business API. Preserve [protocol](../docs/contracts/ipc.md) and
  [Mutation contracts](../docs/contracts/mutations.md) when changing codecs/types.
