#pragma once

#include "vektor/agent/v1/agent.grpc.pb.h"
#include "vektor/status.hpp"

#include <grpcpp/channel.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace vektor {

struct FleetTransportConfig {
  bool insecure{false};
  std::optional<std::filesystem::path> ca_certificate;
  std::optional<std::filesystem::path> client_certificate;
  std::optional<std::filesystem::path> client_private_key;
};

struct FleetRobotConfig {
  std::string id;
  std::string endpoint;
  std::map<std::string, std::string> labels;
  std::string tls_server_name;
};

struct FleetConfig {
  std::string fleet_id;
  std::chrono::milliseconds request_timeout{2000};
  std::chrono::milliseconds max_snapshot_age{15000};
  // Bound client-side fan-out so large inventories cannot create one thread
  // per robot. Requests within a batch still run concurrently.
  std::size_t max_concurrent_requests{32};
  FleetTransportConfig transport;
  std::vector<FleetRobotConfig> robots;
};

struct LabelSelector {
  std::string key;
  std::string value;
};

struct FleetRobotStatus {
  FleetRobotConfig robot;
  HealthState state{HealthState::Unreachable};
  std::optional<vektor::agent::v1::StatusSnapshot> snapshot;
  std::string error;
};

struct FleetReport {
  std::string timestamp;
  std::string fleet_id;
  std::size_t inventory_size{0};
  HealthState state{HealthState::Unreachable};
  std::vector<FleetRobotStatus> robots;
};

FleetConfig load_fleet_config(const std::string &path);
std::shared_ptr<grpc::Channel>
make_fleet_channel(const FleetRobotConfig &robot,
                   const FleetTransportConfig &transport);
LabelSelector parse_label_selector(const std::string &value);
std::vector<FleetRobotConfig>
select_fleet_robots(const FleetConfig &config,
                    const std::vector<LabelSelector> &selectors,
                    std::optional<std::size_t> limit = std::nullopt);
FleetReport poll_fleet(const FleetConfig &config,
                       const std::vector<LabelSelector> &selectors = {},
                       std::optional<std::size_t> limit = std::nullopt);
HealthState aggregate_fleet_state(const std::vector<FleetRobotStatus> &robots);
void print_fleet_report(const FleetReport &report, std::ostream &out);
std::string fleet_report_to_json(const FleetReport &report);

} // namespace vektor
