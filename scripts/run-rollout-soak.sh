#!/usr/bin/env bash
set -eo pipefail

duration_seconds="${1:-86400}"
if ! [[ "$duration_seconds" =~ ^[1-9][0-9]*$ ]]; then
  echo "usage: $0 [positive-duration-seconds]" >&2
  exit 2
fi

ros_setup="${VEKTOR_ROS_SETUP:-/opt/ros/jazzy/setup.bash}"
if [[ ! -f "$ros_setup" ]]; then
  echo "ROS setup script does not exist: $ros_setup" >&2
  exit 2
fi
# shellcheck disable=SC1090
source "$ros_setup"
set -u

test_dir="${VEKTOR_TEST_DIR:-build/vektor}"
if [[ ! -d "$test_dir" ]]; then
  echo "VEKTOR test directory does not exist: $test_dir" >&2
  exit 2
fi

deadline=$((SECONDS + duration_seconds))
iterations=0
while (( SECONDS < deadline )); do
  ctest --test-dir "$test_dir" --output-on-failure \
    -R '^test_rollout$'
  ((++iterations))
done

echo "VEKTOR rollout soak completed: ${iterations} successful iterations"
