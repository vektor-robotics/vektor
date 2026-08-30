# Validation vertical-slice testbed

This qualification testbed exercises the v1.3 Validate workflow on a bounded,
reproducible ROS 2 publisher. It captures one baseline and two controlled
candidate runs, compares the baseline with the fast candidate, replays the
baseline bag in an isolated ROS domain, verifies that every source message was
observed on the remapped replay topic, and ranks both candidates with an
explicit experiment policy.

The testbed is intentionally small. It validates VEKTOR's evidence plumbing and
deterministic ranking, not navigation quality or physical-world correlation.
It explicitly uses subnet discovery because Fast DDS in the qualified Jazzy/WSL
environment does not exchange rosbag2 traffic in localhost-only mode. Run it
only on a trusted isolated test network or behind an appropriate firewall.

## Prerequisites

- Ubuntu 24.04 with ROS 2 Jazzy
- `rosbag2`, `std_msgs`, Python 3, and Git
- a locally built VEKTOR workspace

## Run

Source ROS 2 and the built workspace, then provide a new output directory:

```bash
source /opt/ros/jazzy/setup.bash
source install/setup.bash
scripts/run-validation-testbed.sh /tmp/vektor-validation-evidence
```

Set `VEKTOR_EXECUTABLE` when testing a different binary. The output directory
must not already exist, which prevents accidental evidence replacement.

## Retained evidence

The output contains the exact run, replay, experiment, and policy definitions;
all three candidate run manifests, a replay-observation run, their rosbag2
artifacts, bag metadata, command JSON, publisher logs, comparison, replay, and
experiment manifests, and `summary.json`.

The qualification succeeds only when:

- the controlled candidate change produces a comparison difference;
- rosbag2 replay completes and all 15 source messages are observed after the
  declared topic remap;
- the explicit policy ranks `fast-publisher` first; and
- the experiment manifest keeps automatic deployment disabled.

`summary.json` labels the evidence scope as simulated ROS 2 only. Do not use
this result to check the roadmap's full evidence item: that item also requires
retained physical-correlation evidence. A suitable follow-up must run the same
declared candidates on a physical or production-equivalent robot, retain
offline and physical metric/rank results, and explain every agreement and
disagreement.
