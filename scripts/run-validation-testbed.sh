#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <new-evidence-directory>" >&2
  exit 2
fi

script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(cd -- "${script_directory}/.." && pwd)"
fixture_directory="${repository_root}/test/validation-testbed"
evidence_directory="$1"
vektor_executable="${VEKTOR_EXECUTABLE:-${repository_root}/install/vektor/lib/vektor/vektor}"
message_count=15
capture_domain=230
active_run=""

if [[ -e "${evidence_directory}" ]]; then
  echo "evidence directory already exists: ${evidence_directory}" >&2
  exit 2
fi
if [[ ! -x "${vektor_executable}" ]]; then
  echo "VEKTOR executable is not available: ${vektor_executable}" >&2
  exit 2
fi
for command in ros2 python3 git; do
  if ! command -v "${command}" >/dev/null 2>&1; then
    echo "required command is not available: ${command}" >&2
    exit 2
  fi
done

cleanup() {
  if [[ -n "${active_run}" ]]; then
    "${vektor_executable}" capture stop --run-id "${active_run}" \
      --outcome testbed_interrupted --state-dir "${evidence_directory}/runs" \
      >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

mkdir -p "${evidence_directory}/definitions" \
  "${evidence_directory}/outputs"
cp "${fixture_directory}"/*.yaml "${evidence_directory}/definitions/"
cp "${repository_root}/config/example.yaml" \
  "${evidence_directory}/definitions/policy.yaml"
: >"${evidence_directory}/definitions/status.jsonl"
: >"${evidence_directory}/definitions/audit.jsonl"

export ROS_DOMAIN_ID="${capture_domain}"
export ROS_LOCALHOST_ONLY=1

capture_run() {
  local definition="$1"
  local run_id="$2"
  local rate="$3"
  local started_ns
  local stopped_ns
  local elapsed

  active_run="${run_id}"
  "${vektor_executable}" capture start \
    --config "${evidence_directory}/definitions/${definition}" \
    --state-dir "${evidence_directory}/runs" --format json \
    >"${evidence_directory}/outputs/${run_id}-start.json"
  started_ns="$(python3 -c 'import time; print(time.monotonic_ns())')"
  ros2 topic pub --rate "${rate}" --times "${message_count}" \
    --wait-matching-subscriptions 1 --max-wait-time-secs 10 \
    /vektor_testbed/input std_msgs/msg/String "{data: testbed}" \
    >"${evidence_directory}/outputs/${run_id}-publisher.log"
  stopped_ns="$(python3 -c 'import time; print(time.monotonic_ns())')"
  elapsed="$(python3 -c \
    "print((${stopped_ns} - ${started_ns}) / 1_000_000_000)")"
  "${vektor_executable}" capture stop --run-id "${run_id}" \
    --outcome passed --annotation "published ${message_count} bounded messages" \
    --metric "publish_rate_hz=${rate}" --metric "elapsed_time_s=${elapsed}" \
    --state-dir "${evidence_directory}/runs" --format json \
    >"${evidence_directory}/outputs/${run_id}-stop.json"
  active_run=""
  ros2 bag info "${evidence_directory}/artifacts/${run_id}/rosbag2" \
    >"${evidence_directory}/outputs/${run_id}-bag-info.txt"
}

capture_run run-baseline.yaml validation-baseline-001 10
capture_run run-slow.yaml validation-candidate-slow-001 5
capture_run run-fast.yaml validation-candidate-fast-001 20

"${vektor_executable}" compare --baseline validation-baseline-001 \
  --candidate validation-candidate-fast-001 \
  --state-dir "${evidence_directory}/runs" --format json \
  >"${evidence_directory}/outputs/comparison.json"

export ROS_DOMAIN_ID=231
unset ROS_LOCALHOST_ONLY
export ROS_AUTOMATIC_DISCOVERY_RANGE=SUBNET
active_run="validation-replay-observation-001"
"${vektor_executable}" capture start \
  --config "${evidence_directory}/definitions/run-replay-observation.yaml" \
  --state-dir "${evidence_directory}/runs" --format json \
  >"${evidence_directory}/outputs/replay-observation-start.json"
"${vektor_executable}" replay execute \
  --config "${evidence_directory}/definitions/replay.yaml" \
  --state-dir "${evidence_directory}/runs" \
  --replay-dir "${evidence_directory}/replays" --format json \
  >"${evidence_directory}/outputs/replay.json"
"${vektor_executable}" capture stop \
  --run-id validation-replay-observation-001 --outcome observed \
  --annotation "captured isolated replay output" \
  --state-dir "${evidence_directory}/runs" --format json \
  >"${evidence_directory}/outputs/replay-observation-stop.json"
active_run=""
ros2 bag info \
  "${evidence_directory}/artifacts/validation-replay-observation-001/rosbag2" \
  >"${evidence_directory}/outputs/replay-observation-bag-info.txt"
replayed_count="$(sed -n 's/^Messages:[[:space:]]*//p' \
  "${evidence_directory}/outputs/replay-observation-bag-info.txt")"
if [[ ! "${replayed_count}" =~ ^[0-9]+$ ]] || \
   (( replayed_count != message_count )); then
  echo "replay observed ${replayed_count}/${message_count} messages" >&2
  exit 1
fi

"${vektor_executable}" experiment score \
  --config "${evidence_directory}/definitions/experiment.yaml" \
  --state-dir "${evidence_directory}/runs" \
  --experiment-dir "${evidence_directory}/experiments" --format json \
  >"${evidence_directory}/outputs/experiment.json"

python3 - "${evidence_directory}" "${replayed_count}" \
  "${repository_root}" <<'PY'
import json
import os
import pathlib
import platform
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
replayed = int(sys.argv[2])
repository = pathlib.Path(sys.argv[3])
git_environment = os.environ.copy()
git_environment.update({
    "GIT_CONFIG_COUNT": "1",
    "GIT_CONFIG_KEY_0": "safe.directory",
    "GIT_CONFIG_VALUE_0": str(repository),
})

def git_output(*args):
    try:
        return subprocess.check_output(
            ["git", "-C", str(repository), *args],
            env=git_environment,
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except (FileNotFoundError, subprocess.CalledProcessError):
        return ""

comparison = json.loads((root / "outputs/comparison.json").read_text())
replay = json.loads((root / "outputs/replay.json").read_text())
experiment = json.loads((root / "outputs/experiment.json").read_text())
repository_commit = git_output("rev-parse", "HEAD") or os.environ.get(
    "GITHUB_ACTION_REF"
) or os.environ.get("GITHUB_SHA") or "unknown"
repository_dirty = False
if git_output("rev-parse", "--is-inside-work-tree") == "true":
    repository_dirty = (
        subprocess.run(
            ["git", "-C", str(repository), "diff", "--quiet", "--ignore-space-at-eol"],
            check=False,
            env=git_environment,
        ).returncode != 0
        or bool(git_output("ls-files", "--others", "--exclude-standard"))
    )
summary = {
    "schema_version": 1,
    "testbed": "bounded-ros2-publisher",
    "repository_commit": repository_commit,
    "repository_dirty": repository_dirty,
    "platform": platform.platform(),
    "capture_domain_id": 230,
    "replay_domain_id": 231,
    "replay_discovery_scope": "subnet",
    "source_messages": 15,
    "replayed_messages": replayed,
    "comparison_detected_difference": comparison["different"],
    "replay_status": replay["status"],
    "top_candidate": experiment["candidates"][0]["candidate_id"],
    "top_candidate_rank": experiment["candidates"][0]["rank"],
    "automatic_deployment": experiment["automatic_deployment"],
    "evidence_scope": "simulated ROS 2 only; no physical correlation",
}
if not summary["comparison_detected_difference"]:
    raise SystemExit("comparison did not detect the controlled change")
if summary["replay_status"] != "completed":
    raise SystemExit("replay did not complete")
if summary["top_candidate"] != "fast-publisher" or summary["top_candidate_rank"] != 1:
    raise SystemExit("experiment did not rank the expected candidate first")
if summary["automatic_deployment"]:
    raise SystemExit("experiment unexpectedly enabled deployment")
(root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
PY

trap - EXIT INT TERM
echo "VEKTOR validation testbed evidence: ${evidence_directory}"
