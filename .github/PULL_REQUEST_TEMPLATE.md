## Summary

Briefly describe what this PR changes and why.

Fixes #(issue)

## Type of change

- [ ] Bug fix (non-breaking change that fixes an issue)
- [ ] New feature (non-breaking change that adds functionality)
- [ ] Breaking change (fix or feature that changes existing behavior)
- [ ] Documentation only

## How has this been tested?

- [ ] `./test/integration.sh` passes
- [ ] `make installcheck` (TAP suite) passes
- [ ] Built cleanly against PostgreSQL versions: (list, e.g. 15/16/17/18)

Describe any additional manual testing.

## Checklist

- [ ] Code follows the PostgreSQL source formatting and error-message style
- [ ] Builds with no new warnings on all of PG 15–18
- [ ] Shared-memory access is guarded by `PgbShared->lock`
- [ ] Added/updated tests covering the change
- [ ] Updated `docs/` where relevant
- [ ] Added an entry to `CHANGELOG.md` under `[Unreleased]`
