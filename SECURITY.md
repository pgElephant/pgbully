# Security Policy

## Supported versions

Security fixes are provided for the latest released minor version of pgBully,
built against currently-supported PostgreSQL releases (15, 16, 17, 18).

| Version | Supported |
|---|---|
| 1.0.x | ✅ |
| < 1.0 | ❌ |

## Reporting a vulnerability

**Please do not report security vulnerabilities through public GitHub
issues.**

Instead, report them privately by either:

- using GitHub's **"Report a vulnerability"** workflow under the repository's
  *Security* tab (Private Vulnerability Reporting), or
- emailing **security@pgedge.com** with the details.

Please include:

- a description of the vulnerability and its impact;
- the pgBully version and PostgreSQL version affected;
- step-by-step reproduction instructions or a proof of concept;
- any suggested mitigation, if you have one.

You can expect an acknowledgement within **3 business days** and a status
update within **10 business days**. We will coordinate a disclosure timeline
with you and credit you in the release notes unless you prefer to remain
anonymous.

## Security considerations when deploying

- Peer communication uses libpq on the normal PostgreSQL port, so it inherits
  your `pg_hba.conf`, TLS, and authentication configuration. **Use TLS
  (`sslmode=verify-full`) and authenticated connections** in the `conninfo`
  strings of `pgbully.nodes` on untrusted networks.
- The `rpc_*` and `force_election()` functions are revoked from `PUBLIC` on
  install. Grant the `rpc_*` functions only to the dedicated role your peers
  connect as.
- pgBully elects a leader but does not fence writes. Do not rely on it alone
  for split-brain protection in correctness-critical workloads; see
  [docs/operations.md](docs/operations.md#consistency-caveats).
