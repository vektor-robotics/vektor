#include "vektor/agent.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <future>
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

class AuditRuntime final : public vektor::RuntimeDriver {
public:
  void prepare(const std::string &) override { ++prepare_calls; }
  vektor::RuntimeObservation activate(const std::string &,
                                      const vektor::WorkloadSpec &) override {
    return {};
  }
  vektor::RuntimeObservation stop() override { return {}; }
  vektor::RuntimeObservation inspect() override { return {}; }
  int prepare_calls{0};
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
  options.insecure = true;
  EXPECT_NO_THROW(vektor::validate_agent_options(options));
}

TEST(AgentOptions, RejectsInsecurePublicListener) {
  vektor::AgentOptions options;
  options.insecure = true;
  options.listen_address = "0.0.0.0:50051";
  EXPECT_THROW(vektor::validate_agent_options(options), std::invalid_argument);
}

TEST(AgentOptions, RequiresCompleteMutualTlsConfiguration) {
  vektor::AgentOptions options;
  options.tls_certificate = "server.crt";
  EXPECT_THROW(vektor::validate_agent_options(options), std::invalid_argument);
  options.tls_private_key = "server.key";
  options.tls_client_ca = "client-ca.crt";
  EXPECT_NO_THROW(vektor::validate_agent_options(options));
}

TEST(AgentOptions, RejectsInvalidRuntimeContainerName) {
  vektor::AgentOptions options;
  options.insecure = true;
  options.runtime_container = "invalid name";
  EXPECT_THROW(vektor::validate_agent_options(options), std::invalid_argument);
}

TEST(AgentOptions, RequiresAuditLogPath) {
  vektor::AgentOptions options;
  options.insecure = true;
  options.audit_log_path.clear();
  EXPECT_THROW(vektor::validate_agent_options(options), std::invalid_argument);
}

TEST(AgentOptions, RequiresMutualTlsForAuthorizationPolicy) {
  vektor::AgentOptions options;
  options.insecure = true;
  options.authorization_policy_path = "authorization.yaml";
  EXPECT_THROW(vektor::validate_agent_options(options), std::invalid_argument);
}

TEST(AgentOptions, RequiresServerBoundScopeForAuthorizationPolicy) {
  vektor::AgentOptions options;
  options.authorization_policy_path = "authorization.yaml";
  options.tls_certificate = "server.crt";
  options.tls_private_key = "server.key";
  options.tls_client_ca = "client-ca.crt";
  EXPECT_THROW(vektor::validate_agent_options(options), std::invalid_argument);
  options.resource_scope = {"warehouse-prod", "picker"};
  EXPECT_NO_THROW(vektor::validate_agent_options(options));
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
