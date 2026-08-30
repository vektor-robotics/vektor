# VEKTOR

Safe software delivery for autonomous machines.

VEKTOR is an open-source, health-aware deployment tool for ROS 2 fleets.
`vektor check` validates a live ROS graph, `vektor status` produces a
timestamped machine-health snapshot, `vektor agent` serves health and deployment
operations over gRPC, `vektor fleet` aggregates selected machines, and the
deployment commands run health-gated OCI rollouts with durable audit events.

## Milestone 1 checks

- Required node presence
- Topic existence
- Topic frequency over a configurable sampling window
- Per-topic reliable or best-effort QoS
- TF connectivity between two frames
- Managed-node lifecycle state through `/<node>/get_state`
- Text or machine-readable JSON output

The check exits `0` only when every configured check passes, `1` when a health check fails, and `2` for invalid CLI/configuration errors.

## Offline configuration validation

Validate configuration before installing it on a robot or reloading agent
policy. This command uses VEKTOR's strict parsers but does not contact ROS,
agents, registries, or an OCI runtime.

```bash
vektor validate --type health --config config/example.yaml
vektor validate --type fleet --config config/fleet.example.yaml --format json
vektor validate --type rollout --config config/rollout.example.yaml
vektor validate --type authorization --config config/authorization.example.yaml
vektor validate --type approval-policy --config config/approval-policy.example.yaml
vektor validate --type approvals --config config/approvals.example.yaml
vektor validate --type trust --config config/trust.example.yaml
```

JSON output is schema 1 and contains `valid` and `type`; validation failures
return exit code 2 with the parser's field-specific error.

## Validation run manifests

Start a local validation run from a versioned definition, inspect its exact
provenance, and close it with an outcome and optional annotations:

```bash
vektor capture start --config config/run.example.yaml --format json
vektor capture show --run-id navigation-baseline-001
vektor capture stop --run-id navigation-baseline-001 --outcome passed \
  --annotation "no localization drift" --metric goal_error_m=0.125
vektor capture export --run-id navigation-baseline-001 \
  --output /tmp/navigation-baseline-001
vektor compare --baseline navigation-baseline-001 \
  --candidate navigation-candidate-001 --format json
```

Run manifests use schema version 1 and bind a run to a digest-pinned OCI
artifact, workload, VEKTOR policy path and computed SHA-256 fingerprint,
flattened ROS parameter snapshot, environment metadata, robot, operator, and
UTC start/stop timestamps. A definition selects 1–64 explicit ROS topics and a
rosbag2 storage plugin. `capture start` launches `ros2 bag record` without a
shell; `capture stop` sends bounded shutdown signals and records the resulting
bag directory as an external artifact with a deterministic SHA-256 fingerprint
and byte count. Manifests stay under `.vektor/runs` and bags under
`.vektor/artifacts`; use `--state-dir` to relocate the state root. `capture export`
writes portable YAML and JSON metadata without copying unbounded bag
data. Each run definition also identifies the bounded health-history and
append-only deployment-audit sources. At start, VEKTOR records the audit byte
offset; at stop, it imports only new deployment events and health-state
transitions timestamped within the run. Reads are capped at 1 MiB per source
and the combined imported-event limit is configurable from 1 to 1,024
(default 256). Source rotation, truncation, and malformed records become
manifest warning events instead of making sensor or log data unbounded.

`capture stop --metric name=value` persists up to 256 finite numeric metrics
using stable names. `vektor compare` requires two completed runs and emits
deterministically ordered text or schema-versioned JSON differences for
outcome, parameters, metrics, and events. Metric entries include candidate
minus baseline deltas when both values exist. Event comparison aggregates by
type and message and compares counts, deliberately ignoring timestamps so
equivalent events recorded at different times do not appear as changes.

## Offline replay adapters

Replay a completed, fingerprint-verified capture through rosbag2 in an isolated
ROS domain:

```bash
vektor replay execute --config config/replay.example.yaml --format json
```

Replay definitions use schema version 1 and identify a unique replay, its
completed source run, an adapter, a required ROS domain ID from 1–232, a bounded
timeout, optional topic remaps, and optional rosbag2 QoS overrides. The rosbag2
adapter always publishes simulated time with `--clock`, disables keyboard
controls, and executes without a shell.

The `simulator` adapter launches an operator-selected executable directly. Its
bounded argument list may use `${bag_path}`, `${source_run_id}`, `${replay_id}`,
`${ros_domain_id}`, and `${output_dir}` placeholders. See
`config/replay.simulator.example.yaml`. VEKTOR sets `ROS_DOMAIN_ID` for the
adapter process and forces `ROS_LOCALHOST_ONLY=1`, redirects its output to a
private log, signals the entire adapter process group on timeout, and never
issues actuator commands itself.

Before launch, VEKTOR requires a completed source run with a rosbag2 artifact
whose current SHA-256 fingerprint and byte count match the run manifest. Replay
manifests are stored under `.vektor/replays` by default and retain the source
OCI artifact, bag fingerprint, adapter version, exact argument vector, topic
remaps, domain, localhost-only isolation, timestamps, status, exit code, and log
path. Adapter failure or timeout returns exit code 1; invalid configuration or
provenance returns 2.

## Redacted support bundle

Create a fresh support directory with version metadata and a SHA-256 fingerprint
of the health configuration. The command deliberately excludes configuration
contents, certificates, private keys, trust/approval files, and audit payloads.

```bash
vektor support-bundle --config /etc/vektor/policy.yaml --history /var/lib/vektor/status.jsonl \
  --metrics /var/lib/vektor/metrics.prom --output /tmp/vektor-support
```

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
  --audit-log .vektor/robot-001-audit.jsonl \
  --trust-policy config/trust.example.yaml
```

Any non-loopback deployment requires mutual TLS. The server certificate and key
identify the agent; `--tls-ca` is the CA used to authenticate fleet clients:

```bash
ros2 run vektor vektor agent --config /etc/vektor/policy.yaml \
  --listen 0.0.0.0:50051 \
  --tls-cert /etc/vektor/tls/agent.crt \
  --tls-key /etc/vektor/tls/agent.key \
  --tls-ca /etc/vektor/tls/client-ca.crt \
  --authorization-policy /etc/vektor/authorization.yaml \
  --fleet-id warehouse-prod --workload-id picker
```

The first request may return gRPC `UNAVAILABLE` until initial ROS discovery and
inspection finish. The agent continues writing the bounded local status history
while fleet connectivity is unavailable.

### Role-based authorization

Pass `--authorization-policy <path>` to map authenticated client-certificate
identities to fixed roles. Authorization policies require mutual TLS, are strict
versioned YAML, and deny identities not listed in the policy. The identity value
must exactly match the peer identity selected by gRPC from the verified client
certificate; audit records show the same value prefixed with `mtls:`.

```yaml
schema_version: 2
identities:
  - identity: release-manager
    roles: [deployer]
    scopes:
      fleets: [warehouse-prod]
      workloads: [picker]
  - identity: fleet-operator
    roles: [operator]
    scopes:
      fleets: [warehouse-prod]
      workloads: ['*']
  - identity: fleet-observer
    roles: [viewer]
    scopes:
      fleets: [warehouse-prod]
      workloads: ['*']
```

| Role | Inspect | Deploy | Promote | Roll back |
| --- | --- | --- | --- | --- |
| `viewer` | yes | no | no | no |
| `deployer` | yes | yes | yes | no |
| `operator` | yes | yes | yes | yes |
| `admin` | yes | yes | yes | yes |

`GetStatus`, `WatchStatus`, and `GetDeployment` require `inspect`;
`PrepareDeployment`, `ActivateDeployment`, and `RollbackDeployment` require
`deploy`, `promote`, and `rollback` respectively. A denial returns gRPC
`PERMISSION_DENIED` before runtime mutation and a stable error body such as:

```json
{"schema_version":1,"code":"VEKTOR_AUTHORIZATION_DENIED","action":"deploy"}
```

Denied requests are recorded as append-only `authorization.<action>` audit
events. Schema 2 also requires each identity to declare non-empty `fleets` and
`workloads` scopes. Values match exact fleet and workload IDs; use `'*'` only
when deliberately granting every ID. Fleet status requests require a fleet
match, while deployment inspection and mutation require both matches. The fleet
ID comes from the fleet inventory and `workload_id` comes from rollout YAML.
The agent must also be started with matching `--fleet-id` and `--workload-id`
values whenever authorization is enabled. These server-bound IDs prevent a
client from gaining access by claiming a different scope in its request.

Schema-1 authorization files remain supported as role-only policies for a
controlled migration. Omitting `--authorization-policy` preserves the pre-v0.8
behavior while operators migrate existing installations. See
`config/authorization.example.yaml` for a complete example.

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

Fleet requests run concurrently with the configured per-agent deadline, bounded
by `max_concurrent_requests` (default `32`) to keep large inventories from
creating one client thread per robot. Results remain in inventory order. A robot
is reported as `unreachable` when its RPC fails, times out, or returns a snapshot
older than `max_snapshot_age_ms`. Unsupported schemas and identity mismatches are
`unhealthy`, preventing accidental targeting of incompatible or wrong machines.
The same limit bounds concurrent rollout RPCs for preparation, activation,
observation, and rollback.
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
the endpoint hostname. See the
[credential and certificate rotation runbook](docs/credential-rotation.md)
before renewing a leaf, changing an identity, or replacing a CA.

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

### Signed rollout approvals

Set `environment`, `approval_policy`, and `approval_file` in rollout YAML to
gate sensitive environments or waves larger than the policy threshold. The
policy lists trusted approver public keys, the number of distinct signatures
required, and the maximum approval lifetime. Relative paths resolve from the
file that declares them. See `config/approval-policy.example.yaml` and
`config/approvals.example.yaml`.

Each approval is signed over a canonical payload bound to the exact deployment
ID, OCI digest, fleet ID, workload ID, environment, wave, approver identity,
and UTC validity window. Generate that payload without exposing a private key:

```bash
ros2 run vektor vektor approval-payload \
  --config config/rollout.example.yaml --wave canary \
  --identity safety-lead \
  --issued-at 2026-08-15T10:00:00Z \
  --expires-at 2026-08-15T18:00:00Z > approval.payload
```

Sign it on the approver's machine and place the base64 result in the matching
record in the approval bundle:

```bash
openssl dgst -sha256 -sign safety-lead.private.pem approval.payload \
  | base64 -w0
```

Every required approver signs their own identity-specific payload. Duplicate,
expired, future-dated, overlong, untrusted, incorrectly bound, or invalid
signatures do not count. If the threshold is not met, VEKTOR returns
`VEKTOR_APPROVAL_REQUIRED` before target-wave polling or agent mutation.
Automatic rollback never requires approval. Protect the rollout config,
approval policy, and approver public keys with the control-plane host's file
permissions; these files define the approval trust boundary.

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
See the [rollout soak and fault-injection guide](docs/soak-testing.md) for the
repeatable multi-day release-candidate campaign.
For agent replacement and incident handling, use the
[upgrade, downgrade, backup, and recovery runbook](docs/upgrade-recovery.md).

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

Plain-HTTP registries and signatures without transparency-log verification are
disabled by default. A self-managed public-key policy can opt into
`allow_http_registry: true` and `ignore_transparency_log: true` for an isolated
test or private registry. VEKTOR rejects both exceptions in keyless mode; do not
use them for internet-facing production registries.

CI also exercises Cosign against a real ephemeral OCI registry. To run that
suite yourself, provide a registry and Cosign, prepare the fixtures, then enable
the integration target:

```bash
VEKTOR_TEST_REGISTRY=registry:5000 \
  bash scripts/prepare-trust-integration.sh /tmp/vektor-trust.env
source /tmp/vektor-trust.env
colcon build --cmake-args -DVEKTOR_ENABLE_TRUST_INTEGRATION_TESTS=ON
colcon test --packages-select vektor --ctest-args -R test_trust_integration
colcon test-result --verbose
```

The suite proves that a trusted signed digest succeeds while an unsigned
digest, a trusted signature checked with the wrong key, and a tag moved to new
content after signing all fail closed. Deployment commands still require digest
pins; the tag-tampering case independently checks Cosign's content binding.

### Deployment audit log

Every agent writes append-only JSON Lines audit records to
`.vektor/audit.jsonl` by default. Set a machine-specific destination with
`--audit-log <path>`. VEKTOR opens the file in append-only mode, creates it with
owner-only permissions, writes one complete JSON object per event, and syncs
each record to durable storage. It never truncates or rewrites existing audit
records.

Events distinguish operator actions such as `deployment.prepare`,
`deployment.activate`, and `deployment.rollback` from agent decisions such as
`artifact.verify`, `runtime.prepare`, `runtime.drift`, and
`deployment.recover`. Each record includes schema version, UTC timestamp,
actor, action, outcome, deployment ID, artifact, phase, reconciliation
operation, and message. With mutual TLS, the actor is the authenticated client
certificate identity. Loopback development clients are explicitly recorded as
`unauthenticated:<peer>`.

```json
{"schema_version":1,"timestamp":"2026-08-15T00:00:00Z","actor":"mtls:release-manager","action":"deployment.prepare","outcome":"started","deployment_id":"release-42","artifact":"ghcr.io/example/robot@sha256:...","phase":"active","operation":"none","message":""}
```

An audit append failure prevents a new runtime mutation from starting. Ship or
rotate the JSONL file using host-level tooling appropriate for the deployment;
VEKTOR reopens the configured path for every event, so renaming a rotated file
does not require an agent restart.

## Repository layout

```text
include/vektor/       Public C++ interfaces
src/config.cpp        YAML policy loading
src/health_inspector  ROS 2 graph, frequency, TF, lifecycle checks
src/reporter.cpp      Check output and exit semantics
src/status.cpp        Fleet-ready snapshots and bounded local history
src/agent.cpp         Periodic inspection, mTLS, and gRPC service
src/audit.cpp         Durable append-only deployment event records
src/approval.cpp      Signed rollout approval policy and verification
src/fleet.cpp         Inventory, targeting, concurrent polling, aggregation
src/runtime.cpp       Versioned Docker/Podman runtime-driver implementation
src/deployment.cpp    Desired/observed state and reconciliation transactions
src/rollout.cpp       Health-gated waves, promotion, state, and rollback
proto/                Versioned fleet API contract
src/main.cpp          CLI entry point
config/example.yaml   Example health policy
config/fleet.example.yaml  Example fleet inventory
config/rollout.example.yaml  Example staged OCI rollout
config/approval-policy.example.yaml  Example rollout approval policy
docs/credential-rotation.md  Operator credential and certificate rotation runbook
test/                 GoogleTest coverage
```

The core library is intentionally independent of deployment orchestration. Future commands can add command modules and reuse `CheckConfig`, `HealthInspector`, and the result/reporting types.

## Ubuntu 24.04 / ROS 2 Jazzy

### Ubuntu package and managed agent

The `debian/` manifest builds a versioned native Ubuntu 24.04 package in a
sourced ROS 2 Jazzy environment:

```bash
sudo apt install -y build-essential debhelper devscripts
dpkg-buildpackage -us -uc -b
sudo apt install ../vektor_1.0.0-1_amd64.deb
```

Installation creates the `vektor` service account and persistent state and log
directories at `/var/lib/vektor` and `/var/log/vektor`. It installs, but does
not enable or start, `vektor-agent.service`. Before enabling it, replace the
four root-owned, group-readable files in `/etc/vektor` and install the mTLS
certificate, private key, and client CA under `/etc/vektor/tls`. The service
uses those fixed paths for policy, trust, authorization, status history,
deployment state, and audit records. It runs with systemd filesystem and
privilege sandboxing; Docker support deliberately grants membership in the
local `docker` group, which is privileged access and must be treated like root
access on the host.

The package does not choose or download a Cosign binary. Install a supported
Cosign release using your organization’s approved software source and ensure
`cosign` is on the service account’s `PATH` before enabling the agent.

```bash
sudo systemctl enable --now vektor-agent.service
sudo systemctl status vektor-agent.service
```

After replacing `/etc/vektor/policy.yaml`, validate it and request a
transactional reload. VEKTOR retains the last known-good health policy if the
replacement is invalid or cannot be audited; each attempt is recorded as a
`policy.reload` audit event.

```bash
sudo /usr/lib/vektor/vektor validate --type health --config /etc/vektor/policy.yaml
sudo systemctl reload vektor-agent.service
```

The service remains inactive until its health policy and all three mTLS files
exist. Validate every YAML file offline before replacing it:

```bash
sudo /usr/lib/vektor/vektor validate --type health --config /etc/vektor/policy.yaml
sudo /usr/lib/vektor/vektor validate --type trust --config /etc/vektor/trust.yaml
sudo /usr/lib/vektor/vektor validate --type authorization --config /etc/vektor/authorization.yaml
```

The agent writes fixed-cardinality Prometheus text metrics to
`/var/lib/vektor/metrics.prom` (override with `--metrics`). Collect that file
with a host textfile collector; it contains health inspections, authorization
denials, reconciliation outcomes, and RPC counters/latency totals without
robot, certificate, or deployment identifiers as labels.

Install the ROS dependencies in a sourced Jazzy shell:

```bash
sudo apt update
sudo apt install -y ros-jazzy-rclcpp ros-jazzy-rclcpp-action \
  ros-jazzy-tf2-ros ros-jazzy-lifecycle-msgs libyaml-cpp-dev \
  ros-jazzy-ament-cmake-gtest libprotobuf-dev protobuf-compiler \
  libgrpc++-dev protobuf-compiler-grpc libssl-dev
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
machine agent, fleet aggregation, and health-gated OCI rollouts. VEKTOR has
completed v0.6 Reconcile, v0.7 Trust, v0.8 Control, v0.9 Harden, the v1.0
compatibility engineering milestone, v1.1 Operate, and v1.2 Multi-workload. The
current product focus is v1.3 Validate: capture exact deployment runs, compare
their parameters and outcomes, replay them in configured offline environments,
and score candidate configurations. Customer interviews and design-partner
evaluation are milestone gates. Learned validation environments remain a
conditional research track, not a current product claim. Release qualification
evidence remains a separate gate before the `v1.0.0` tag. See `ROADMAP.md`.

## Contributing and security

Contributions are welcome. See `CONTRIBUTING.md` for the development workflow,
`SECURITY.md` for private vulnerability reporting guidance, and
`docs/credential-rotation.md` for operational key and certificate rotation.
For release qualification, see the [soak-testing](docs/soak-testing.md),
[scale-baseline](docs/scale-baselines.md), and
[upgrade/recovery](docs/upgrade-recovery.md) runbooks. The stable public
interface commitments are documented in the [v1 compatibility
contract](docs/compatibility.md). The supported platform, versioning rules, and
release procedure are in the [v1 release policy](docs/release-policy.md).

## License

Apache-2.0. See `package.xml` for package metadata.
