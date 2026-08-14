# VEKTOR roadmap

## v0.1 — Check

Health-policy validation for ROS 2 nodes, topics, TF, and lifecycle state.

## v0.2 — Status

Machine identity, aggregate health state, watch mode, timing, JSON snapshots, and
bounded local history.

## v0.3 — Agent

A lightweight machine agent exposing versioned status snapshots over gRPC with
mutual authentication and offline-safe local operation.

- Periodic policy evaluation
- Versioned `GetStatus` and `WatchStatus` RPCs
- Mutual TLS for network listeners
- Explicit loopback-only insecure development mode
- Sequence-numbered snapshots and bounded local history

## v0.4 — Fleet

Fleet inventory, labels, health aggregation, and staged target selection.

- Strict YAML inventory and shared transport policy
- Concurrent gRPC status collection with deadlines
- Label selectors and deterministic target limits
- Aggregate health with identity-mismatch detection
- Text, JSON, and watch output

## v0.5 — Deploy

OCI artifact delivery, health-gated rollout waves, promotion, pause, and automatic
rollback.

- Digest-pinned OCI pulls through a configured Docker or Podman runtime
- Restart-safe agent deployment records and versioned deployment RPCs
- Deterministic, non-overlapping rollout waves
- Explicit promotion between healthy waves
- Automatic rollback on preparation, activation, or health-gate failure
- Operator-triggered fleet rollback in reverse application order

## v1.0 — Production readiness

Signed artifacts, policy audit trails, role-based access, long-running soak tests,
and stable compatibility guarantees.
