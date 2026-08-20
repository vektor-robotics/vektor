# v1 release and support policy

VEKTOR follows semantic versioning beginning with `1.0.0`. The canonical
release version appears in both `package.xml` and the CMake project metadata;
the build rejects a mismatch.

## Compatibility and versioning

- Patch releases fix defects, security issues, and documentation without
  changing the v1 public contracts.
- Minor releases may add optional protobuf fields, optional YAML fields, and
  additive JSON fields. They retain the [v1 compatibility contract](compatibility.md).
- Major releases may remove or alter a public contract only with a documented
  migration path, supported-version overlap, and an explicit upgrade guide.
- Release tags use `vMAJOR.MINOR.PATCH`; package versions omit the leading `v`.

## Supported platform

The v1 support target is Ubuntu 24.04 LTS and ROS 2 Jazzy. The supported
runtime-driver contract is version 1, with the bundled Docker and Podman OCI
backends. VEKTOR supports the current v1 minor release on that platform; CI
tests this target for every pull request. Other operating systems, ROS
distributions, and OCI runtimes may work but are not release-qualified.

## Release checklist

1. Ensure all v1 compatibility fixtures, Jazzy tests, and artifact-trust CI
   pass.
2. Complete the v0.9 multi-day soak and real-fleet scale evidence review.
3. Update `CHANGELOG.md`, `package.xml`, and the CMake project version together.
4. Build a clean release candidate on Ubuntu 24.04 / Jazzy and run the upgrade
   and recovery procedure from [upgrade/recovery](upgrade-recovery.md).
5. Tag the reviewed commit as `vMAJOR.MINOR.PATCH`, publish release notes, and
   retain the qualification evidence with the release.

## Deprecation and security fixes

An interface is deprecated only in a minor release, with a replacement and an
announced removal target no earlier than the next major release. Security fixes
follow [SECURITY.md](../SECURITY.md); they do not weaken mTLS, authorization,
approval, trust, or audit fail-closed behavior for compatibility.
