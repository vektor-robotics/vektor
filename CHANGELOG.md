# Changelog

All notable changes to VEKTOR are documented here.

## Unreleased

### Added

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
