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

## Current focus

### v0.6 — Reconcile

Make a VEKTOR deployment change the machine's running workload, then verify that
the observed runtime matches the desired digest. This closes the most important
gap in the current deployment path.

- [x] Define a versioned runtime-driver interface independent of any one supervisor
- [x] Ship the initial Docker/Podman single-container backend
- [x] Persist desired, observed, and previously active artifact state separately
- [x] Add idempotent start, stop, inspect, and rollback reconciliation operations
- [ ] Add a strict workload spec for arguments, environment, mounts, and devices
- [ ] Require runtime readiness and ROS health before rollout activation completes
- [x] Detect drift between desired and observed runtime state
- [x] Recheck observed state after agent restart without unintended promotion
- [ ] Add explicit reconciliation progress and failures to operator CLI output
- [ ] Complete integration coverage for timeout, crash recovery, drift, and rollback

Definition of done: a two-robot test fleet can deploy a digest-pinned container,
prove that exact digest is running and healthy, survive an agent restart, detect
manually introduced drift, and restore the previous healthy artifact on failure.

## Planned

### v0.7 — Trust

Verify artifact identity and make every deployment decision attributable.

- Keyless and public-key OCI signature verification
- Configurable trust policy enforced before preparation
- Append-only structured audit events for operator and agent actions
- Artifact provenance surfaced in deployment status
- Negative tests for unsigned, untrusted, and tampered artifacts

### v0.8 — Control

Add production access controls and operational safety boundaries.

- Role-based authorization for inspect, deploy, promote, and rollback
- Workload and fleet scopes derived from authenticated identity
- Approval policy for sensitive environments and large rollout waves
- Credential and certificate rotation guidance
- Stable, machine-readable authorization failures

### v0.9 — Harden

Prove reliability under realistic fleet and failure conditions.

- Multi-day soak tests and restart/fault injection
- Scale and performance baselines for fleet polling and rollout orchestration
- API and configuration migration tests
- Upgrade, downgrade, backup, and recovery documentation
- Release-candidate security review

### v1.0 — Production readiness

Stable compatibility guarantees for the agent API, configuration schemas, CLI
automation output, deployment state, and supported runtime-driver interface.

The ordering may change when field testing reveals a higher-risk gap, but v0.6
remains the next milestone until end-to-end runtime reconciliation is complete.
