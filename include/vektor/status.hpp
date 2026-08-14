#pragma once

#include "vektor/health_inspector.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace vektor {

enum class HealthState { Healthy, Degraded, Unhealthy, Unreachable };

struct StatusSnapshot {
  std::string timestamp;
  std::string robot_id;
  std::string hostname;
  int ros_domain_id{0};
  HealthState state{HealthState::Unreachable};
  std::chrono::milliseconds duration{0};
  std::vector<CheckResult> checks;
};

HealthState derive_health_state(const std::vector<CheckResult> &results);
const char *health_state_name(HealthState state);
StatusSnapshot make_status_snapshot(std::string robot_id,
                                    std::vector<CheckResult> results,
                                    std::chrono::milliseconds duration);
void print_status(const StatusSnapshot &snapshot, std::ostream &out);
std::string status_to_json(const StatusSnapshot &snapshot);
std::filesystem::path default_status_history_path();

class SnapshotStore {
public:
  explicit SnapshotStore(std::filesystem::path path,
                         std::size_t max_entries = 100);
  void append(const StatusSnapshot &snapshot) const;
  const std::filesystem::path &path() const;

private:
  std::filesystem::path path_;
  std::size_t max_entries_;
};

} // namespace vektor
