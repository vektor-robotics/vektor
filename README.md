# VEKTOR

Safe software delivery for autonomous machines.

VEKTOR is an open-source, health-aware deployment tool for ROS 2 fleets.
`vektor check` validates a live ROS graph, `vektor status` produces a
timestamped machine-health snapshot, `vektor agent` serves health and deployment
operations over gRPC, `vektor fleet` aggregates selected machines, and the
deployment commands run health-gated OCI rollouts.

## Milestone 1 checks

- Required node presence
- Topic existence
- Topic frequency over a configurable sampling window
- Per-topic reliable or best-effort QoS
- TF connectivity between two frames
- Managed-node lifecycle state through `/<node>/get_state`
- Text or machine-readable JSON output

The check exits `0` only when every configured check passes, `1` when a health check fails, and `2` for invalid CLI/configuration errors.

## Status snapshots

```bash
vektor status --config config/example.yaml
vektor status --config config/example.yaml --format json
vektor status --config config/example.yaml --watch --interval-ms 5000
```

Snapshots include the robot ID, hostname, UTC timestamp, ROS domain ID, overall
state, total inspection duration, and individual check durations. Overall state
is one of `healthy`, `degraded`, `unhealthy`, or `unreachable`.

By default, `status` keeps the latest 100 JSON snapshots at
`$XDG_STATE_HOME/vektor/status.jsonl` or
`~/.local/state/vektor/status.jsonl`. Override this with `--history <path>` or
disable persistence with `--no-history`. `--robot-id` overrides `robot_id` from
the policy.

## Agent

The agent runs the same health policy continuously and exposes two versioned
gRPC methods:

- `GetStatus` returns the latest snapshot.
- `WatchStatus` streams each new snapshot as it is published.

For local development, insecure transport must be requested explicitly and is
restricted to loopback or Unix sockets:

```bash
ros2 run vektor vektor agent --config config/talker.yaml \
  --listen 127.0.0.1:50051 --interval-ms 5000 --insecure \
  --oci-runtime docker --runtime-container vektor-workload \
  --deployment-state .vektor/robot-001-deployment.yaml \
  --trust-policy config/trust.example.yaml
```

Any non-loopback deployment requires mutual TLS. The server certificate and key
identify the agent; `--tls-ca` is the CA used to authenticate fleet clients:

```bash
ros2 run vektor vektor agent --config /etc/vektor/policy.yaml \
  --listen 0.0.0.0:50051 \
  --tls-cert /etc/vektor/tls/agent.crt \
  --tls-key /etc/vektor/tls/agent.key \
  --tls-ca /etc/vektor/tls/client-ca.crt
```

The first request may return gRPC `UNAVAILABLE` until initial ROS discovery and
inspection finish. The agent continues writing the bounded local status history
while fleet connectivity is unavailable.

## Fleet inventory and targeting

Fleet inventory is a strict YAML document containing robot identities,
endpoints, labels, request deadlines, and shared client transport credentials.
See `config/fleet.example.yaml` for a loopback development inventory.

Query every robot:

```bash
ros2 run vektor vektor fleet --config config/fleet.example.yaml
```

Select a deterministic canary stage using one or more label predicates and an
inventory-order limit:

```bash
ros2 run vektor vektor fleet --config config/fleet.example.yaml \
  --selector site=berlin --selector role=picker --limit 1
```

Use JSON for automation or watch the fleet continuously:

```bash
ros2 run vektor vektor fleet --config config/fleet.example.yaml --format json
ros2 run vektor vektor fleet --config config/fleet.example.yaml \
  --watch --interval-ms 5000
```

Fleet requests run concurrently with the configured per-agent deadline. A robot
is reported as `unreachable` when its RPC fails, times out, or returns a snapshot
older than `max_snapshot_age_ms`. Unsupported schemas and identity mismatches are
`unhealthy`, preventing accidental targeting of incompatible or wrong machines.
The command exits `1` when any selected robot is unhealthy or unreachable.

For production, replace `insecure: true` with shared mutual-TLS client settings:

```yaml
transport:
  ca_certificate: tls/agent-ca.crt
  client_certificate: tls/fleet-client.crt
  client_key: tls/fleet-client.key
```

Relative certificate paths resolve from the inventory file. Optional
`tls_server_name` on a robot supports certificate identities that differ from
the endpoint hostname.

## Health-gated deployments

A rollout file binds one digest-pinned OCI artifact to a fleet inventory and a
sequence of non-overlapping waves. See `config/rollout.example.yaml`.

Start the canary wave:

```bash
ros2 run vektor vektor deploy --config config/rollout.example.yaml
```

VEKTOR checks the selected robots before deployment, asks each agent to pull the
artifact with its configured Docker or Podman runtime, replaces the managed
container with that exact digest, verifies the observed image reference, waits
for runtime readiness and a fresh post-activation ROS health snapshot, waits
for the configured settling period, and checks fleet health again. A failed
prepare, activation, observation, or post-deploy health gate rolls the entire
wave back automatically.

A successful non-final wave pauses deliberately. Re-check the active machines
and advance the next wave with:

```bash
ros2 run vektor vektor promote --config config/rollout.example.yaml
```

Rollback every applied wave in reverse robot order:

```bash
ros2 run vektor vektor rollback --config config/rollout.example.yaml
```

Add `--format json` to any rollout command for automation. Operator rollout
progress and each agent's desired deployment are persisted atomically, so a
process restart does not silently advance a release. OCI references must use
`name@sha256:<64-hex-digest>`; mutable tags such as `latest` are rejected.
`operation_timeout_ms` bounds OCI runtime work and non-activation deployment
RPCs. When artifact trust is enabled, verification and the runtime pull share
this same deadline.
`readiness_timeout_ms` bounds both container-readiness polling and the wait for
a newly published ROS health snapshot. The activation RPC deadline covers both
windows, so a stale pre-deployment health result cannot approve a release.

The initial runtime driver manages one container, named `vektor-workload` by
default. Override the name with `--runtime-container`. A strict optional
`workload` mapping in the rollout config controls `network` (`host`, `bridge`,
or `none`), `restart_policy`, environment variables, bind mounts, device
mappings, and the command passed after the image. VEKTOR passes every value as a
separate process argument without invoking a shell. Mount and device paths must
be absolute, and duplicate targets or invalid environment names are rejected.
Do not place secrets directly in environment values because rollout and agent
state are operational configuration, not a secrets store.

VEKTOR labels containers it creates and refuses to replace or stop a same-named
container without that ownership label.
Desired and observed artifacts, the runtime container ID, ownership, running
and readiness state, reconciliation operation, attempt number, artifact
verification provenance, and drift status are persisted atomically and returned
by `GetDeployment`. For containers with an OCI health check, readiness requires
the runtime to report `healthy`; without one, a running container is considered
runtime-ready. The agent rechecks observed state after restart, during health
inspection, and when deployment state is requested. Existing deployment state
schemas 1 through 4 remain readable and are upgraded when the state is next
persisted.

Prepare, activation, and rollback persist their operation intent before
changing the runtime. After an interrupted agent process restarts, VEKTOR
inspects the actual container. A completed rollback is finalized, while an
observed interrupted activation returns to `staged` and must be retried so a
fresh ROS health gate still runs. It never silently promotes an interrupted
activation.

The current runtime implementation supports one managed container per agent.
Its workload specification is persisted alongside the artifact, and rollback
restores the previous artifact, workload settings, and verification provenance.

### Artifact trust

Install [Cosign](https://docs.sigstore.dev/cosign/system_config/installation/)
on each agent host, then pass `--trust-policy <path>` to enforce verification
before the OCI runtime can pull an artifact. Policies are strict versioned YAML;
unknown fields and mixed key/keyless settings are rejected.

The example policy uses keyless verification with an exact certificate identity
and OIDC issuer:

```yaml
schema_version: 1
mode: keyless
certificate_identity: https://github.com/vektor-robotics/vektor/.github/workflows/release.yaml@refs/heads/main
certificate_oidc_issuer: https://token.actions.githubusercontent.com
cosign_executable: cosign
timeout_ms: 30000
```

For a public key, use `mode: public_key` and `key: cosign.pub`. Relative key
paths resolve from the policy file; Cosign-supported KMS URIs can be used
directly. VEKTOR invokes `cosign verify` with the configured key or exact keyless
identity constraints. A timeout, missing signature, identity mismatch, issuer
mismatch, or invalid signature fails preparation before the container runtime
is called. Successful status records expose the verification method, signer,
issuer, and timestamp through deployment API schema v5.

## Repository layout

```text
include/vektor/       Public C++ interfaces
src/config.cpp        YAML policy loading
src/health_inspector  ROS 2 graph, frequency, TF, lifecycle checks
src/reporter.cpp      Check output and exit semantics
src/status.cpp        Fleet-ready snapshots and bounded local history
src/agent.cpp         Periodic inspection, mTLS, and gRPC service
src/fleet.cpp         Inventory, targeting, concurrent polling, aggregation
src/runtime.cpp       Versioned Docker/Podman runtime-driver implementation
src/deployment.cpp    Desired/observed state and reconciliation transactions
src/rollout.cpp       Health-gated waves, promotion, state, and rollback
proto/                Versioned fleet API contract
src/main.cpp          CLI entry point
config/example.yaml   Example health policy
config/fleet.example.yaml  Example fleet inventory
config/rollout.example.yaml  Example staged OCI rollout
test/                 GoogleTest coverage
```

The core library is intentionally independent of deployment orchestration. Future commands can add command modules and reuse `CheckConfig`, `HealthInspector`, and the result/reporting types.

## Ubuntu 24.04 / ROS 2 Jazzy

Install the ROS dependencies in a sourced Jazzy shell:

```bash
sudo apt update
sudo apt install -y ros-jazzy-rclcpp ros-jazzy-rclcpp-action \
  ros-jazzy-tf2-ros ros-jazzy-lifecycle-msgs libyaml-cpp-dev \
  ros-jazzy-ament-cmake-gtest libprotobuf-dev protobuf-compiler \
  libgrpc++-dev protobuf-compiler-grpc
source /opt/ros/jazzy/setup.bash
```

Build from the directory containing this repository as a colcon workspace package:

```bash
mkdir -p ~/vektor_ws/src
cp -r . ~/vektor_ws/src/vektor
cd ~/vektor_ws
source /opt/ros/jazzy/setup.bash
rosdep install --from-paths src --ignore-src -r -y --skip-keys grpc
colcon build --symlink-install --cmake-args -DCMAKE_BUILD_TYPE=RelWithDebInfo
source install/setup.bash
```

Run against a live ROS graph:

```bash
ros2 run vektor vektor check --config ~/vektor_ws/src/vektor/config/example.yaml
```

Use JSON output in scripts or deployment gates:

```bash
ros2 run vektor vektor check --config config/example.yaml --format json
```

### Live self-test

Start the standard ROS 2 talker in one terminal:

```bash
source /opt/ros/jazzy/setup.bash
ros2 run demo_nodes_cpp talker
```

Then run VEKTOR in another terminal:

```bash
cd /mnt/c/Users/maxle/OneDrive/Documents/vektor
source /opt/ros/jazzy/setup.bash
source install/setup.bash
ros2 run vektor vektor check --config config/talker.yaml
```

The node and topic-frequency checks should pass. Node names in policy may be bare
(`talker`) or fully qualified (`/robot_1/talker`); VEKTOR normalizes both forms.

### Policy options

The optional top-level `robot_id` identifies the machine, and
`discovery_timeout_ms` controls how long a new inspector waits for DDS graph
discovery. Topic mappings accept
`min_frequency_hz`, `max_frequency_hz`, `sample_window_ms`,
`reliability` (`system_default`, `reliable`, or `best_effort`), and `qos_depth`.
TF mappings accept `timeout_ms`. Lifecycle mappings accept `state`,
`service_timeout_ms`, and `request_timeout_ms`. Unknown top-level fields and
invalid values are rejected before any ROS checks run.

Run tests:

```bash
colcon test --packages-select vektor
colcon test-result --verbose
```

## Roadmap

Milestones 1–5 provide checks, status snapshots, the mutually authenticated
machine agent, fleet aggregation, and health-gated OCI rollouts. VEKTOR has now
completed v0.6 Reconcile: applying the desired artifact to the actual machine
runtime, verifying the observed digest and health, detecting drift, and safely
recovering or rolling back after failures. The current focus is v0.7 Trust:
artifact identity, policy enforcement, provenance, and auditability. Access
control and production hardening follow in v0.8–v0.9. See `ROADMAP.md`.

## Contributing and security

Contributions are welcome. See `CONTRIBUTING.md` for the development workflow and
`SECURITY.md` for private vulnerability reporting guidance.

## License

Apache-2.0. See `package.xml` for package metadata.
