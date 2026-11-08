---
name: validate-fix
description: Apply Folio C++ formatting and run repository validation when explicitly requested.
---

Read `docs/engineering/harness.md` for coverage and limitations.
Run `python3 scripts/validate.py --fix` from the repository root. Honor explicit
check, build-directory and resource-suite selections. This formats all first-party
C++ files, including files with existing edits, before running selected checks.
Review and report the resulting diff; preserve existing staging.

Report executed checks, failures, missing prerequisites, logs and actual platform.
Missing tools block validation; do not install tools or change policies to pass.
Do not apply automatic clang-tidy fixes. Investigate analyzer findings only within
separately authorized scope. Do not stage, commit or modify Git history.
Leave commit-msg unchanged.
