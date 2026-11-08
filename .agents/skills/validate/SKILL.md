---
name: validate
description: Run Folio repository validation when explicitly requested before reviewing or committing a subphase.
---

Read `docs/engineering/harness.md` for check coverage and limitations.
Run `python3 scripts/validate.py` from the repository root.
Keep this skill check-only; use `$validate-fix` for formatting fixes. Honor explicit check,
build-directory and resource-suite selections. For documentation-only changes,
use `--checks whitespace docs` and review affected links/contracts separately.

Report executed checks, failures, missing prerequisites, log locations and actual
platform. Missing formatter/analyzer executables or compilation databases block
validation; report prerequisites without silently skipping checks.
Do not install tools, choose policies, stage or commit files merely to obtain a
passing result. Do not apply formatting fixes through this skill.
Do not apply automatic clang-tidy fixes; investigate findings within authorized scope.
Leave commit-msg unchanged.
