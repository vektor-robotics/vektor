#include "vektor/deployment.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace {
constexpr auto kArtifact =
    "ghcr.io/vektor-robotics/demo@sha256:"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr auto kArtifactB =
    "ghcr.io/vektor-robotics/demo@sha256:"
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

class FakeRuntime final : public vektor::RuntimeDriver {
public:
  void prepare(const std::string &artifact) override {
    ++calls;
    last_artifact = artifact;
    if (fail)
      throw std::runtime_error("simulated pull failure");
  }

  vektor::RuntimeObservation
  activate(const std::string &artifact,
           const vektor::WorkloadSpec &workload) override {
    ++activate_calls;
    last_workload = workload;
    if (fail_activate)
      throw std::runtime_error("simulated activation failure");
    observation = {true, artifact, "container-123", true,
                   vektor::workload_fingerprint(workload)};
    return observation;
  }

  vektor::RuntimeObservation stop() override {
    ++stop_calls;
    observation = {};
    return observation;
  }

  vektor::RuntimeObservation inspect() override { return observation; }

  int calls{0};
  int activate_calls{0};
  int stop_calls{0};
  bool fail{false};
  bool fail_activate{false};
  std::string last_artifact;
  vektor::RuntimeObservation observation;
  vektor::WorkloadSpec last_workload;
};
} // namespace

TEST(Deployment, RequiresDigestPinnedOciArtifact) {
  EXPECT_TRUE(vektor::is_pinned_oci_artifact(kArtifact));
  EXPECT_FALSE(vektor::is_pinned_oci_artifact("ghcr.io/demo:latest"));
  EXPECT_FALSE(vektor::is_pinned_oci_artifact("demo@sha256:abc"));
  EXPECT_FALSE(vektor::is_pinned_oci_artifact(
      "demo@sha256:"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
  EXPECT_FALSE(vektor::is_valid_deployment_id(".."));
}

TEST(Deployment, PersistsPrepareActivateAndRollback) {
  const auto path = std::filesystem::path("vektor_test_deployment.yaml");
  std::filesystem::remove(path);
  auto backend = std::make_shared<FakeRuntime>();
  {
    vektor::AgentDeploymentState state(path, backend);
    const auto staged = state.prepare("release-1", kArtifact);
    EXPECT_EQ(staged.phase, vektor::DeploymentPhase::Staged);
    EXPECT_EQ(backend->calls, 1);
    const auto active = state.activate("release-1");
    EXPECT_EQ(active.phase, vektor::DeploymentPhase::Active);
    EXPECT_TRUE(active.runtime_running);
    EXPECT_EQ(active.observed_artifact, kArtifact);
  }
  {
    vektor::AgentDeploymentState restored(path, backend);
    EXPECT_EQ(restored.current().phase, vektor::DeploymentPhase::Active);
    const auto rolled_back = restored.rollback("release-1");
    EXPECT_EQ(rolled_back.phase, vektor::DeploymentPhase::RolledBack);
    EXPECT_TRUE(rolled_back.artifact.empty());
  }
  std::filesystem::remove(path);
}

TEST(Deployment, RecordsBackendFailure) {
  const auto path =
      std::filesystem::path("vektor_test_deployment_failure.yaml");
  std::filesystem::remove(path);
  auto backend = std::make_shared<FakeRuntime>();
  backend->fail = true;
  vektor::AgentDeploymentState state(path, backend);
  EXPECT_THROW(state.prepare("release-1", kArtifact), std::runtime_error);
  EXPECT_EQ(state.current().phase, vektor::DeploymentPhase::Failed);
  EXPECT_NE(state.current().message.find("simulated pull failure"),
            std::string::npos);
  std::filesystem::remove(path);
}

TEST(Deployment, DetectsObservedRuntimeDrift) {
  const auto path = std::filesystem::path("vektor_test_deployment_drift.yaml");
  std::filesystem::remove(path);
  auto runtime = std::make_shared<FakeRuntime>();
  vektor::AgentDeploymentState state(path, runtime);
  state.prepare("release-1", kArtifact);
  state.activate("release-1");

  runtime->observation.workload_fingerprint = "tampered";
  const auto drifted = state.refresh_observed();
  EXPECT_EQ(drifted.phase, vektor::DeploymentPhase::Failed);
  EXPECT_TRUE(drifted.drift_detected);
  EXPECT_TRUE(drifted.runtime_running);
  EXPECT_EQ(drifted.observed_artifact, drifted.artifact);
  EXPECT_NE(drifted.observed_workload_fingerprint,
            vektor::workload_fingerprint(drifted.workload));
  std::filesystem::remove(path);
}

TEST(Deployment, RecordsActivationFailureAndCanRollback) {
  const auto path =
      std::filesystem::path("vektor_test_deployment_activation.yaml");
  std::filesystem::remove(path);
  auto runtime = std::make_shared<FakeRuntime>();
  vektor::AgentDeploymentState state(path, runtime);
  state.prepare("release-1", kArtifact);
  runtime->fail_activate = true;
  EXPECT_THROW(state.activate("release-1"), std::runtime_error);
  EXPECT_EQ(state.current().phase, vektor::DeploymentPhase::Failed);
  runtime->fail_activate = false;
  const auto rolled_back = state.rollback("release-1");
  EXPECT_EQ(rolled_back.phase, vektor::DeploymentPhase::RolledBack);
  EXPECT_FALSE(rolled_back.runtime_running);
  EXPECT_EQ(runtime->stop_calls, 1);
  std::filesystem::remove(path);
}

TEST(Deployment, PersistsAndRestoresPreviousWorkloadSpec) {
  const auto path =
      std::filesystem::path("vektor_test_deployment_workload.yaml");
  std::filesystem::remove(path);
  auto runtime = std::make_shared<FakeRuntime>();
  vektor::WorkloadSpec first;
  first.environment["ROS_DOMAIN_ID"] = "42";
  first.command = {"ros2", "launch", "picker", "robot.launch.py"};
  vektor::WorkloadSpec second;
  second.network = vektor::NetworkMode::Bridge;
  second.restart_policy = "always";
  second.mounts.push_back({"/var/lib/vektor", "/data", true});

  {
    vektor::AgentDeploymentState state(path, runtime);
    state.prepare("release-1", kArtifact, first);
    state.activate("release-1");
    state.prepare("release-2", kArtifactB, second);
    state.activate("release-2");
  }
  {
    vektor::AgentDeploymentState restored(path, runtime);
    EXPECT_EQ(restored.current().workload, second);
    EXPECT_EQ(restored.current().previous_workload, first);
    const auto rolled_back = restored.rollback("release-2");
    EXPECT_EQ(rolled_back.artifact, kArtifact);
    EXPECT_EQ(rolled_back.workload, first);
    EXPECT_EQ(runtime->last_workload, first);
  }
  std::filesystem::remove(path);
}

TEST(Runtime, ValidatesManagedContainerName) {
  EXPECT_TRUE(vektor::is_valid_runtime_container_name("vektor-workload_1"));
  EXPECT_FALSE(vektor::is_valid_runtime_container_name("bad name"));
  EXPECT_FALSE(vektor::is_valid_runtime_container_name("-bad"));
  EXPECT_THROW(vektor::OciRuntimeDriver("docker", "bad name"),
               std::invalid_argument);
}

TEST(Runtime, ValidatesAndFingerprintsWorkloadSpec) {
  vektor::WorkloadSpec spec;
  spec.environment = {{"MODE", "production"}, {"ROS_DOMAIN_ID", "42"}};
  spec.mounts.push_back({"/var/lib/vektor", "/data", true});
  spec.devices.push_back({"/dev/video0", "/dev/video0"});
  spec.command = {"robot", "--safe"};
  EXPECT_NO_THROW(vektor::validate_workload_spec(spec));
  EXPECT_EQ(vektor::workload_fingerprint(spec),
            vektor::workload_fingerprint(spec));

  auto invalid = spec;
  invalid.environment["BAD-NAME"] = "value";
  EXPECT_THROW(vektor::validate_workload_spec(invalid), std::invalid_argument);
  invalid = spec;
  invalid.mounts.push_back({"relative", "/other", false});
  EXPECT_THROW(vektor::validate_workload_spec(invalid), std::invalid_argument);
}

TEST(Runtime, WorkloadProtoRoundTrips) {
  vektor::WorkloadSpec expected;
  expected.network = vektor::NetworkMode::None;
  expected.restart_policy = "on-failure";
  expected.environment["MODE"] = "test";
  expected.mounts.push_back({"/source", "/target", true});
  expected.devices.push_back({"/dev/input0", "/dev/input0"});
  expected.command = {"run", "--once"};
  EXPECT_EQ(vektor::from_proto(vektor::to_proto(expected)), expected);
}

TEST(Runtime, OciDriverActivatesInspectsAndProtectsUnmanagedContainer) {
  const auto executable = std::filesystem::path("vektor_fake_runtime.sh");
  const auto state = std::filesystem::path(executable.string() + ".state");
  const auto managed = std::filesystem::path(executable.string() + ".managed");
  const auto workload_state =
      std::filesystem::path(executable.string() + ".workload");
  const auto arguments = std::filesystem::path(executable.string() + ".args");
  std::filesystem::remove(executable);
  std::filesystem::remove(state);
  std::filesystem::remove(managed);
  std::filesystem::remove(workload_state);
  std::filesystem::remove(arguments);
  {
    std::ofstream script(executable);
    script
        << "#!/bin/sh\n"
           "state=\"$0.state\"\n"
           "managed=\"$0.managed\"\n"
           "case \"$1\" in\n"
           "  pull) exit 0 ;;\n"
           "  inspect)\n"
           "    test -f \"$state\" || exit 1\n"
           "    printf '%s|fake-id|%s|true|%s\\n' \"$(cat \"$state\")\" "
           "\"$(cat \"$managed\")\" \"$(cat \"$0.workload\")\" ;;\n"
           "  rm) rm -f \"$state\" \"$managed\" \"$0.workload\" ;;\n"
           "  run)\n"
           "    printf '%s\\n' \"$@\" > \"$0.args\"\n"
           "    for value in \"$@\"; do\n"
           "      case \"$value\" in *@sha256:*) artifact=\"$value\" ;; esac\n"
           "      case \"$value\" in io.vektor.workload=*) "
           "printf '%s' \"${value#*=}\" > \"$0.workload\" ;; esac\n"
           "    done\n"
           "    printf '%s' \"$artifact\" > \"$state\"\n"
           "    printf 'true' > \"$managed\"\n"
           "    printf 'fake-id\\n' ;;\n"
           "  *) exit 2 ;;\n"
           "esac\n";
  }
  std::filesystem::permissions(executable,
                               std::filesystem::perms::owner_all |
                                   std::filesystem::perms::group_read |
                                   std::filesystem::perms::group_exec);

  vektor::OciRuntimeDriver driver("./" + executable.string(), "workload");
  EXPECT_NO_THROW(driver.prepare(kArtifact));
  vektor::WorkloadSpec workload;
  workload.network = vektor::NetworkMode::Bridge;
  workload.restart_policy = "always";
  workload.environment["MODE"] = "test";
  workload.mounts.push_back({"/source", "/target", true});
  workload.devices.push_back({"/dev/video0", "/dev/video0"});
  workload.command = {"robot", "--safe"};
  const auto active = driver.activate(kArtifact, workload);
  EXPECT_TRUE(active.running);
  EXPECT_TRUE(active.managed);
  EXPECT_EQ(active.artifact, kArtifact);
  EXPECT_EQ(driver.inspect().runtime_id, "fake-id");
  std::ifstream argument_file(arguments);
  const std::string recorded_arguments{
      std::istreambuf_iterator<char>(argument_file), {}};
  EXPECT_NE(recorded_arguments.find("bridge"), std::string::npos);
  EXPECT_NE(recorded_arguments.find("MODE=test"), std::string::npos);
  EXPECT_NE(recorded_arguments.find(
                "type=bind,source=/source,target=/target,readonly"),
            std::string::npos);
  EXPECT_NE(recorded_arguments.find("/dev/video0:/dev/video0"),
            std::string::npos);
  EXPECT_NE(recorded_arguments.find("robot\n--safe"), std::string::npos);

  {
    std::ofstream state_file(state);
    state_file << kArtifact;
    std::ofstream managed_file(managed);
    managed_file << "false";
  }
  EXPECT_THROW(driver.activate(kArtifact, {}), std::runtime_error);
  EXPECT_TRUE(std::filesystem::exists(state));

  std::filesystem::remove(executable);
  std::filesystem::remove(state);
  std::filesystem::remove(managed);
  std::filesystem::remove(workload_state);
  std::filesystem::remove(arguments);
}
