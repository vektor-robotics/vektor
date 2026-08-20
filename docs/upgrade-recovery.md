# Upgrade, downgrade, backup, and recovery

Use this runbook for VEKTOR agents on Ubuntu 24.04 with ROS 2 Jazzy. Schedule
one agent at a time and verify fleet health before advancing to the next robot.
The examples use the default agent paths; substitute the explicit paths passed
through `--deployment-state`, `--audit-log`, `--history`, and the rollout
configuration's `state_file` when your service uses different locations.

## Before an upgrade

1. Record the current VEKTOR package version or commit, OCI runtime version,
   active deployment digest, and agent command-line options.
2. Confirm the target agent is healthy with `vektor fleet` and that no rollout
   is actively preparing, activating, or rolling back it.
3. Stop the agent cleanly. A clean stop prevents a normal upgrade from being
   treated as an interrupted reconciliation operation.
4. Make a restricted backup on the same machine, outside the agent state
   directory. Preserve the deployment state, audit log, status history, rollout
   state, and every referenced policy, certificate, and private key.

```bash
backup_dir=/var/lib/vektor-backups/$(date -u +%Y%m%dT%H%M%SZ)
sudo install -d -m 0700 "$backup_dir"
sudo cp -a .vektor/deployment.yaml .vektor/audit.jsonl "$backup_dir/"
sudo cp -a .vektor/status-history.jsonl "$backup_dir/" 2>/dev/null || true
sudo cp -a /etc/vektor "$backup_dir/config"
sudo sha256sum "$backup_dir"/* "$backup_dir"/config/* > "$backup_dir/SHA256SUMS"
```

Treat these backups as sensitive: they can include certificates, private keys,
authorization policy, approval material, and operational history. Encrypt them
at rest and restrict access to the VEKTOR service operator.

## Upgrade procedure

1. Install the new VEKTOR package or build without changing the agent's runtime
   container name, deployment-state path, or policy paths.
2. Start the agent with the same options and wait for its first health snapshot.
   At startup VEKTOR re-observes any persisted deployment. If an activation was
   interrupted, it deliberately returns the record to `staged`; explicitly
   retry activation after the normal ROS health gate rather than treating it as
   promoted.
3. Run `vektor fleet --config <fleet.yaml>` and inspect `GetDeployment` or the
   rollout report for the upgraded robot. Verify the observed artifact digest,
   workload fingerprint, runtime readiness, and absence of drift.
4. Continue one robot at a time. Do not advance a rollout wave while an agent
   reports `failed`, `staged`, an in-progress reconciliation operation, or
   runtime drift.

Deployment-state schemas 1 through 4 are readable by the current agent and are
rewritten as schema 5 when state next persists. Retain the pre-upgrade backup
until the fleet has completed its post-upgrade observation and a canary rollout
has passed.

For a release candidate, run the real runtime-driver qualification on the same
Ubuntu 24.04 / Jazzy image used by the target fleet:

```bash
bash scripts/qualify-oci-runtime.sh docker
# Run this as well when Podman is a supported target runtime.
bash scripts/qualify-oci-runtime.sh podman
```

## Downgrade procedure

An older VEKTOR binary may not understand a newer persisted deployment schema.
Do not start a downgrade binary against state written by the newer binary.

1. Stop the newer agent.
2. Restore the matching pre-upgrade deployment state, rollout state, policies,
   and service configuration from the backup. Keep the newer audit log as
   evidence; do not truncate or rewrite it.
3. Install and start the previous VEKTOR version with exactly the saved runtime
   and state paths.
4. Verify the running container digest and ROS health before resuming any
   rollout. If the observed runtime does not match the restored state, treat it
   as drift and use an explicit rollback or recovery rollout.

If no compatible pre-upgrade state is available, keep the newer agent version
installed and recover forward; do not edit the YAML schema by hand.

## Recovery after a failed upgrade

1. Stop further rollout promotion and preserve the first failing deployment
   state, audit log, status history, system journal, and OCI runtime logs.
2. If the agent is running, query deployment status. A `staged` result after an
   interrupted activation requires an explicit retry; a `failed` result should
   be rolled back using the deployment or rollout command.
3. If the agent cannot start because of a bad binary or incompatible state,
   follow the downgrade procedure and restore the matching backup.
4. Re-run fleet health and verify the exact digest on every affected robot
   before permitting a new promotion.

Never delete deployment state merely to make an agent start. That discards the
rollback target and breaks the audit trail needed to diagnose the incident.
