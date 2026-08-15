#include "vektor/agent.hpp"
#include "vektor/rollout.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr auto kArtifact =
    "ghcr.io/vektor-robotics/demo@sha256:"
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr auto kPreviousArtifact =
    "ghcr.io/vektor-robotics/demo@sha256:"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

class FakeBackend final : public vektor::RuntimeDriver {
public:
  void prepare(const std::string &) override {
    ++calls;
    if (on_prepare)
      on_prepare();
  }
  vektor::RuntimeObservation
  activate(const std::string &artifact,
           const vektor::WorkloadSpec &workload) override {
    last_workload = workload;
    observation = {true,     true,
                   artifact, "test-container",
                   true,     vektor::workload_fingerprint(workload),
                   "none"};
    if (on_activate)
      on_activate();
    return observation;
  }
  vektor::RuntimeObservation stop() override {
    observation = {};
    return observation;
  }
  vektor::RuntimeObservation inspect() override { return observation; }
  int calls{0};
  std::function<void()> on_prepare;
  std::function<void()> on_activate;
  vektor::RuntimeObservation observation;
  vektor::WorkloadSpec last_workload;
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
    backend->on_activate = [this, robot_id] {
      health.publish(healthy_snapshot(robot_id));
    };
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
  std::filesystem::path crash_copy{"vektor_test_rollout_agent_crash.yaml"};
  std::filesystem::path approval_policy{
      "vektor_test_rollout_approval_policy.yaml"};
  std::filesystem::path approval_file{"vektor_test_rollout_approvals.yaml"};

  RolloutFiles() { cleanup(); }
  ~RolloutFiles() { cleanup(); }

  void cleanup() const {
    std::filesystem::remove(fleet);
    std::filesystem::remove(rollout);
    std::filesystem::remove(state);
    std::filesystem::remove(first_agent);
    std::filesystem::remove(second_agent);
    std::filesystem::remove(crash_copy);
    std::filesystem::remove(approval_policy);
    std::filesystem::remove(approval_file);
  }
};

void write_configs(const RolloutFiles &files, int first_port, int second_port,
                   bool single_wave = false) {
  std::ofstream fleet(files.fleet);
  fleet << "fleet_id: test-fleet\nrequest_timeout_ms: 1000\n"
        << "max_snapshot_age_ms: 5000\ntransport:\n  insecure: true\n"
        << "robots:\n  - id: robot-1\n    endpoint: 127.0.0.1:" << first_port
        << "\n    labels:\n      ring: canary\n"
        << "  - id: robot-2\n    endpoint: 127.0.0.1:" << second_port
        << "\n    labels:\n      ring: stable\n";
  fleet.close();

  std::ofstream rollout(files.rollout);
  rollout << "schema_version: 1\ndeployment_id: release-1\nworkload_id: "
             "picker\nartifact: "
          << kArtifact << "\nfleet_config: " << files.fleet.string()
          << "\nstate_file: " << files.state.string()
          << "\noperation_timeout_ms: 1000\nsettle_time_ms: 0\n"
          << "readiness_timeout_ms: 1000\n"
          << "workload:\n  network: host\n  restart_policy: unless-stopped\n"
          << "  environment:\n    ROS_DOMAIN_ID: '42'\n"
          << "  mounts:\n    - source: /tmp\n      target: /data\n"
          << "      read_only: true\n  command: [sleep, infinity]\n"
          << "waves:\n";
  if (single_wave) {
    rollout << "  - name: all\n";
  } else {
    rollout << "  - name: canary\n    selectors: [ring=canary]\n"
            << "  - name: stable\n    selectors: [ring=stable]\n";
  }
}

struct ActivationLog {
  void record(const std::string &robot_id, const std::string &artifact) {
    std::lock_guard lock(mutex);
    entries.push_back(robot_id + ":" + artifact);
  }

  void clear() {
    std::lock_guard lock(mutex);
    entries.clear();
  }

  std::vector<std::string> snapshot() const {
    std::lock_guard lock(mutex);
    return entries;
  }

  mutable std::mutex mutex;
  std::vector<std::string> entries;
};

class FaultInjectingRuntime final : public vektor::RuntimeDriver {
public:
  FaultInjectingRuntime(std::string robot_id,
                        std::shared_ptr<ActivationLog> activation_log)
      : robot_id_(std::move(robot_id)),
        activation_log_(std::move(activation_log)) {}

  void prepare(const std::string &artifact) override {
    prepare_impl(artifact, std::chrono::milliseconds::max());
  }

  void prepare(const std::string &artifact,
               std::chrono::milliseconds timeout) override {
    prepare_impl(artifact, timeout);
  }

  vektor::RuntimeObservation
  activate(const std::string &artifact,
           const vektor::WorkloadSpec &workload) override {
    return activate_impl(artifact, workload);
  }

  vektor::RuntimeObservation activate(const std::string &artifact,
                                      const vektor::WorkloadSpec &workload,
                                      std::chrono::milliseconds,
                                      std::chrono::milliseconds) override {
    return activate_impl(artifact, workload);
  }

  vektor::RuntimeObservation stop() override {
    std::lock_guard lock(mutex_);
    observation_ = {};
    return observation_;
  }

  vektor::RuntimeObservation stop(std::chrono::milliseconds) override {
    return stop();
  }

  vektor::RuntimeObservation inspect() override {
    std::lock_guard lock(mutex_);
    return observation_;
  }

  vektor::RuntimeObservation inspect(std::chrono::milliseconds) override {
    return inspect();
  }

  void set_prepare_timeout(bool enabled) {
    std::lock_guard lock(mutex_);
    prepare_timeout_ = enabled;
  }

  std::chrono::milliseconds last_prepare_timeout() const {
    std::lock_guard lock(mutex_);
    return last_prepare_timeout_;
  }

  void set_on_activate(std::function<void()> callback) {
    std::lock_guard lock(mutex_);
    on_activate_ = std::move(callback);
  }

  void introduce_drift() {
    std::lock_guard lock(mutex_);
    observation_.workload_fingerprint = "manually-tampered";
  }

  vektor::RuntimeObservation observation() const {
    std::lock_guard lock(mutex_);
    return observation_;
  }

private:
  void prepare_impl(const std::string &, std::chrono::milliseconds timeout) {
    std::lock_guard lock(mutex_);
    last_prepare_timeout_ = timeout;
    if (prepare_timeout_)
      throw std::runtime_error("runtime prepare timed out");
  }

  vektor::RuntimeObservation
  activate_impl(const std::string &artifact,
                const vektor::WorkloadSpec &workload) {
    std::function<void()> callback;
    vektor::RuntimeObservation observed;
    {
      std::lock_guard lock(mutex_);
      observation_ = {true,     true,
                      artifact, robot_id_ + "-container",
                      true,     vektor::workload_fingerprint(workload),
                      "healthy"};
      observed = observation_;
      callback = on_activate_;
    }
    activation_log_->record(robot_id_, artifact);
    if (callback)
      callback();
    return observed;
  }

  std::string robot_id_;
  std::shared_ptr<ActivationLog> activation_log_;
  mutable std::mutex mutex_;
  bool prepare_timeout_{false};
  std::chrono::milliseconds last_prepare_timeout_{0};
  std::function<void()> on_activate_;
  vektor::RuntimeObservation observation_;
};

class RestartableDeployAgent {
private:
  std::string robot_id_;
  std::filesystem::path state_path_;

public:
  RestartableDeployAgent(std::string robot_id, std::filesystem::path state_path,
                         std::shared_ptr<ActivationLog> activation_log)
      : robot_id_(std::move(robot_id)), state_path_(std::move(state_path)),
        runtime(std::make_shared<FaultInjectingRuntime>(robot_id_,
                                                        activation_log)) {
    publish_healthy();
    runtime->set_on_activate([this] { publish_healthy(); });
    start();
  }

  ~RestartableDeployAgent() {
    stop();
    runtime->set_on_activate(nullptr);
  }

  RestartableDeployAgent(const RestartableDeployAgent &) = delete;
  RestartableDeployAgent &operator=(const RestartableDeployAgent &) = delete;

  void publish_healthy() { health.publish(healthy_snapshot(robot_id_)); }

  void start() {
    deployment =
        std::make_unique<vektor::AgentDeploymentState>(state_path_, runtime);
    service = std::make_unique<vektor::GrpcAgentService>(health, *deployment);
    grpc::ServerBuilder builder;
    int selected_port = 0;
    const auto address =
        port == 0 ? "127.0.0.1:0" : "127.0.0.1:" + std::to_string(port);
    builder.AddListeningPort(address, grpc::InsecureServerCredentials(),
                             &selected_port);
    builder.RegisterService(service.get());
    server = builder.BuildAndStart();
    if (!server || selected_port <= 0 || (port != 0 && selected_port != port))
      throw std::runtime_error("failed to start restartable test agent");
    port = selected_port;
  }

  void stop() {
    if (server) {
      server->Shutdown();
      server->Wait();
      server.reset();
    }
    service.reset();
    deployment.reset();
  }

  vektor::DeploymentRecord restart_and_recover() {
    stop();
    start();
    return deployment->refresh_observed();
  }

  std::string endpoint() const { return "127.0.0.1:" + std::to_string(port); }

  vektor::AgentStatusState health;
  std::shared_ptr<FaultInjectingRuntime> runtime;
  std::unique_ptr<vektor::AgentDeploymentState> deployment;
  std::unique_ptr<vektor::GrpcAgentService> service;
  std::unique_ptr<grpc::Server> server;
  int port{0};
};

grpc::Status prepare_via_rpc(const RestartableDeployAgent &agent,
                             const vektor::RolloutConfig &config,
                             vektor::agent::v1::DeploymentRecord &response) {
  auto stub = vektor::agent::v1::Agent::NewStub(grpc::CreateChannel(
      agent.endpoint(), grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  vektor::agent::v1::PrepareDeploymentRequest request;
  request.set_deployment_id(config.deployment_id);
  request.set_artifact(config.artifact);
  request.set_operation_timeout_ms(config.operation_timeout.count());
  *request.mutable_workload() = vektor::to_proto(config.workload);
  return stub->PrepareDeployment(&context, request, &response);
}

grpc::Status activate_via_rpc(const RestartableDeployAgent &agent,
                              const vektor::RolloutConfig &config,
                              vektor::agent::v1::DeploymentRecord &response) {
  auto stub = vektor::agent::v1::Agent::NewStub(grpc::CreateChannel(
      agent.endpoint(), grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  vektor::agent::v1::ActivateDeploymentRequest request;
  request.set_deployment_id(config.deployment_id);
  request.set_operation_timeout_ms(config.operation_timeout.count());
  request.set_readiness_timeout_ms(config.readiness_timeout.count());
  return stub->ActivateDeployment(&context, request, &response);
}

void install_previous_release(RestartableDeployAgent &agent) {
  agent.deployment->prepare("previous-release", kPreviousArtifact);
  agent.deployment->activate("previous-release");
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
  EXPECT_EQ(config.workload_id, "picker");
  EXPECT_EQ(config.operation_timeout.count(), 1000);
  EXPECT_EQ(config.readiness_timeout.count(), 1000);
  EXPECT_EQ(config.workload.environment.at("ROS_DOMAIN_ID"), "42");
  ASSERT_EQ(config.workload.mounts.size(), 1U);
  EXPECT_TRUE(config.workload.mounts.front().read_only);
  const auto deployed = vektor::deploy_release(config);
  EXPECT_TRUE(deployed.success);
  EXPECT_FALSE(deployed.complete);
  EXPECT_EQ(deployed.wave, "canary");
  EXPECT_EQ(first.backend->calls, 1);
  EXPECT_EQ(first.backend->last_workload, config.workload);
  EXPECT_EQ(second.backend->calls, 0);
  EXPECT_NE(
      vektor::rollout_report_to_json(deployed).find("\"action\":\"deploy\""),
      std::string::npos);
  EXPECT_NE(
      vektor::rollout_report_to_json(deployed).find("\"schema_version\":3"),
      std::string::npos);

  auto stub = vektor::agent::v1::Agent::NewStub(
      grpc::CreateChannel("127.0.0.1:" + std::to_string(first.port),
                          grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  vektor::agent::v1::GetDeploymentRequest request;
  vektor::agent::v1::DeploymentRecord response;
  ASSERT_TRUE(stub->GetDeployment(&context, request, &response).ok());
  EXPECT_EQ(response.phase(), vektor::agent::v1::DEPLOYMENT_PHASE_ACTIVE);
  EXPECT_EQ(response.schema_version(), 5U);
  EXPECT_EQ(response.observed_artifact(), kArtifact);
  EXPECT_EQ(response.observed_workload_fingerprint(),
            vektor::workload_fingerprint(config.workload));
  EXPECT_TRUE(response.runtime_running());
  EXPECT_TRUE(response.runtime_ready());
  EXPECT_EQ(response.reconciliation_operation(),
            vektor::agent::v1::RECONCILIATION_OPERATION_NONE);
  EXPECT_EQ(response.operation_attempt(), 2U);
  EXPECT_FALSE(response.drift_detected());

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
  first.backend->on_activate = [&] {
    auto snapshot = healthy_snapshot("robot-1");
    snapshot.state = vektor::HealthState::Unhealthy;
    first.health.publish(std::move(snapshot));
  };

  const auto config = vektor::load_rollout_config(files.rollout.string());
  const auto deployed = vektor::deploy_release(config);
  EXPECT_FALSE(deployed.success);
  EXPECT_NE(deployed.message.find("activation failed"), std::string::npos);
  ASSERT_EQ(deployed.robots.size(), 1U);
  EXPECT_NE(deployed.robots.front().message.find("fresh ROS health"),
            std::string::npos);
  EXPECT_EQ(deployed.robots.front().phase, "failed");
  EXPECT_EQ(deployed.robots.front().operation, "none");
  EXPECT_EQ(first.deployment.current().phase,
            vektor::DeploymentPhase::RolledBack);
  EXPECT_FALSE(std::filesystem::exists(files.state));
}

TEST(Rollout, TimesOutWithoutFreshPostActivationHealth) {
  RolloutFiles files;
  TestDeployAgent first("robot-1", files.first_agent);
  TestDeployAgent second("robot-2", files.second_agent);
  write_configs(files, first.port, second.port);
  first.backend->on_activate = nullptr;

  auto config = vektor::load_rollout_config(files.rollout.string());
  config.readiness_timeout = std::chrono::milliseconds(25);
  const auto deployed = vektor::deploy_release(config);
  EXPECT_FALSE(deployed.success);
  EXPECT_NE(deployed.message.find("activation failed"), std::string::npos);
  ASSERT_EQ(deployed.robots.size(), 1U);
  EXPECT_NE(deployed.robots.front().message.find("before timeout"),
            std::string::npos);
  EXPECT_EQ(deployed.robots.front().phase, "failed");
  EXPECT_EQ(first.deployment.current().phase,
            vektor::DeploymentPhase::RolledBack);
}

TEST(Rollout, DeniesRequiredApprovalBeforeAgentMutation) {
  RolloutFiles files;
  TestDeployAgent first("robot-1", files.first_agent);
  TestDeployAgent second("robot-2", files.second_agent);
  write_configs(files, first.port, second.port);
  {
    std::ofstream rollout(files.rollout, std::ios::app);
    rollout << "environment: production\napproval_policy: "
            << files.approval_policy.string()
            << "\napproval_file: " << files.approval_file.string() << '\n';
  }
  {
    std::ofstream policy(files.approval_policy);
    policy << "schema_version: 1\nsensitive_environments: [production]\n"
              "max_wave_size_without_approval: 100\nrequired_approvals: 1\n"
              "approvers:\n  - identity: release-approver\n"
              "    public_key: missing.pem\n";
  }

  const auto config = vektor::load_rollout_config(files.rollout.string());
  const auto report = vektor::deploy_release(config);
  EXPECT_FALSE(report.success);
  EXPECT_TRUE(report.approval_required);
  EXPECT_TRUE(report.approvers.empty());
  EXPECT_TRUE(report.message.starts_with("VEKTOR_APPROVAL_REQUIRED:"));
  EXPECT_NE(report.message.find("approval bundle"), std::string::npos);
  EXPECT_EQ(first.backend->calls, 0);
  EXPECT_EQ(second.backend->calls, 0);
  EXPECT_FALSE(std::filesystem::exists(files.state));
  const auto json = vektor::rollout_report_to_json(report);
  EXPECT_NE(json.find("\"schema_version\":3"), std::string::npos);
  EXPECT_NE(json.find("\"approval_required\":true"), std::string::npos);
}

TEST(RolloutIntegration,
     RecoversRestartDetectsDriftAndRollsBackTwoRobotsInReverseOrder) {
  RolloutFiles files;
  auto activation_log = std::make_shared<ActivationLog>();
  RestartableDeployAgent canary("robot-1", files.first_agent, activation_log);
  RestartableDeployAgent stable("robot-2", files.second_agent, activation_log);
  install_previous_release(canary);
  install_previous_release(stable);
  write_configs(files, canary.port, stable.port);
  const auto config = vektor::load_rollout_config(files.rollout.string());

  canary.runtime->set_on_activate([&] {
    std::filesystem::copy_file(
        files.first_agent, files.crash_copy,
        std::filesystem::copy_options::overwrite_existing);
    canary.publish_healthy();
  });
  const auto deployed = vektor::deploy_release(config);
  ASSERT_TRUE(deployed.success) << deployed.message;
  ASSERT_FALSE(deployed.complete);
  ASSERT_TRUE(std::filesystem::exists(files.crash_copy));
  EXPECT_EQ(canary.runtime->observation().artifact, kArtifact);
  EXPECT_TRUE(canary.runtime->observation().ready);

  canary.stop();
  std::filesystem::copy_file(files.crash_copy, files.first_agent,
                             std::filesystem::copy_options::overwrite_existing);
  const auto recovered = canary.restart_and_recover();
  EXPECT_EQ(recovered.phase, vektor::DeploymentPhase::Staged);
  EXPECT_EQ(recovered.operation, vektor::ReconciliationOperation::None);
  EXPECT_NE(recovered.message.find("retry activation"), std::string::npos);

  canary.runtime->set_on_activate([&] { canary.publish_healthy(); });
  vektor::agent::v1::DeploymentRecord response;
  ASSERT_TRUE(activate_via_rpc(canary, config, response).ok());
  EXPECT_EQ(response.phase(), vektor::agent::v1::DEPLOYMENT_PHASE_ACTIVE);
  EXPECT_EQ(response.observed_artifact(), kArtifact);
  EXPECT_TRUE(response.runtime_ready());

  canary.runtime->introduce_drift();
  const auto paused = vektor::promote_release(config);
  EXPECT_FALSE(paused.success);
  EXPECT_EQ(paused.next_wave, 1U);
  EXPECT_NE(paused.message.find("runtime drift detected"), std::string::npos);
  ASSERT_EQ(paused.robots.size(), 1U);
  EXPECT_EQ(paused.robots.front().phase, "failed");

  response.Clear();
  ASSERT_TRUE(prepare_via_rpc(canary, config, response).ok());
  response.Clear();
  ASSERT_TRUE(activate_via_rpc(canary, config, response).ok());
  const auto promoted = vektor::promote_release(config);
  ASSERT_TRUE(promoted.success) << promoted.message;
  ASSERT_TRUE(promoted.complete);
  EXPECT_EQ(canary.runtime->observation().artifact, kArtifact);
  EXPECT_EQ(stable.runtime->observation().artifact, kArtifact);
  EXPECT_TRUE(canary.runtime->observation().ready);
  EXPECT_TRUE(stable.runtime->observation().ready);

  activation_log->clear();
  const auto rolled_back = vektor::rollback_release(config);
  ASSERT_TRUE(rolled_back.success) << rolled_back.message;
  ASSERT_EQ(rolled_back.robots.size(), 2U);
  EXPECT_EQ(rolled_back.robots[0].robot_id, "robot-2");
  EXPECT_EQ(rolled_back.robots[1].robot_id, "robot-1");
  const auto rollback_order = activation_log->snapshot();
  ASSERT_EQ(rollback_order.size(), 2U);
  EXPECT_EQ(rollback_order[0], std::string("robot-2:") + kPreviousArtifact);
  EXPECT_EQ(rollback_order[1], std::string("robot-1:") + kPreviousArtifact);
  EXPECT_EQ(canary.runtime->observation().artifact, kPreviousArtifact);
  EXPECT_EQ(stable.runtime->observation().artifact, kPreviousArtifact);
  EXPECT_TRUE(canary.runtime->observation().ready);
  EXPECT_TRUE(stable.runtime->observation().ready);
}

TEST(RolloutIntegration, RuntimeTimeoutRollsBackEntireTwoRobotWave) {
  RolloutFiles files;
  auto activation_log = std::make_shared<ActivationLog>();
  RestartableDeployAgent first("robot-1", files.first_agent, activation_log);
  RestartableDeployAgent second("robot-2", files.second_agent, activation_log);
  install_previous_release(first);
  install_previous_release(second);
  write_configs(files, first.port, second.port, true);
  auto config = vektor::load_rollout_config(files.rollout.string());
  // Leave enough margin for gRPC scheduling on loaded CI runners. The fake
  // runtime still fails immediately and records the deadline it receives.
  config.operation_timeout = std::chrono::milliseconds(250);
  first.runtime->set_prepare_timeout(true);

  const auto deployed = vektor::deploy_release(config);
  EXPECT_FALSE(deployed.success);
  EXPECT_NE(deployed.message.find("prepare failed; wave rolled back"),
            std::string::npos);
  ASSERT_EQ(deployed.robots.size(), 2U);
  EXPECT_TRUE(std::any_of(
      deployed.robots.begin(), deployed.robots.end(), [](const auto &robot) {
        return robot.message.find("timed out") != std::string::npos;
      }));
  EXPECT_GT(first.runtime->last_prepare_timeout(),
            std::chrono::milliseconds(0));
  EXPECT_LE(first.runtime->last_prepare_timeout(),
            std::chrono::milliseconds(250));
  EXPECT_EQ(first.deployment->current().phase,
            vektor::DeploymentPhase::RolledBack);
  EXPECT_EQ(second.deployment->current().phase,
            vektor::DeploymentPhase::RolledBack);
  EXPECT_EQ(first.runtime->observation().artifact, kPreviousArtifact);
  EXPECT_EQ(second.runtime->observation().artifact, kPreviousArtifact);
  EXPECT_TRUE(first.runtime->observation().ready);
  EXPECT_TRUE(second.runtime->observation().ready);
  EXPECT_FALSE(std::filesystem::exists(files.state));
}
