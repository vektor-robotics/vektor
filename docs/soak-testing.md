# Rollout soak and fault-injection testing

Run this campaign against Ubuntu 24.04 with ROS 2 Jazzy after building the
workspace. It repeatedly executes the rollout integration suite,
which includes a bounded 12-cycle crash-recovery test. Each cycle snapshots the
state after runtime activation but before the final deployment record is saved,
restarts the agent, verifies that VEKTOR returns to `staged` rather than silently
promoting, retries activation, and verifies a subsequent clean restart.

Start with one hour on a two-agent test fleet:

```bash
bash scripts/run-rollout-soak.sh 3600
```

The script sources `/opt/ros/jazzy/setup.bash` by default. Set
`VEKTOR_ROS_SETUP` when Jazzy is installed elsewhere, and `VEKTOR_TEST_DIR` for
a non-default build directory.

For release-candidate qualification, run the same command for at least 72 hours
on dedicated test hardware. Preserve `build/vektor/Testing`, agent deployment
state, and audit logs for every failed iteration. Stop the campaign and retain
the first failing state file if any test fails; do not automatically retry past
a failure.

The campaign is intentionally separate from normal CI: each CI invocation runs
the bounded fault-injection test once, while the script provides duration-based
coverage without changing CI runtime. Before a long run, confirm that the test
fleet uses isolated registry credentials, a non-production ROS domain, and a
disposable rollout state path.

Record the start/end UTC times, host and ROS versions, commit SHA, test command,
successful iteration count, and links to retained logs in the release-candidate
evidence record. Treat an interrupted campaign as incomplete rather than as a
successful soak.
