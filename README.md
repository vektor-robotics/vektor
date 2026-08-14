# VEKTOR

Safe software delivery for autonomous machines.

VEKTOR is an open-source, health-aware deployment tool for ROS 2 fleets.
`vektor check` validates a live ROS graph, while `vektor status` produces a
timestamped machine-health snapshot for operators, scripts, and future fleet
agents.

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

## Repository layout

```text
include/vektor/       Public C++ interfaces
src/config.cpp        YAML policy loading
src/health_inspector  ROS 2 graph, frequency, TF, lifecycle checks
src/reporter.cpp      Check output and exit semantics
src/status.cpp        Fleet-ready snapshots and bounded local history
src/main.cpp          CLI entry point
config/example.yaml   Example health policy
test/                 GoogleTest coverage
```

The core library is intentionally independent of deployment orchestration. Future commands can add command modules and reuse `CheckConfig`, `HealthInspector`, and the result/reporting types.

## Ubuntu 24.04 / ROS 2 Jazzy

Install the ROS dependencies in a sourced Jazzy shell:

```bash
sudo apt update
sudo apt install -y ros-jazzy-rclcpp ros-jazzy-rclcpp-action \
  ros-jazzy-tf2-ros ros-jazzy-lifecycle-msgs libyaml-cpp-dev \
  ros-jazzy-ament-cmake-gtest
source /opt/ros/jazzy/setup.bash
```

Build from the directory containing this repository as a colcon workspace package:

```bash
mkdir -p ~/vektor_ws/src
cp -r . ~/vektor_ws/src/vektor
cd ~/vektor_ws
source /opt/ros/jazzy/setup.bash
rosdep install --from-paths src --ignore-src -r -y
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

Milestone 2 adds status snapshots and watch mode. The next milestone introduces a
lightweight `vektor-agent` and gRPC API, followed by fleet inventory, progressive
deployment, promotion, and rollback. See `ROADMAP.md`.

## Contributing and security

Contributions are welcome. See `CONTRIBUTING.md` for the development workflow and
`SECURITY.md` for private vulnerability reporting guidance.

## License

Apache-2.0. See `package.xml` for package metadata.
