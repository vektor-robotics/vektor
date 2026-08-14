#include "vektor/deployment.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <stdexcept>

namespace {
constexpr auto kArtifact =
    "ghcr.io/vektor-robotics/demo@sha256:"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

class FakeBackend final : public vektor::ArtifactBackend {
public:
  void prepare(const std::string &artifact) override {
    ++calls;
    last_artifact = artifact;
    if (fail)
      throw std::runtime_error("simulated pull failure");
  }

  int calls{0};
  bool fail{false};
  std::string last_artifact;
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
  auto backend = std::make_shared<FakeBackend>();
  {
    vektor::AgentDeploymentState state(path, backend);
    const auto staged = state.prepare("release-1", kArtifact);
    EXPECT_EQ(staged.phase, vektor::DeploymentPhase::Staged);
    EXPECT_EQ(backend->calls, 1);
    const auto active = state.activate("release-1");
    EXPECT_EQ(active.phase, vektor::DeploymentPhase::Active);
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
  auto backend = std::make_shared<FakeBackend>();
  backend->fail = true;
  vektor::AgentDeploymentState state(path, backend);
  EXPECT_THROW(state.prepare("release-1", kArtifact), std::runtime_error);
  EXPECT_EQ(state.current().phase, vektor::DeploymentPhase::Failed);
  EXPECT_NE(state.current().message.find("simulated pull failure"),
            std::string::npos);
  std::filesystem::remove(path);
}
