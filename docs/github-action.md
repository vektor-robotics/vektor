# GitHub Action

Use the VEKTOR GitHub Action to run robot-run validation evidence in CI and
attach the retained evidence to a pull request or workflow run.

The first public path is intentionally low-risk: run the built-in ROS 2
validation testbed in a `ros:jazzy-ros-base-noble` container. This proves that
VEKTOR can build, capture runs, compare a controlled candidate, replay a bag on
an isolated ROS domain, score candidates, and publish a job summary. It is
simulated ROS 2 evidence only; it does not claim physical correlation.

## Built-in Testbed

```yaml
name: VEKTOR Robot Validation

on:
  pull_request:
  workflow_dispatch:

jobs:
  validate:
    runs-on: ubuntu-24.04
    container: ros:jazzy-ros-base-noble

    steps:
      - uses: actions/checkout@v4

      - name: Run VEKTOR validation
        uses: vektor-robotics/vektor@main
        with:
          mode: testbed
          artifact-name: vektor-validation-evidence
```

The Action writes a Markdown summary to the GitHub job summary and uploads the
complete evidence directory as an artifact.

A minimal public demo repository is available at
`https://github.com/nahid633/vektor-action-demo`. It runs the built-in testbed
from a separate repository and proves that an external project can consume
VEKTOR through GitHub Actions.

## Custom Command

Use `mode: custom` when a repository already has a simulator, test publisher,
or saved run definitions. The custom command runs from the caller repository
with these environment variables available:

- `VEKTOR_EXECUTABLE`: path to the built VEKTOR CLI
- `VEKTOR_ACTION_EVIDENCE_DIR`: directory to place retained evidence

```yaml
name: Robot Behavior Check

on:
  pull_request:

jobs:
  validate:
    runs-on: ubuntu-24.04
    container: ros:jazzy-ros-base-noble

    steps:
      - uses: actions/checkout@v4

      - name: Run project validation
        uses: vektor-robotics/vektor@main
        with:
          mode: custom
          command: |
            "$VEKTOR_EXECUTABLE" validate --type health \
              --config .vektor/health.yaml
            mkdir -p "$VEKTOR_ACTION_EVIDENCE_DIR"
```

For early external testers, prefer a small deterministic ROS 2 publisher,
offline rosbag replay, or report-only validation before connecting CI to any
physical robot. Physical robots should remain operator-supervised and should
not be reachable from replay jobs.

## Inputs

| Input | Default | Description |
|---|---|---|
| `mode` | `testbed` | `testbed` runs the built-in validation testbed; `custom` runs `command`. |
| `evidence-directory` | runner temp path | Directory for retained VEKTOR evidence. |
| `install-dependencies` | `true` | Installs ROS build dependencies with `apt` and `rosdep`. |
| `build` | `true` | Builds VEKTOR before running validation. |
| `command` | empty | Shell command for `mode: custom`. |
| `artifact-name` | `vektor-evidence` | Uploaded artifact name. |
| `upload-artifact` | `true` | Uploads the evidence directory with `actions/upload-artifact`. |

## Expected PR Signal

A good first PR signal is concise:

```text
VEKTOR Robot Validation: Needs Review

The candidate completed the task, but elapsed time changed by +4.0 s.

Baseline: validation-baseline-001
Candidate: validation-candidate-slow-001
Safety stops: 0
Replay: completed
Evidence: uploaded as vektor-validation-evidence
Decision: review before merge
```

This keeps VEKTOR aligned with its current scope: Git-style evidence for robot
runs, not robot control and not safety certification.
