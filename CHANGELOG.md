# Changelog

All notable changes to VEKTOR are documented here.

## [0.2.0] - 2026-08-14

### Added

- `vektor status` health snapshots with robot, host, time, and ROS domain metadata
- Healthy, degraded, unhealthy, and unreachable aggregate states
- Text and schema-versioned JSON status output
- Continuous `--watch` mode with configurable intervals
- Bounded local JSONL status history
- Total and per-check duration measurements

## [0.1.0] - 2026-08-14

### Added

- `vektor check` for ROS nodes, topics, TF, and lifecycle states
- YAML policies with topic QoS and timeout controls
- Text and JSON check output
- ROS 2 Jazzy integration tests and GitHub Actions CI
