#include "vektor/status.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace vektor {
namespace {
std::string json_string(std::string_view value) {
  std::ostringstream out;
  out << '"';
  for (const char character : value) {
    switch (character) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      out << character;
    }
  }
  out << '"';
  return out.str();
}

std::string utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string hostname() {
  char value[256]{};
  if (gethostname(value, sizeof(value) - 1) != 0)
    return "unknown";
  return value;
}

int ros_domain_id() {
  const char *value = std::getenv("ROS_DOMAIN_ID");
  if (!value || std::string(value).empty())
    return 0;
  try {
    return std::stoi(value);
  } catch (const std::exception &) {
    return 0;
  }
}

const char *check_status_name(CheckStatus status) {
  if (status == CheckStatus::Pass)
    return "pass";
  if (status == CheckStatus::Warn)
    return "warn";
  return "fail";
}
} // namespace

HealthState derive_health_state(const std::vector<CheckResult> &results) {
  if (results.empty())
    return HealthState::Unreachable;
  if (std::any_of(results.begin(), results.end(), [](const auto &result) {
        return result.status == CheckStatus::Fail;
      }))
    return HealthState::Unhealthy;
  if (std::any_of(results.begin(), results.end(), [](const auto &result) {
        return result.status == CheckStatus::Warn;
      }))
    return HealthState::Degraded;
  return HealthState::Healthy;
}

const char *health_state_name(HealthState state) {
  switch (state) {
  case HealthState::Healthy:
    return "healthy";
  case HealthState::Degraded:
    return "degraded";
  case HealthState::Unhealthy:
    return "unhealthy";
  case HealthState::Unreachable:
    return "unreachable";
  }
  return "unreachable";
}

StatusSnapshot make_status_snapshot(std::string robot_id,
                                    std::vector<CheckResult> results,
                                    std::chrono::milliseconds duration) {
  StatusSnapshot snapshot;
  snapshot.timestamp = utc_timestamp();
  snapshot.hostname = hostname();
  snapshot.robot_id =
      robot_id.empty() ? snapshot.hostname : std::move(robot_id);
  snapshot.ros_domain_id = ros_domain_id();
  snapshot.state = derive_health_state(results);
  snapshot.duration = duration;
  snapshot.checks = std::move(results);
  return snapshot;
}

void print_status(const StatusSnapshot &snapshot, std::ostream &out) {
  out << "VEKTOR STATUS: " << health_state_name(snapshot.state) << '\n'
      << "robot: " << snapshot.robot_id << '\n'
      << "host: " << snapshot.hostname << '\n'
      << "timestamp: " << snapshot.timestamp << '\n'
      << "ros_domain_id: " << snapshot.ros_domain_id << '\n'
      << "duration_ms: " << snapshot.duration.count() << "\n\n";
  for (const auto &check : snapshot.checks) {
    out << '[' << check_status_name(check.status) << "] " << check.category
        << ' ' << check.target << ": " << check.message << " ("
        << check.duration.count() << " ms)\n";
  }
}

std::string status_to_json(const StatusSnapshot &snapshot) {
  std::ostringstream out;
  out << "{\"schema_version\":1,\"timestamp\":"
      << json_string(snapshot.timestamp)
      << ",\"robot_id\":" << json_string(snapshot.robot_id)
      << ",\"hostname\":" << json_string(snapshot.hostname)
      << ",\"ros_domain_id\":" << snapshot.ros_domain_id
      << ",\"state\":" << json_string(health_state_name(snapshot.state))
      << ",\"duration_ms\":" << snapshot.duration.count() << ",\"checks\":[";
  for (std::size_t index = 0; index < snapshot.checks.size(); ++index) {
    if (index > 0)
      out << ',';
    const auto &check = snapshot.checks[index];
    out << "{\"status\":" << json_string(check_status_name(check.status))
        << ",\"category\":" << json_string(check.category)
        << ",\"target\":" << json_string(check.target)
        << ",\"message\":" << json_string(check.message)
        << ",\"duration_ms\":" << check.duration.count() << '}';
  }
  out << "]}";
  return out.str();
}

std::filesystem::path default_status_history_path() {
  if (const char *state_home = std::getenv("XDG_STATE_HOME"))
    return std::filesystem::path(state_home) / "vektor" / "status.jsonl";
  if (const char *home = std::getenv("HOME"))
    return std::filesystem::path(home) / ".local" / "state" / "vektor" /
           "status.jsonl";
  return std::filesystem::path(".vektor") / "status.jsonl";
}

SnapshotStore::SnapshotStore(std::filesystem::path path,
                             std::size_t max_entries)
    : path_(std::move(path)), max_entries_(max_entries) {
  if (max_entries_ == 0)
    throw std::invalid_argument(
        "snapshot history size must be greater than zero");
}

void SnapshotStore::append(const StatusSnapshot &snapshot) const {
  std::vector<std::string> lines;
  if (std::ifstream input(path_); input) {
    std::string line;
    while (std::getline(input, line)) {
      if (!line.empty())
        lines.push_back(std::move(line));
    }
  }
  lines.push_back(status_to_json(snapshot));
  if (lines.size() > max_entries_)
    lines.erase(lines.begin(), lines.end() - max_entries_);

  if (!path_.parent_path().empty())
    std::filesystem::create_directories(path_.parent_path());
  std::ofstream output(path_, std::ios::trunc);
  if (!output)
    throw std::runtime_error("failed to write status history '" +
                             path_.string() + "'");
  for (const auto &line : lines)
    output << line << '\n';
}

const std::filesystem::path &SnapshotStore::path() const { return path_; }

} // namespace vektor
