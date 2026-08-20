#!/usr/bin/env bash
# Run the real runtime-driver contract against a disposable digest-pinned OCI workload.
set -eo pipefail

runtime="${1:-docker}"
image="${VEKTOR_TEST_OCI_IMAGE:-alpine:3.20}"
ros_setup="${VEKTOR_ROS_SETUP:-/opt/ros/jazzy/setup.bash}"

command -v "$runtime" >/dev/null || { echo "OCI runtime is unavailable: $runtime" >&2; exit 2; }
[[ -f "$ros_setup" ]] || { echo "ROS setup script does not exist: $ros_setup" >&2; exit 2; }

"$runtime" pull "$image"
artifact=$("$runtime" image inspect --format '{{index .RepoDigests 0}}' "$image")
[[ -n "$artifact" && "$artifact" == *@sha256:* ]] || { echo "could not resolve digest-pinned artifact" >&2; exit 2; }

export VEKTOR_TEST_OCI_RUNTIME="$runtime"
export VEKTOR_TEST_OCI_ARTIFACT="$artifact"
# shellcheck disable=SC1090
source "$ros_setup"
colcon build --packages-select vektor --cmake-args -DVEKTOR_ENABLE_OCI_RUNTIME_INTEGRATION_TESTS=ON
source install/setup.bash
set -u
colcon test --packages-select vektor --ctest-args -R '^test_runtime_integration$'
colcon test-result --verbose
