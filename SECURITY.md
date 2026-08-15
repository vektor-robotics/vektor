# Security policy

## Supported versions

VEKTOR is pre-1.0. Security fixes are applied to the latest tagged release.

## Reporting a vulnerability

Do not open a public issue for a suspected vulnerability. Use GitHub's private
vulnerability reporting feature on the Security tab of this repository. Include
affected versions, impact, reproduction steps, and any suggested mitigation.

Please allow the maintainers time to validate and coordinate a fix before public
disclosure.

## Operational credential security

Use the [credential and certificate rotation runbook](docs/credential-rotation.md)
for planned renewal, CA migration, signing-key changes, and emergency compromise
response. VEKTOR does not currently live-reload agent-side TLS or trust policy.
