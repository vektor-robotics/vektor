#pragma once

#include "vektor/fleet.hpp"
#include "vektor/runtime.hpp"

#include <chrono>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace vektor {

struct DeploymentWave {
  std::string name;
  std::vector<LabelSelector> selectors;
  std::optional<std::size_t> limit;
};

struct RolloutConfig {
  std::string deployment_id;
  std::string workload_id;
  std::string environment{"development"};
  std::string artifact;
  WorkloadSpec workload;
  std::filesystem::path fleet_config_path;
  std::filesystem::path state_path;
  std::optional<std::filesystem::path> approval_policy_path;
  std::optional<std::filesystem::path> approval_file_path;
  std::chrono::milliseconds operation_timeout{300000};
  std::chrono::milliseconds readiness_timeout{30000};
  std::chrono::milliseconds settle_time{5000};
  bool allow_degraded{false};
  std::vector<DeploymentWave> waves;
};

struct RolloutRobotResult {
  std::string robot_id;
  bool success{false};
  std::string message;
  std::string phase;
  std::string operation;
};

struct RolloutReport {
  std::string deployment_id;
  std::string action;
  std::string wave;
  bool success{false};
  bool complete{false};
  std::size_t next_wave{0};
  std::vector<RolloutRobotResult> robots;
  std::string message;
  bool approval_required{false};
  std::vector<std::string> approvers;
};

RolloutConfig load_rollout_config(const std::string &path);
RolloutReport deploy_release(const RolloutConfig &config);
RolloutReport promote_release(const RolloutConfig &config);
RolloutReport rollback_release(const RolloutConfig &config);
void print_rollout_report(const RolloutReport &report, std::ostream &out);
std::string rollout_report_to_json(const RolloutReport &report);

} // namespace vektor
