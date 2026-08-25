#include "vektor/agent.hpp"

#include <gtest/gtest.h>

#include <google/protobuf/descriptor.h>

#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
vektor::StatusSnapshot sample_snapshot() {
  vektor::StatusSnapshot snapshot;
  snapshot.timestamp = "2026-08-14T12:00:00Z";
  snapshot.robot_id = "robot-007";
  snapshot.hostname = "test-host";
  snapshot.ros_domain_id = 42;
  snapshot.state = vektor::HealthState::Degraded;
  snapshot.duration = std::chrono::milliseconds(17);
  snapshot.checks.push_back({vektor::CheckStatus::Warn, "topic", "/camera",
                             "frequency below target",
                             std::chrono::milliseconds(12)});
  return snapshot;
}

constexpr auto kArtifact =
    "ghcr.io/vektor-robotics/demo@sha256:"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr auto kArtifactB =
    "ghcr.io/vektor-robotics/demo@sha256:"
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr auto kArtifactC =
    "ghcr.io/vektor-robotics/demo@sha256:"
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

class AuditRuntime final : public vektor::RuntimeDriver {
public:
  void prepare(const std::string &artifact) override {
    if (fail_prepare)
      throw std::runtime_error("simulated workload pull failure");
    ++prepare_calls;
    last_artifact = artifact;
  }
  vektor::RuntimeObservation activate(
      const std::string &artifact, const vektor::WorkloadSpec &spec) override {
    ++activate_calls;
    last_artifact = artifact;
    return {true, true, artifact, "runtime-" + artifact.substr(artifact.size() - 8),
            true, vektor::workload_fingerprint(spec), "healthy"};
  }
  vektor::RuntimeObservation stop() override {
    ++stop_calls;
    return {};
  }
  vektor::RuntimeObservation inspect() override {
    return {activate_calls > stop_calls, activate_calls > stop_calls,
            activate_calls > stop_calls ? last_artifact : "",
            activate_calls > stop_calls ? "runtime" : "", activate_calls > stop_calls,
            inspection_fingerprint.empty() ? vektor::workload_fingerprint({})
                                            : inspection_fingerprint,
            activate_calls > stop_calls ? "healthy" : "stopped"};
  }
  int prepare_calls{0};
  int activate_calls{0};
  int stop_calls{0};
  std::string last_artifact;
  bool fail_prepare{false};
  std::string inspection_fingerprint;
};

class RecordingAudit final : public vektor::AuditSink {
public:
  void append(const vektor::AuditEvent &event) override {
    events.push_back(event);
  }
  std::vector<vektor::AuditEvent> events;
};
} // namespace

TEST(AgentOptions, AllowsExplicitLoopbackInsecureMode) {
  vektor::AgentOptions options;
  options.health_policy_path = "policy.yaml";
  options.insecure = true;
  EXPECT_NO_THROW(vektor::validate_agent_options(options));
}

TEST(AgentDeployment, HandlesConcurrentThreeWorkloadPartialFailureAndDrift) {
  const auto path = std::filesystem::path("vektor_test_agent_concurrent.yaml");
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".workloads");
  std::map<std::string, std::shared_ptr<AuditRuntime>> runtimes{
      {"camera", std::make_shared<AuditRuntime>()},
      {"navigation", std::make_shared<AuditRuntime>()},
      {"lidar", std::make_shared<AuditRuntime>()}};
  vektor::WorkloadDeploymentStates deployments(
      path, [&runtimes](const std::string &id) {
        auto runtime = runtimes.at(id);
        runtime->fail_prepare = id == "lidar";
        return runtime;
      });
  const auto prepare = [&deployments](const std::string &id,
                                      const std::string &artifact) {
    auto &state = deployments.for_workload(id);
    try {
      return state.prepare(id + "-release", artifact);
    } catch (...) {
      return state.current();
    }
  };
  auto camera = std::async(std::launch::async, prepare, "camera", kArtifact);
  auto navigation =
      std::async(std::launch::async, prepare, "navigation", kArtifactB);
  auto lidar = std::async(std::launch::async, prepare, "lidar", kArtifactC);
  EXPECT_EQ(camera.get().phase, vektor::DeploymentPhase::Staged);
  EXPECT_EQ(navigation.get().phase, vektor::DeploymentPhase::Staged);
  EXPECT_EQ(lidar.get().phase, vektor::DeploymentPhase::Failed);
  ASSERT_EQ(deployments.for_workload("camera").activate("camera-release").phase,
            vektor::DeploymentPhase::Active);
  runtimes.at("camera")->inspection_fingerprint = "tampered";
  EXPECT_TRUE(deployments.for_workload("camera").refresh_observed().drift_detected);
  EXPECT_EQ(runtimes.at("camera")->prepare_calls, 1);
  EXPECT_EQ(runtimes.at("navigation")->prepare_calls, 1);
  EXPECT_EQ(runtimes.at("lidar")->prepare_calls, 0);
  std::filesystem::remove(path.string() + ".workloads");
  std::filesystem::remove(vektor::workload_state_path(path, "camera"));
  std::filesystem::remove(vektor::workload_state_path(path, "navigation"));
  std::filesystem::remove(vektor::workload_state_path(path, "lidar"));
}

TEST(OperationalMetrics, RendersBoundedPrometheusCounters) {
  vektor::OperationalMetrics metrics;
  metrics.record_health(vektor::HealthState::Healthy);
  metrics.record_authorization_denial();
  metrics.record_reconciliation(false);
  metrics.record_rollout(true);
  metrics.record_rpc(std::chrono::milliseconds(7));
  const auto rendered = metrics.prometheus();
  EXPECT_NE(rendered.find("vektor_health_inspections_total{state=\"healthy\"} 1"),
            std::string::npos);
  EXPECT_NE(rendered.find("vektor_authorization_denials_total 1"),
            std::string::npos);
  EXPECT_NE(rendered.find("vektor_reconciliation_total{outcome=\"failure\"} 1"),
            std::string::npos);
  EXPECT_NE(rendered.find("vektor_rollout_total{outcome=\"success\"} 1"),
            std::string::npos);
  EXPECT_NE(rendered.find("vektor_rpc_latency_milliseconds_total 7"),
            std::string::npos);
}

TEST(AgentOptions, RejectsInsecurePublicListener) {
  vektor::AgentOptions options;
  options.health_policy_path = "policy.yaml";
  options.insecure = true;
  options.listen_address = "0.0.0.0:50051";
  EXPECT_THROW(vektor::validate_agent_options(options), std::invalid_argument);
}

TEST(AgentOptions, RequiresCompleteMutualTlsConfiguration) {
  vektor::AgentOptions options;
  options.health_policy_path = "policy.yaml";
  options.tls_certificate = "server.crt";
  EXPECT_THROW(vektor::validate_agent_options(options), std::invalid_argument);
  options.tls_private_key = "server.key";
  options.tls_client_ca = "client-ca.crt";
  EXPECT_NO_THROW(vektor::validate_agent_options(options));
}

TEST(AgentOptions, RejectsInvalidRuntimeContainerName) {
  vektor::AgentOptions options;
  options.health_policy_path = "policy.yaml";
  options.insecure = true;
  options.runtime_container = "invalid name";
  EXPECT_THROW(vektor::validate_agent_options(options), std::invalid_argument);
}

TEST(AgentOptions, RequiresAuditLogPath) {
  vektor::AgentOptions options;
  options.health_policy_path = "policy.yaml";
  options.insecure = true;
  options.audit_log_path.clear();
  EXPECT_THROW(vektor::validate_agent_options(options), std::invalid_argument);
}

TEST(AgentOptions, RequiresMutualTlsForAuthorizationPolicy) {
  vektor::AgentOptions options;
  options.health_policy_path = "policy.yaml";
  options.insecure = true;
  options.authorization_policy_path = "authorization.yaml";
  EXPECT_THROW(vektor::validate_agent_options(options), std::invalid_argument);
}

TEST(AgentOptions, RequiresServerBoundScopeForAuthorizationPolicy) {
  vektor::AgentOptions options;
  options.health_policy_path = "policy.yaml";
  options.authorization_policy_path = "authorization.yaml";
  options.tls_certificate = "server.crt";
  options.tls_private_key = "server.key";
  options.tls_client_ca = "client-ca.crt";
  EXPECT_THROW(vektor::validate_agent_options(options), std::invalid_argument);
  options.resource_scope = {"warehouse-prod", "picker"};
  EXPECT_NO_THROW(vektor::validate_agent_options(options));
  options.resource_scope = {"warehouse-prod", ""};
  EXPECT_NO_THROW(vektor::validate_agent_options(options));
}

TEST(AgentPolicy, KeepsLastKnownGoodConfigWhenReplacementIsInvalid) {
  const auto path = std::filesystem::path("vektor_test_reload_policy.yaml");
  {
    std::ofstream output(path);
    output << "schema_version: 1\nrobot_id: before\n";
  }
  vektor::ReloadableCheckConfig policy(path, vektor::load_config(path.string()));
  {
    std::ofstream output(path);
    output << "schema_version: 1\nrobot_id: after\n";
  }
  policy.replace(policy.load_candidate());
  EXPECT_EQ(policy.current().robot_id, "after");
  {
    std::ofstream output(path);
    output << "schema_version: 99\nrobot_id: rejected\n";
  }
  EXPECT_THROW(policy.load_candidate(), std::runtime_error);
  EXPECT_EQ(policy.current().robot_id, "after");
  std::filesystem::remove(path);
}

TEST(AgentStatusState, PublishesAndWaitsForVersionedSnapshots) {
  vektor::AgentStatusState state;
  EXPECT_FALSE(state.latest().has_value());

  state.publish(sample_snapshot());
  const auto first = state.latest();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->sequence, 1U);
  EXPECT_EQ(first->snapshot.robot_id, "robot-007");

  auto next = std::async(std::launch::async, [&] {
    return state.wait_for_change(first->sequence,
                                 std::chrono::milliseconds(500));
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  auto updated = sample_snapshot();
  updated.state = vektor::HealthState::Healthy;
  state.publish(std::move(updated));

  const auto second = next.get();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->sequence, 2U);
  EXPECT_EQ(second->snapshot.state, vektor::HealthState::Healthy);
}

TEST(AgentProto, PreservesSnapshotFields) {
  const auto proto =
      vektor::to_proto(vektor::VersionedSnapshot{sample_snapshot(), 9});
  EXPECT_EQ(proto.schema_version(), 1U);
  EXPECT_EQ(proto.sequence(), 9U);
  EXPECT_EQ(proto.robot_id(), "robot-007");
  EXPECT_EQ(proto.ros_domain_id(), 42);
  EXPECT_EQ(proto.state(), vektor::agent::v1::HEALTH_STATE_DEGRADED);
  ASSERT_EQ(proto.checks_size(), 1);
  EXPECT_EQ(proto.checks(0).status(), vektor::agent::v1::CHECK_STATUS_WARN);
  EXPECT_EQ(proto.checks(0).duration_ms(), 12);
}

TEST(AgentApiCompatibility, PreservesV1WireContract) {
  const auto *snapshot =
      vektor::agent::v1::StatusSnapshot::descriptor();
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(snapshot->FindFieldByName("schema_version")->number(), 1);
  EXPECT_EQ(snapshot->FindFieldByName("robot_id")->number(), 3);
  EXPECT_EQ(snapshot->FindFieldByName("checks")->number(), 8);
  EXPECT_EQ(snapshot->FindFieldByName("sequence")->number(), 9);

  const auto *deployment =
      vektor::agent::v1::DeploymentRecord::descriptor();
  ASSERT_NE(deployment, nullptr);
  EXPECT_EQ(deployment->FindFieldByName("schema_version")->number(), 1);
  EXPECT_EQ(deployment->FindFieldByName("artifact")->number(), 3);
  EXPECT_EQ(deployment->FindFieldByName("reconciliation_operation")->number(),
            19);
  EXPECT_EQ(deployment->FindFieldByName("verified_at")->number(), 26);

  const auto *service = google::protobuf::DescriptorPool::generated_pool()
                            ->FindServiceByName("vektor.agent.v1.Agent");
  ASSERT_NE(service, nullptr);
  ASSERT_EQ(service->method_count(), 6);
  EXPECT_EQ(service->method(0)->name(), "GetStatus");
  EXPECT_EQ(service->method(1)->name(), "WatchStatus");
  EXPECT_TRUE(service->method(1)->server_streaming());
  EXPECT_EQ(service->method(5)->name(), "GetDeployment");
}

TEST(AgentGrpc, ServesLatestSnapshot) {
  vektor::AgentStatusState state;
  state.publish(sample_snapshot());
  vektor::GrpcAgentService service(state);

  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);
  ASSERT_GT(port, 0);

  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                     grpc::InsecureChannelCredentials());
  auto stub = vektor::agent::v1::Agent::NewStub(channel);
  grpc::ClientContext context;
  vektor::agent::v1::GetStatusRequest request;
  vektor::agent::v1::StatusSnapshot response;
  const auto status = stub->GetStatus(&context, request, &response);

  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response.robot_id(), "robot-007");
  EXPECT_EQ(response.sequence(), 1U);
  server->Shutdown();
}

TEST(AgentGrpc, LabelsInsecureDeploymentActorAsUnauthenticatedPeer) {
  const auto path = std::filesystem::path("vektor_test_agent_audit.yaml");
  std::filesystem::remove(path);
  vektor::AgentStatusState health;
  health.publish(sample_snapshot());
  auto runtime = std::make_shared<AuditRuntime>();
  auto audit = std::make_shared<RecordingAudit>();
  vektor::AgentDeploymentState deployment(path, runtime, nullptr, audit);
  vektor::GrpcAgentService service(health, deployment);

  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);
  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                     grpc::InsecureChannelCredentials());
  auto stub = vektor::agent::v1::Agent::NewStub(channel);
  grpc::ClientContext context;
  vektor::agent::v1::PrepareDeploymentRequest request;
  request.set_deployment_id("release-1");
  request.set_artifact(kArtifact);
  vektor::agent::v1::DeploymentRecord response;

  const auto status = stub->PrepareDeployment(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(runtime->prepare_calls, 1);
  ASSERT_FALSE(audit->events.empty());
  EXPECT_TRUE(audit->events.front().actor.starts_with("unauthenticated:"));
  EXPECT_NE(audit->events.front().actor.find("127.0.0.1"), std::string::npos);
  server->Shutdown();
  std::filesystem::remove(path);
}

TEST(AgentGrpc, IsolatesPreparedDeploymentsByWorkloadScope) {
  const auto path = std::filesystem::path("vektor_test_agent_workloads.yaml");
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".workloads");
  vektor::AgentStatusState health;
  health.publish(sample_snapshot());
  std::map<std::string, std::shared_ptr<AuditRuntime>> runtimes;
  vektor::WorkloadDeploymentStates deployments(
      path, [&runtimes](const std::string &id) {
        auto runtime = std::make_shared<AuditRuntime>();
        runtimes.emplace(id, runtime);
        return runtime;
      });
  vektor::GrpcAgentService service(health, deployments);
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);
  auto stub = vektor::agent::v1::Agent::NewStub(
      grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  for (const auto &[id, artifact] : std::vector<std::pair<std::string, std::string>>{{"camera", kArtifact}, {"navigation", kArtifactB}, {"lidar", kArtifactC}}) {
    grpc::ClientContext context;
    vektor::agent::v1::PrepareDeploymentRequest request;
    request.set_deployment_id(id + "-release");
    request.set_artifact(artifact);
    request.mutable_scope()->set_workload_id(id);
    vektor::agent::v1::DeploymentRecord response;
    EXPECT_TRUE(stub->PrepareDeployment(&context, request, &response).ok());
  }
  EXPECT_EQ(runtimes.at("camera")->last_artifact, kArtifact);
  EXPECT_EQ(runtimes.at("navigation")->last_artifact, kArtifactB);
  EXPECT_EQ(runtimes.at("lidar")->last_artifact, kArtifactC);

  for (const auto &id : {std::string("camera"), std::string("navigation"),
                         std::string("lidar")}) {
    std::thread health_update([&health] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      auto snapshot = sample_snapshot();
      snapshot.state = vektor::HealthState::Healthy;
      health.publish(std::move(snapshot));
    });
    grpc::ClientContext context;
    vektor::agent::v1::ActivateDeploymentRequest request;
    request.set_deployment_id(id + "-release");
    request.set_allow_degraded(true);
    request.mutable_scope()->set_workload_id(id);
    vektor::agent::v1::DeploymentRecord response;
    const auto status = stub->ActivateDeployment(&context, request, &response);
    health_update.join();
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.phase(),
              vektor::agent::v1::DEPLOYMENT_PHASE_ACTIVE);
  }
  EXPECT_EQ(runtimes.at("camera")->activate_calls, 1);
  EXPECT_EQ(runtimes.at("navigation")->activate_calls, 1);
  EXPECT_EQ(runtimes.at("lidar")->activate_calls, 1);

  {
    grpc::ClientContext context;
    vektor::agent::v1::RollbackDeploymentRequest request;
    request.set_deployment_id("camera-release");
    request.mutable_scope()->set_workload_id("camera");
    vektor::agent::v1::DeploymentRecord response;
    const auto status = stub->RollbackDeployment(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.phase(), vektor::agent::v1::DEPLOYMENT_PHASE_ROLLED_BACK);
  }
  EXPECT_EQ(runtimes.at("camera")->stop_calls, 1);
  EXPECT_EQ(runtimes.at("navigation")->stop_calls, 0);
  EXPECT_EQ(runtimes.at("lidar")->stop_calls, 0);
  server->Shutdown();
  std::filesystem::remove(path.string() + ".workloads");
  std::filesystem::remove(vektor::workload_state_path(path, "camera"));
  std::filesystem::remove(vektor::workload_state_path(path, "navigation"));
  std::filesystem::remove(vektor::workload_state_path(path, "lidar"));
}
