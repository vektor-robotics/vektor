# VEKTOR

Safe software delivery for autonomous machines.

VEKTOR is an open-source, health-aware deployment tool for ROS 2 fleets.
`vektor check` validates a live ROS graph, `vektor status` produces a
timestamped machine-health snapshot, `vektor agent` serves continuously updated
snapshots over gRPC, and `vektor fleet` aggregates health across selected
machines.

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
  --listen 127.0.0.1:50051 --interval-ms 5000 --insecure
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

## Repository layout

```text
include/vektor/       Public C++ interfaces
src/config.cpp        YAML policy loading
src/health_inspector  ROS 2 graph, frequency, TF, lifecycle checks
src/reporter.cpp      Check output and exit semantics
src/status.cpp        Fleet-ready snapshots and bounded local history
src/agent.cpp         Periodic inspection, mTLS, and gRPC service
src/fleet.cpp         Inventory, targeting, concurrent polling, aggregation
proto/                Versioned fleet API contract
src/main.cpp          CLI entry point
config/example.yaml   Example health policy
config/fleet.example.yaml  Example fleet inventory
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

Milestones 1–4 provide checks, status snapshots, the mutually authenticated
machine agent, and fleet aggregation. The next milestone adds progressive OCI
deployment, promotion, pause, and rollback. See `ROADMAP.md`.

## Contributing and security

Contributions are welcome. See `CONTRIBUTING.md` for the development workflow and
`SECURITY.md` for private vulnerability reporting guidance.

## License

Apache-2.0. See `package.xml` for package metadata.
