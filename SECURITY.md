# Security policy

## Supported versions

Only the most recent minor release on the current major version
receives security fixes.  At the time of writing that is
**v2.2.x**.  Older majors are unsupported.

| Version | Supported |
|---------|-----------|
| 2.2.x   | ✓ |
| 2.1.x   | ✗ |
| 2.0.x   | ✗ |
| 1.x.x   | ✗ |

If you are vendoring an older version and need a backport, a fix
to the latest version plus a port-back-yourself patch is the
expected path.

## Reporting a vulnerability

**Do not open public issues for security problems.**  The
sparsemap repository is read by several PostgreSQL extension
maintainers; a public report tells every consumer exactly how to
exploit them before a fix is available.

Report vulnerabilities by email to the maintainer:

> **greg@burd.me**

Encrypt with PGP if the issue is sensitive; key fingerprint:

> (publish your fingerprint here when you have one)

Include in your report:

- A description of the vulnerability
- The affected versions
- A reproducing input or test case
- Whether you have informed any downstream consumer (pg_tre,
  postgres/undo) directly

## Response timeline

- **Acknowledgement**: within 7 days of receipt
- **Initial triage**: within 14 days
- **Fix or workaround**: within 90 days for confirmed
  vulnerabilities, sooner for actively-exploited ones

This is a single-maintainer project.  These timelines are
best-effort; if your timeline is tighter, say so in the report
and we can discuss.

## Disclosure

We follow coordinated disclosure.  Once a fix is released and
downstream consumers have had time to upgrade (typically 14 days),
the vulnerability is documented in the release notes and CVE if
appropriate.

## Scope

The following are in scope for security reports:

- Memory-safety bugs (out-of-bounds read/write, use-after-free,
  double-free)
- Crashes triggered by serialized input (`sm_open`,
  `sm_deserialize`)
- Logic bugs that lead to data corruption in the bitmap
- Allocator-callback misuse that could be exploited
- Integer overflows in size calculations

Out of scope:

- Performance issues that aren't denial-of-service
- Behavior under deliberate misuse of the C API (e.g. passing
  freed pointers, double-init)
- Bugs in the test harness, examples, or documentation
- Issues in the experimental branches (`experiment/*`)

## Hall of fame

Security researchers who report valid vulnerabilities are
acknowledged here, with permission, after the fix ships.

(empty so far)
