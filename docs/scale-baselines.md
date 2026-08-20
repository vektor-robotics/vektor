# Fleet and rollout scale baselines

Capture v0.9 measurements on Ubuntu 24.04 with ROS 2 Jazzy and a disposable,
dedicated test fleet. The objective is to establish a release-candidate
baseline, not to set a universal latency target: network RTT, agent hardware,
and OCI registry locality materially affect the result.

Build and source the workspace, then record at least 20 polling samples for
each inventory size and concurrency cap being qualified. The harness preserves
all samples in CSV and stops at the first failed command.

```bash
source /opt/ros/jazzy/setup.bash
source install/setup.bash
bash scripts/run-performance-baseline.sh results/fleet-100-c32.csv 20 -- \
  ros2 run vektor vektor fleet --config config/fleet-100.yaml --format json
```

Repeat for the fleet sizes used in production (at minimum 25, 100, and the
largest planned inventory) and record `max_concurrent_requests`, request
timeout, transport mode, hardware, and network topology alongside the CSV.
Keep the original JSON reports so unreachable robots and snapshot age failures
are visible rather than hidden in a latency aggregate.

For rollout orchestration, use an isolated registry, disposable state paths,
and a non-production ROS domain. Measure a complete canary and a bounded
larger wave with the same harness, for example:

```bash
bash scripts/run-performance-baseline.sh results/rollout-25-c8.csv 10 -- \
  ros2 run vektor vektor deploy --config config/rollout-25.yaml --format json
```

Reset the rollout state and agent deployment state between samples so each
sample exercises preparation, activation, observation, and health gating. Do
not run this command against production workloads. A release-candidate evidence
record includes the CSV files, configuration digests with secrets removed,
command output, agent audit logs, and the median plus worst successful sample.

Fleet polling and rollout RPC fan-out are limited by
`max_concurrent_requests`; reports retain inventory order regardless of that
limit. Qualify at least the intended production cap and a cap of one to verify
the bounded sequential path.
