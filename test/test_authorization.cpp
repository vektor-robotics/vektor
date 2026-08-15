#include "vektor/agent.hpp"
#include "vektor/authorization.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
struct PolicyFile {
  std::filesystem::path path{"vektor_test_authorization.yaml"};
  PolicyFile() { std::filesystem::remove(path); }
  ~PolicyFile() { std::filesystem::remove(path); }
};

class RecordingRuntime final : public vektor::RuntimeDriver {
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

constexpr auto kArtifact =
    "ghcr.io/vektor-robotics/demo@sha256:"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
} // namespace

TEST(AuthorizationPolicy, AppliesFixedRoleCapabilities) {
  PolicyFile file;
  {
    std::ofstream policy(file.path);
    policy << "schema_version: 1\nidentities:\n"
              "  - identity: viewer@example.com\n    roles: [viewer]\n"
              "  - identity: release@example.com\n    roles: [deployer]\n"
              "  - identity: sre@example.com\n    roles: [operator]\n";
  }
  const auto policy = vektor::load_authorization_policy(file.path);

  EXPECT_TRUE(policy.allows("viewer@example.com",
                            vektor::AuthorizationAction::Inspect));
  EXPECT_FALSE(
      policy.allows("viewer@example.com", vektor::AuthorizationAction::Deploy));
  EXPECT_TRUE(policy.allows("release@example.com",
                            vektor::AuthorizationAction::Deploy));
  EXPECT_TRUE(policy.allows("release@example.com",
                            vektor::AuthorizationAction::Promote));
  EXPECT_FALSE(policy.allows("release@example.com",
                             vektor::AuthorizationAction::Rollback));
  EXPECT_TRUE(
      policy.allows("sre@example.com", vektor::AuthorizationAction::Rollback));
  EXPECT_FALSE(policy.allows("unknown@example.com",
                             vektor::AuthorizationAction::Inspect));
}

TEST(AuthorizationPolicy, RejectsUnknownFieldsRolesAndDuplicateIdentities) {
  PolicyFile file;
  {
    std::ofstream policy(file.path);
    policy << "schema_version: 1\nidentities:\n"
              "  - identity: user@example.com\n    roles: [viewer]\n"
              "unexpected: true\n";
  }
  EXPECT_THROW(vektor::load_authorization_policy(file.path),
               std::runtime_error);
  {
    std::ofstream policy(file.path);
    policy << "schema_version: 1\nidentities:\n"
              "  - identity: user@example.com\n    roles: [owner]\n";
  }
  EXPECT_THROW(vektor::load_authorization_policy(file.path),
               std::runtime_error);
  {
    std::ofstream policy(file.path);
    policy << "schema_version: 1\nidentities:\n"
              "  - identity: user@example.com\n    roles: [viewer]\n"
              "  - identity: user@example.com\n    roles: [operator]\n";
  }
  EXPECT_THROW(vektor::load_authorization_policy(file.path),
               std::runtime_error);
}

TEST(AuthorizationPolicy, ProducesStableMachineReadableDenial) {
  EXPECT_EQ(
      vektor::authorization_denial_json(vektor::AuthorizationAction::Rollback),
      "{\"schema_version\":1,\"code\":"
      "\"VEKTOR_AUTHORIZATION_DENIED\",\"action\":\"rollback\"}");
}

TEST(AuthorizationGrpc, DeniesUnauthenticatedMutationBeforeRuntimeAndAudits) {
  PolicyFile file;
  {
    std::ofstream policy(file.path);
    policy << "schema_version: 1\nidentities:\n"
              "  - identity: release@example.com\n    roles: [deployer]\n";
  }
  auto authorization = std::make_shared<const vektor::AuthorizationPolicy>(
      vektor::load_authorization_policy(file.path));
  const auto state_path =
      std::filesystem::path("vektor_test_authorization_state.yaml");
  std::filesystem::remove(state_path);
  vektor::AgentStatusState health;
  auto runtime = std::make_shared<RecordingRuntime>();
  auto audit = std::make_shared<RecordingAudit>();
  vektor::AgentDeploymentState deployment(state_path, runtime, nullptr, audit);
  vektor::GrpcAgentService service(health, deployment, authorization);

  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);
  auto stub = vektor::agent::v1::Agent::NewStub(grpc::CreateChannel(
      "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  vektor::agent::v1::PrepareDeploymentRequest request;
  request.set_deployment_id("release-1");
  request.set_artifact(kArtifact);
  vektor::agent::v1::DeploymentRecord response;

  const auto status = stub->PrepareDeployment(&context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
  EXPECT_EQ(status.error_message(), vektor::authorization_denial_json(
                                        vektor::AuthorizationAction::Deploy));
  EXPECT_EQ(runtime->prepare_calls, 0);
  ASSERT_EQ(audit->events.size(), 1U);
  EXPECT_EQ(audit->events.front().action, "authorization.deploy");
  EXPECT_EQ(audit->events.front().outcome, "denied");
  EXPECT_TRUE(audit->events.front().actor.starts_with("unauthenticated:"));

  server->Shutdown();
  std::filesystem::remove(state_path);
}
