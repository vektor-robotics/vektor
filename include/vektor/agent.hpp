#pragma once

#include "vektor/agent/v1/agent.grpc.pb.h"
#include "vektor/config.hpp"
#include "vektor/status.hpp"

#include <grpcpp/grpcpp.h>
#include <rclcpp/rclcpp.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace vektor {

struct AgentOptions {
  std::string listen_address{"127.0.0.1:50051"};
  std::chrono::milliseconds interval{5000};
  std::optional<std::filesystem::path> history_path;
  bool history{true};
  bool insecure{false};
  std::optional<std::filesystem::path> tls_certificate;
  std::optional<std::filesystem::path> tls_private_key;
  std::optional<std::filesystem::path> tls_client_ca;
};

void validate_agent_options(const AgentOptions &options);

struct VersionedSnapshot {
  StatusSnapshot snapshot;
  std::uint64_t sequence{0};
};

class AgentStatusState {
public:
  void publish(StatusSnapshot snapshot);
  std::optional<VersionedSnapshot> latest() const;
  std::optional<VersionedSnapshot>
  wait_for_change(std::uint64_t after_sequence,
                  std::chrono::milliseconds timeout) const;
  void stop();

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::optional<StatusSnapshot> latest_;
  std::uint64_t sequence_{0};
  bool stopped_{false};
};

vektor::agent::v1::StatusSnapshot to_proto(const VersionedSnapshot &snapshot);

class GrpcAgentService final : public vektor::agent::v1::Agent::Service {
public:
  explicit GrpcAgentService(const AgentStatusState &state);

  grpc::Status GetStatus(grpc::ServerContext *context,
                         const vektor::agent::v1::GetStatusRequest *request,
                         vektor::agent::v1::StatusSnapshot *response) override;
  grpc::Status WatchStatus(
      grpc::ServerContext *context,
      const vektor::agent::v1::WatchStatusRequest *request,
      grpc::ServerWriter<vektor::agent::v1::StatusSnapshot> *writer) override;

private:
  const AgentStatusState &state_;
};

class AgentRunner {
public:
  AgentRunner(rclcpp::Node::SharedPtr node, CheckConfig config,
              std::string robot_id, AgentOptions options);
  int run();

private:
  rclcpp::Node::SharedPtr node_;
  CheckConfig config_;
  std::string robot_id_;
  AgentOptions options_;
  AgentStatusState state_;
};

} // namespace vektor
