# Changelog

All notable changes to VEKTOR are documented here.

## Unreleased

### Added

- Independent activation and rollback routing for three named workloads per
  agent, with unaffected workload runtimes preserved during rollback.
- Strict CPU and memory workload limits carried through YAML, gRPC state,
  fingerprints, and OCI runtime activation arguments.
- Automatic registration of legacy single-workload state as the `default`
  workload when a v1.2 registry is first opened.
- Three-workload integration coverage for concurrent preparation, isolated
  partial failure, and post-activation runtime drift.
- Stable workload-ID validation and deterministic per-workload deployment-state
  paths, retaining the legacy single-workload state path for `default`.
- A versioned workload-state registry that persists managed workload identities
  and gives each workload independent desired, observed, previous, and audit
  state machines across agent reconstruction.
- WSL systemd package-lifecycle qualification covering clean installation,
  restart persistence, upgrade, downgrade, and purge cleanup. Package purge
  now removes empty VEKTOR runtime and log directories while retaining
  operator-created diagnostic contents.
- Bounded status-history and Prometheus diagnostic collection in the redacted
  support bundle, with explicit caller-selected paths.
- Deployment-operation outcome counters in the bounded Prometheus metrics
  file, alongside per-RPC latency totals.
- A disposable-VM package lifecycle harness for clean install, reboot,
  upgrade, rollback, and uninstall qualification.
- Redacted support bundles containing version metadata and configuration
  fingerprints without configuration contents, credentials, or audit payloads.
- Bounded Prometheus textfile metrics for agent health, authorization denials,
  reconciliation outcomes, and RPC activity without unbounded identifiers.
- Transactional SIGHUP/systemd health-policy reloads that validate candidates,
  retain the last known-good policy on failure, and audit every outcome.
- Versioned Ubuntu 24.04 package metadata, a least-privilege `vektor` system
  account, and a disabled-by-default hardened `systemd` agent service with
  explicit configuration, credential, state, and audit paths.
- Offline `vektor validate` support for strict health, fleet, rollout,
  authorization, approval, and trust configuration parsing without contacting
  ROS, agents, registries, or OCI runtimes.
- Opt-in real OCI runtime-driver qualification for digest-pinned Docker and
  Podman workloads on Ubuntu 24.04 / ROS 2 Jazzy.
- v1.0.0 release metadata, an enforced CMake/package version match, and a
  supported-platform, versioning, and security-fix policy.
- Schema-versioned `check --format json` output and golden fixtures for all
  machine-readable check, status, fleet, and rollout command responses.
- v1 compatibility contract covering gRPC, YAML, CLI JSON, persisted state,
  and extension-interface support windows.
- Optional schema-1 markers for health and fleet YAML, retaining support for
  existing unversioned configuration files and failing closed on future schemas.
- Bounded fleet polling concurrency with deterministic inventory-order reports,
  including strict configuration validation for large-inventory operation.
- Applied the fleet concurrency limit to rollout preparation, activation,
  observation, and rollback RPCs.
- Upgrade fixtures for deployment-state schemas 1 through 4, proving safe
  re-observation and migration to the current persisted schema.
- Repeated crash-recovery rollout testing and a timed, release-candidate soak
  harness for multi-day fault-injection campaigns.
- An operator runbook for safe agent upgrades, compatible-state downgrades,
  restricted backups, and failure recovery.
- Bounded captured OCI runtime output to 64 KiB, preventing noisy local runtime
  failures from exhausting agent memory.
- Protobuf v1 wire-contract tests and a CSV performance-baseline harness for
  fleet polling and disposable rollout qualification.
- Release-candidate evidence procedures for multi-day soak campaigns and fleet
  and rollout scale baselines.
- Versioned runtime-driver interface with an initial Docker/Podman backend.
- Managed-container activation, observation, stop, and rollback operations.
- Persistent desired and observed artifact state, runtime ID, and drift status.
- Startup and periodic runtime drift detection.
- Protection against replacing or stopping same-named unmanaged containers.
- Strict workload configuration for network mode, restart policy, environment,
  bind mounts, devices, and command arguments.
- Restart-safe restoration of the previous artifact and workload configuration.
- Deployment API schema v4 fields for runtime readiness and persisted
  reconciliation operation progress.
- Bounded OCI runtime commands with process-tree termination on timeout.
- OCI health-check readiness polling and fresh post-activation ROS health gates.
- Restart recovery for interrupted prepare, activation, and rollback operations
  without silently promoting an interrupted activation.
- Rollout JSON schema v2 with per-robot phase and operation fields.
- Deterministic two-robot integration coverage for runtime timeout, interrupted
  activation recovery, manually introduced drift, and reverse-order rollback.
- Strict versioned trust policies with bounded Cosign public-key and keyless
  verification before OCI artifact preparation.
- Deployment API and persisted-state schema v5 with artifact verification
  method, signer, issuer, and timestamp provenance.
- Negative trust tests for malformed policy, rejected signatures, and verifier
  timeouts, including proof that rejected artifacts never reach the runtime.
- Durable append-only JSON Lines audit events for attributed operator requests,
  artifact verification, runtime preparation, drift, and restart recovery.
- Fail-closed audit persistence that prevents runtime mutation when the agent
  cannot durably record the initiating event.
- Real Cosign and OCI-registry integration coverage for trusted signed,
  unsigned, wrong-key, and post-signing tag-tampering cases.
- Explicit, secure-by-default public-key policy controls for isolated HTTP
  registries and signatures intentionally excluded from a transparency log.
- Strict mTLS identity-to-role authorization for inspect, deploy, promote, and
  rollback RPCs, with pre-mutation enforcement and audited denials.
- Stable JSON `VEKTOR_AUTHORIZATION_DENIED` error bodies carried by gRPC
  `PERMISSION_DENIED` responses.
- Schema-2 authorization policies that bind authenticated identities to explicit
  fleet and workload scopes, with exact matching or deliberate `*` wildcards.
- Fleet and rollout clients propagate authorization scopes on every agent RPC,
  including deployment-status recovery requests.
- Agent-side fleet and workload bindings prevent callers from spoofing a scope
  label that does not identify the agent's configured resource.
- Strict signed approval policies for sensitive environments and oversized
  rollout waves, with distinct trusted approvers and bounded validity windows.
- Approval signatures bound to the exact deployment digest, fleet, workload,
  environment, and wave before target-wave polling or agent mutation.
- Rollout JSON schema v3 fields for approval requirements and verified
  approver identities.
- An operator runbook for routine and emergency rotation of mutual-TLS
  certificates, authorization identities, approval keys, and Cosign trust.

## [0.5.0] - 2026-08-14

### Added

- Digest-pinned OCI artifact preparation through Docker or Podman.
- Versioned agent RPCs for prepare, activate, inspect, and rollback operations.
- Atomic, restart-safe desired-deployment state on every agent.
- Strict rollout YAML with deterministic, non-overlapping waves.
- `vektor deploy`, `vektor promote`, and `vektor rollback` commands.
- Preflight and post-activation health gates with automatic wave rollback.
- Persistent operator rollout state for deliberate, resumable promotion.

## [0.4.0] - 2026-08-14

### Added

- `vektor fleet` command for concurrent multi-agent health collection.
- Strict YAML fleet inventory with robot endpoints and labels.
- Deterministic label selectors and bounded staged target selection.
- Mutual-TLS fleet client credentials and loopback-only insecure mode.
- Aggregate fleet health with explicit unhealthy and unreachable robots.
- Text, JSON, and continuous watch output for fleet operators.

## [0.3.0] - 2026-08-14

### Added

- Long-running `vektor agent` command with periodic health inspection.
- Versioned gRPC `GetStatus` and server-streaming `WatchStatus` methods.
- Mutual TLS with mandatory client-certificate verification for network use.
- Explicit insecure development mode restricted to loopback and Unix sockets.
- Sequence-numbered protobuf snapshots backed by offline-safe local history.

## [0.2.0] - 2026-08-14

### Added

- `vektor status` health snapshots with robot, host, time, and ROS domain metadata
- Healthy, degraded, unhealthy, and unreachable aggregate states
- Text and schema-versioned JSON status output
- Continuous `--watch` mode with configurable intervals
- Bounded local JSONL status history
- Total and per-check duration measurements

## [0.1.0] - 2026-08-14

### Added

- `vektor check` for ROS nodes, topics, TF, and lifecycle states
- YAML policies with topic QoS and timeout controls
- Text and JSON check output
- ROS 2 Jazzy integration tests and GitHub Actions CI
