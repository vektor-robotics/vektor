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

class FakeRuntime final : public vektor::RuntimeDriver {
public:
  void prepare(const std::string &artifact) override {
    ++calls;
    last_artifact = artifact;
    if (fail)
      throw std::runtime_error("simulated pull failure");
  }

  vektor::RuntimeObservation activate(const std::string &artifact) override {
    ++activate_calls;
    if (fail_activate)
      throw std::runtime_error("simulated activation failure");
    observation = {true, artifact, "container-123", true};
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

  runtime->observation.artifact =
      "ghcr.io/vektor-robotics/other@sha256:"
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  const auto drifted = state.refresh_observed();
  EXPECT_EQ(drifted.phase, vektor::DeploymentPhase::Failed);
  EXPECT_TRUE(drifted.drift_detected);
  EXPECT_TRUE(drifted.runtime_running);
  EXPECT_NE(drifted.observed_artifact, drifted.artifact);
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

TEST(Runtime, ValidatesManagedContainerName) {
  EXPECT_TRUE(vektor::is_valid_runtime_container_name("vektor-workload_1"));
  EXPECT_FALSE(vektor::is_valid_runtime_container_name("bad name"));
  EXPECT_FALSE(vektor::is_valid_runtime_container_name("-bad"));
  EXPECT_THROW(vektor::OciRuntimeDriver("docker", "bad name"),
               std::invalid_argument);
}

TEST(Runtime, OciDriverActivatesInspectsAndProtectsUnmanagedContainer) {
  const auto executable = std::filesystem::path("vektor_fake_runtime.sh");
  const auto state = std::filesystem::path(executable.string() + ".state");
  const auto managed = std::filesystem::path(executable.string() + ".managed");
  std::filesystem::remove(executable);
  std::filesystem::remove(state);
  std::filesystem::remove(managed);
  {
    std::ofstream script(executable);
    script << "#!/bin/sh\n"
              "state=\"$0.state\"\n"
              "managed=\"$0.managed\"\n"
              "case \"$1\" in\n"
              "  pull) exit 0 ;;\n"
              "  inspect)\n"
              "    test -f \"$state\" || exit 1\n"
              "    printf '%s|fake-id|%s|true\\n' \"$(cat \"$state\")\" "
              "\"$(cat \"$managed\")\" ;;\n"
              "  rm) rm -f \"$state\" \"$managed\" ;;\n"
              "  run)\n"
              "    for value in \"$@\"; do artifact=\"$value\"; done\n"
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
  const auto active = driver.activate(kArtifact);
  EXPECT_TRUE(active.running);
  EXPECT_TRUE(active.managed);
  EXPECT_EQ(active.artifact, kArtifact);
  EXPECT_EQ(driver.inspect().runtime_id, "fake-id");

  {
    std::ofstream state_file(state);
    state_file << kArtifact;
    std::ofstream managed_file(managed);
    managed_file << "false";
  }
  EXPECT_THROW(driver.activate(kArtifact), std::runtime_error);
  EXPECT_TRUE(std::filesystem::exists(state));

  std::filesystem::remove(executable);
  std::filesystem::remove(state);
  std::filesystem::remove(managed);
}
