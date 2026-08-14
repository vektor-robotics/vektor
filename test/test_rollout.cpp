#include "vektor/agent.hpp"
#include "vektor/rollout.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>

namespace {
constexpr auto kArtifact =
    "ghcr.io/vektor-robotics/demo@sha256:"
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

class FakeBackend final : public vektor::ArtifactBackend {
public:
  void prepare(const std::string &) override {
    ++calls;
    if (on_prepare)
      on_prepare();
  }
  int calls{0};
  std::function<void()> on_prepare;
};

vektor::StatusSnapshot healthy_snapshot(const std::string &robot_id) {
  auto snapshot =
      vektor::make_status_snapshot(robot_id, {}, std::chrono::milliseconds(0));
  snapshot.state = vektor::HealthState::Healthy;
  return snapshot;
}

struct TestDeployAgent {
  vektor::AgentStatusState health;
  std::shared_ptr<FakeBackend> backend{std::make_shared<FakeBackend>()};
  vektor::AgentDeploymentState deployment;
  vektor::GrpcAgentService service;
  std::unique_ptr<grpc::Server> server;
  int port{0};

  TestDeployAgent(const std::string &robot_id,
                  const std::filesystem::path &state_path)
      : deployment(state_path, backend), service(health, deployment) {
    health.publish(healthy_snapshot(robot_id));
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port);
    builder.RegisterService(&service);
    server = builder.BuildAndStart();
  }

  ~TestDeployAgent() {
    if (server)
      server->Shutdown();
  }
};

struct RolloutFiles {
  std::filesystem::path fleet{"vektor_test_rollout_fleet.yaml"};
  std::filesystem::path rollout{"vektor_test_rollout.yaml"};
  std::filesystem::path state{"vektor_test_rollout_state.yaml"};
  std::filesystem::path first_agent{"vektor_test_rollout_agent_1.yaml"};
  std::filesystem::path second_agent{"vektor_test_rollout_agent_2.yaml"};

  RolloutFiles() { cleanup(); }
  ~RolloutFiles() { cleanup(); }

  void cleanup() const {
    std::filesystem::remove(fleet);
    std::filesystem::remove(rollout);
    std::filesystem::remove(state);
    std::filesystem::remove(first_agent);
    std::filesystem::remove(second_agent);
  }
};

void write_configs(const RolloutFiles &files, int first_port, int second_port) {
  std::ofstream fleet(files.fleet);
  fleet << "fleet_id: test-fleet\nrequest_timeout_ms: 1000\n"
        << "max_snapshot_age_ms: 5000\ntransport:\n  insecure: true\n"
        << "robots:\n  - id: robot-1\n    endpoint: 127.0.0.1:" << first_port
        << "\n    labels:\n      ring: canary\n"
        << "  - id: robot-2\n    endpoint: 127.0.0.1:" << second_port
        << "\n    labels:\n      ring: stable\n";
  fleet.close();

  std::ofstream rollout(files.rollout);
  rollout << "schema_version: 1\ndeployment_id: release-1\nartifact: "
          << kArtifact << "\nfleet_config: " << files.fleet.string()
          << "\nstate_file: " << files.state.string()
          << "\noperation_timeout_ms: 1000\nsettle_time_ms: 0\nwaves:\n"
          << "  - name: canary\n    selectors: [ring=canary]\n"
          << "  - name: stable\n    selectors: [ring=stable]\n";
}
} // namespace

TEST(Rollout, RejectsRobotInMultipleWaves) {
  RolloutFiles files;
  std::ofstream fleet(files.fleet);
  fleet << "fleet_id: test\ntransport:\n  insecure: true\nrobots:\n"
        << "  - id: robot-1\n    endpoint: 127.0.0.1:50051\n"
        << "    labels:\n      site: berlin\n";
  fleet.close();
  std::ofstream rollout(files.rollout);
  rollout << "schema_version: 1\ndeployment_id: release-1\nartifact: "
          << kArtifact << "\nfleet_config: " << files.fleet.string()
          << "\nwaves:\n  - name: first\n    selectors: [site=berlin]\n"
          << "  - name: second\n    selectors: [site=berlin]\n";
  rollout.close();
  EXPECT_THROW(vektor::load_rollout_config(files.rollout.string()),
               std::runtime_error);
}

TEST(Rollout, DeploysPromotesAndRollsBackTwoWaves) {
  RolloutFiles files;
  TestDeployAgent first("robot-1", files.first_agent);
  TestDeployAgent second("robot-2", files.second_agent);
  ASSERT_GT(first.port, 0);
  ASSERT_GT(second.port, 0);
  write_configs(files, first.port, second.port);

  const auto config = vektor::load_rollout_config(files.rollout.string());
  EXPECT_EQ(config.operation_timeout.count(), 1000);
  const auto deployed = vektor::deploy_release(config);
  EXPECT_TRUE(deployed.success);
  EXPECT_FALSE(deployed.complete);
  EXPECT_EQ(deployed.wave, "canary");
  EXPECT_EQ(first.backend->calls, 1);
  EXPECT_EQ(second.backend->calls, 0);
  EXPECT_NE(
      vektor::rollout_report_to_json(deployed).find("\"action\":\"deploy\""),
      std::string::npos);

  auto stub = vektor::agent::v1::Agent::NewStub(
      grpc::CreateChannel("127.0.0.1:" + std::to_string(first.port),
                          grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  vektor::agent::v1::GetDeploymentRequest request;
  vektor::agent::v1::DeploymentRecord response;
  ASSERT_TRUE(stub->GetDeployment(&context, request, &response).ok());
  EXPECT_EQ(response.phase(), vektor::agent::v1::DEPLOYMENT_PHASE_ACTIVE);

  const auto promoted = vektor::promote_release(config);
  EXPECT_TRUE(promoted.success);
  EXPECT_TRUE(promoted.complete);
  EXPECT_EQ(promoted.wave, "stable");
  EXPECT_EQ(second.backend->calls, 1);

  const auto rolled_back = vektor::rollback_release(config);
  EXPECT_TRUE(rolled_back.success);
  EXPECT_EQ(rolled_back.robots.size(), 2U);
  EXPECT_EQ(first.deployment.current().phase,
            vektor::DeploymentPhase::RolledBack);
  EXPECT_EQ(second.deployment.current().phase,
            vektor::DeploymentPhase::RolledBack);
}

TEST(Rollout, AutomaticallyRollsBackFailedHealthGate) {
  RolloutFiles files;
  TestDeployAgent first("robot-1", files.first_agent);
  TestDeployAgent second("robot-2", files.second_agent);
  write_configs(files, first.port, second.port);
  first.backend->on_prepare = [&] {
    auto snapshot = healthy_snapshot("robot-1");
    snapshot.state = vektor::HealthState::Unhealthy;
    first.health.publish(std::move(snapshot));
  };

  const auto config = vektor::load_rollout_config(files.rollout.string());
  const auto deployed = vektor::deploy_release(config);
  EXPECT_FALSE(deployed.success);
  EXPECT_NE(deployed.message.find("health gate failed"), std::string::npos);
  EXPECT_EQ(first.deployment.current().phase,
            vektor::DeploymentPhase::RolledBack);
  EXPECT_FALSE(std::filesystem::exists(files.state));
}
