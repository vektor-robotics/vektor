# VEKTOR roadmap

VEKTOR is building a trustworthy software-delivery control plane for autonomous
machines. Near-term work prioritizes closing the deployment loop before adding
the controls required for production adoption.

## Released

### v0.1 — Check

Health-policy validation for ROS 2 nodes, topics, TF, and lifecycle state.

### v0.2 — Status

Machine identity, aggregate health state, watch mode, timing, JSON snapshots, and
bounded local history.

### v0.3 — Agent

A lightweight machine agent exposing versioned status snapshots over gRPC with
mutual authentication and offline-safe local operation.

- Periodic policy evaluation
- Versioned `GetStatus` and `WatchStatus` RPCs
- Mutual TLS for network listeners
- Explicit loopback-only insecure development mode
- Sequence-numbered snapshots and bounded local history

### v0.4 — Fleet

Fleet inventory, labels, health aggregation, and staged target selection.

- Strict YAML inventory and shared transport policy
- Concurrent gRPC status collection with deadlines
- Label selectors and deterministic target limits
- Aggregate health with identity-mismatch detection
- Text, JSON, and watch output

### v0.5 — Deploy

OCI artifact delivery, health-gated rollout waves, promotion, pause, and automatic
rollback.

- Digest-pinned OCI pulls through a configured Docker or Podman runtime
- Restart-safe agent deployment records and versioned deployment RPCs
- Deterministic, non-overlapping rollout waves
- Explicit promotion between healthy waves
- Automatic rollback on preparation, activation, or health-gate failure
- Operator-triggered fleet rollback in reverse application order

## Completed milestone

### v0.6 — Reconcile

Make a VEKTOR deployment change the machine's running workload, then verify that
the observed runtime matches the desired digest. This closes the most important
gap in the current deployment path.

- [x] Define a versioned runtime-driver interface independent of any one supervisor
- [x] Ship the initial Docker/Podman single-container backend
- [x] Persist desired, observed, and previously active artifact state separately
- [x] Add idempotent start, stop, inspect, and rollback reconciliation operations
- [x] Add a strict workload spec for arguments, environment, mounts, and devices
- [x] Require runtime readiness and ROS health before rollout activation completes
- [x] Detect drift between desired and observed runtime state
- [x] Recheck observed state after agent restart without unintended promotion
- [x] Add explicit reconciliation progress and failures to operator CLI output
- [x] Complete integration coverage for timeout, crash recovery, drift, and rollback

Definition of done: a two-robot test fleet can deploy a digest-pinned container,
prove that exact digest is running and healthy, survive an agent restart, detect
manually introduced drift, and restore the previous healthy artifact on failure.

The v0.6 reconciliation milestone is complete. The two-agent integration suite
now exercises those boundaries through the fleet and agent gRPC APIs, including
bounded runtime failure and reverse-order recovery.

## Completed milestone

### v0.7 — Trust

Verify artifact identity and make every deployment decision attributable.

- [x] Keyless and public-key OCI signature verification
- [x] Configurable trust policy enforced before preparation
- [x] Append-only structured audit events for operator and agent actions
- [x] Artifact provenance surfaced in deployment status
- [x] Complete negative integration tests for unsigned, untrusted, and tampered artifacts

The v0.7 trust milestone is complete. CI now verifies real Cosign signatures
against an ephemeral OCI registry and proves that unsigned content, signatures
checked with an untrusted key, and tags moved after signing all fail closed.

## Completed milestone

### v0.8 — Control

Add production access controls and operational safety boundaries.

- [x] Role-based authorization for inspect, deploy, promote, and rollback
- [x] Workload and fleet scopes derived from authenticated identity
- [x] Approval policy for sensitive environments and large rollout waves
- [x] Credential and certificate rotation guidance
- [x] Stable, machine-readable authorization failures

## Current focus

### v0.9 — Harden

Prove reliability under realistic fleet and failure conditions.

- [x] Repeated crash-recovery fault-injection test and timed soak harness
- Multi-day soak campaign execution and evidence review (procedure and
  evidence-review checklist are ready)
- [x] Bound fleet polling fan-out and preserve deterministic reports at scale
- [x] Bound rollout orchestration fan-out with the fleet concurrency limit
- Execute and review scale and performance baselines for fleet polling and
  rollout orchestration (reproducible harness is ready)
- [x] Deployment-state migration tests for every supported legacy schema
- [x] API wire-contract and configuration/state migration tests
- [x] Upgrade, downgrade, backup, and recovery documentation
- [x] Release-candidate security review: bounded runtime output, argument-vector
  execution, mTLS authorization, and durable audit-path checks

## Planned

### v1.0 — Production readiness

Stable compatibility guarantees for the agent API, configuration schemas, CLI
automation output, deployment state, and supported runtime-driver interface.

- [x] Publish the v1 compatibility contract and version legacy health/fleet
  configuration without breaking unversioned deployments
- [x] Define JSON output compatibility fixtures for check, status, fleet, and
  rollout CLI commands
- [x] Define the v1 release/versioning and support policy
- [x] Automate qualification of digest-pinned OCI runtime activation and the
  ROS 2 Jazzy upgrade path; run Docker and Podman evidence for each candidate

The v0.8 control milestone is complete. VEKTOR now enforces identity-derived
roles and resource scopes, gates sensitive waves with signed approvals, emits
stable authorization failures, and documents safe credential rotation. The
next focus is v0.9 production hardening.

The ordering may change when field testing reveals a higher-risk gap.
