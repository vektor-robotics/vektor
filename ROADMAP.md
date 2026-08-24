# VEKTOR roadmap

VEKTOR is building a trustworthy software-delivery control plane for autonomous
machines. The v1 engineering foundation is complete: health-aware delivery,
runtime reconciliation, artifact trust, access control, fault hardening, and
stable compatibility contracts. Post-v1 work prioritizes operability on real
robots, multiple workloads per machine, and evidence-backed fleet scale.

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

## Completed milestone

### v0.9 — Harden

Prove reliability under realistic fleet and failure conditions.

- [x] Repeated crash-recovery fault-injection test and timed soak harness
- [x] Multi-day soak procedure and evidence-review checklist
- [x] Bound fleet polling fan-out and preserve deterministic reports at scale
- [x] Bound rollout orchestration fan-out with the fleet concurrency limit
- [x] Reproducible fleet polling and rollout performance-baseline harness
- [x] Deployment-state migration tests for every supported legacy schema
- [x] API wire-contract and configuration/state migration tests
- [x] Upgrade, downgrade, backup, and recovery documentation
- [x] Release-candidate security review: bounded runtime output, argument-vector
  execution, mTLS authorization, and durable audit-path checks

## Completed milestone

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

The v1.0 production-readiness engineering milestone is complete. VEKTOR now
defines stable v1 contracts for gRPC, configuration, CLI JSON, persisted state,
and extension interfaces; enforces aligned release metadata; and provides real
OCI runtime qualification automation for Ubuntu 24.04 / ROS 2 Jazzy.

## v1.0 release qualification gate

Engineering completion is not release evidence. Before creating the `v1.0.0`
tag, complete and archive the following against the exact candidate commit:

- [ ] Pass a continuous 72-hour rollout soak on dedicated Ubuntu 24.04 / ROS 2
  Jazzy test hardware with no ignored or retried failures
- [ ] Record fleet polling baselines for 25, 100, and the largest intended
  inventory, including cap `1` and the intended production concurrency cap
- [ ] Record complete canary and larger-wave rollout baselines against an
  isolated registry and disposable fleet
- [ ] Pass digest-pinned runtime qualification for Docker and for Podman when
  Podman is a supported target
- [ ] Review the retained state, audit logs, JSON reports, CSV samples, and
  environment metadata; link the accepted evidence from the release record
- [ ] Finalize the changelog, create the signed `v1.0.0` tag, and publish release
  artifacts according to `docs/release-policy.md`

These are release-owner activities, not blockers for post-v1 development. A
failed gate remains visible and must not be converted into a checked item by
rerunning past the first failure.

## Current focus

### v1.1 — Operate

Make VEKTOR straightforward to install, supervise, diagnose, and upgrade on a
real robot without requiring a source checkout.

- [x] Produce versioned Ubuntu 24.04 packages and a hardened `systemd` agent
  service with explicit state, audit, policy, and credential paths
- [x] Add an offline `vektor validate` command for health, fleet, rollout,
  authorization, approval, and trust configuration
- [x] Add transactional agent policy reload with validation, audit events, and
  automatic retention of the last known-good configuration
- [x] Export bounded operational metrics for agent health, reconciliation,
  authorization denials, rollout outcomes, and RPC latency
- [x] Add a redacted support-bundle command that collects versions,
  configuration fingerprints, status, and recent diagnostics without private
  keys or secret values
- [x] Qualify clean install, reboot recovery, package upgrade, package rollback,
  and uninstall behavior in a disposable systemd-enabled Ubuntu 24.04 WSL
  environment

Lifecycle qualification installs a lower-version package, restarts WSL, upgrades
to the current package, downgrades again, and purges it. Release owners still
need to verify the signed release artifact on a clean production-equivalent
Jazzy machine before publishing it.

Definition of done: an operator can install a signed package on a clean Jazzy
machine, start VEKTOR as a least-privilege service, validate and safely reload
policy, observe its health, collect a redacted diagnostic bundle, and complete
an upgrade and rollback without losing deployment or audit state.

## Next

### v1.2 — Multi-workload

Remove the one-managed-container-per-agent limit while preserving independent
health gates and rollback boundaries.

- [ ] Introduce stable workload identities and per-workload desired, observed,
  previous, reconciliation, and audit state
- [ ] Extend authorization and rollout targeting so one identity can receive
  different roles and scopes for different workloads on the same robot
- [ ] Reconcile, inspect, deploy, and roll back named workloads independently
  without restarting or mutating unaffected workloads
- [ ] Add CPU, memory, and runtime resource limits to the strict workload spec
- [ ] Migrate existing single-workload state without manual edits or loss of the
  previous rollback target
- [ ] Add crash, drift, partial-failure, and concurrent-rollout integration
  coverage for at least three workloads per agent

Definition of done: a fleet can independently roll out and roll back three
named workloads per robot while preserving v1 single-workload behavior and
keeping unaffected workloads available.

### v1.3 — Observe and scale

Reduce control-plane cost and make large-fleet behavior explainable before
increasing default scale targets.

- [ ] Reuse gRPC channels and add bounded retry backoff and jitter for watch and
  rollout operations
- [ ] Add incremental fleet status streaming so unchanged robots do not require
  a full reconnect and report on every watch interval
- [ ] Export fleet and rollout events to an operator-selected durable sink while
  retaining the local append-only audit source of truth
- [ ] Add latency, error-rate, saturation, and stale-snapshot summaries with
  stable machine-readable output
- [ ] Add deterministic benchmark scenarios at 100, 500, and 1,000 simulated
  agents, followed by evidence runs at each claimed production scale
- [ ] Publish capacity guidance for concurrency, deadlines, watch intervals,
  control-plane CPU/memory, and expected network load

Definition of done: VEKTOR has a documented, reproducible fleet-size envelope;
operators can identify why a rollout is slow or blocked without collecting logs
manually from every robot.

## Later

### v1.4 — Runtime and inventory ecosystem

- Package and qualify additional runtime-driver implementations without
  weakening the v1 extension contract
- Support dynamic inventory providers alongside strict static YAML inventory
- Define policy-controlled rollout hooks for site-specific preflight and
  post-deployment checks
- Publish reference integrations and conformance tests for third-party drivers
  and inventory providers

### v2.0 — Persistent control plane

Evaluate a long-running, highly available control plane only after field use
validates the requirements. Candidate scope includes declarative desired state,
durable rollout scheduling, fleet-wide event indexing, operator workflows, and
an API/UI. Agents must remain offline-safe, fail closed, and usable directly
when the control plane is unavailable.

## Prioritization rules

- Safety, rollback correctness, and compatibility take priority over feature
  count or benchmark headline numbers.
- Every claimed platform, runtime, and fleet-size envelope requires retained,
  reproducible evidence.
- New centralized components must not remove local agent autonomy or require
  internet connectivity for safe operation.
- A milestone may change when design-partner use reveals a higher-risk gap, but
  its compatibility and migration impact must be documented before
  implementation.
