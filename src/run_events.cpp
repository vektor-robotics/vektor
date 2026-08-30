#include "vektor/run_events.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace vektor {
namespace {
constexpr std::uintmax_t kMaxSourceBytes = 1024 * 1024;

struct BoundedLines {
  std::vector<std::string> lines;
  bool truncated{false};
  bool rotated{false};
};

void validate_source_file(const std::filesystem::path &path) {
  if (!std::filesystem::exists(path))
    return;
  if (std::filesystem::is_symlink(path) ||
      !std::filesystem::is_regular_file(path))
    throw std::runtime_error("run event source is not a regular file: " +
                             path.string());
}

BoundedLines read_bounded_lines(const std::filesystem::path &path,
                                std::uintmax_t requested_offset,
                                bool tail_window) {
  BoundedLines result;
  validate_source_file(path);
  if (!std::filesystem::exists(path))
    return result;
  const auto size = std::filesystem::file_size(path);
  std::uintmax_t offset = requested_offset;
  bool discard_partial = false;
  if (offset > size) {
    offset = 0;
    result.rotated = true;
  }
  if (tail_window && size > kMaxSourceBytes) {
    offset = size - kMaxSourceBytes;
    result.truncated = true;
    discard_partial = true;
  } else if (!tail_window && size - offset > kMaxSourceBytes) {
    offset = size - kMaxSourceBytes;
    result.truncated = true;
    discard_partial = true;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("failed to read run event source: " +
                             path.string());
  input.seekg(static_cast<std::streamoff>(offset));
  if (!input)
    throw std::runtime_error("failed to seek run event source: " +
                             path.string());
  if (discard_partial) {
    std::string partial;
    std::getline(input, partial);
  }
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty())
      result.lines.push_back(std::move(line));
  }
  return result;
}

std::string scalar(const YAML::Node &node, const char *field,
                   std::size_t max_size = 2048) {
  if (!node[field] || !node[field].IsScalar())
    throw std::runtime_error(std::string("event field '") + field +
                             "' is missing");
  auto value = node[field].as<std::string>();
  if (value.size() > max_size)
    value = value.substr(0, max_size) + " [truncated]";
  return value;
}

std::string optional_scalar(const YAML::Node &node, const char *field,
                            std::size_t max_size = 2048) {
  if (!node[field])
    return {};
  return scalar(node, field, max_size);
}

void append_warning(std::vector<RunEvent> &events, const std::string &timestamp,
                    const std::string &message, std::size_t limit) {
  if (events.size() < limit)
    events.push_back({"event_import_warning", timestamp, message});
}

std::string deployment_message(const YAML::Node &node) {
  std::ostringstream output;
  output << "action=" << scalar(node, "action", 256)
         << " outcome=" << scalar(node, "outcome", 256);
  const auto deployment = optional_scalar(node, "deployment_id", 256);
  const auto phase = optional_scalar(node, "phase", 128);
  const auto operation = optional_scalar(node, "operation", 128);
  const auto message = optional_scalar(node, "message", 1024);
  if (!deployment.empty())
    output << " deployment=" << deployment;
  if (!phase.empty())
    output << " phase=" << phase;
  if (!operation.empty())
    output << " operation=" << operation;
  if (!message.empty())
    output << " message=" << message;
  return output.str();
}
} // namespace

void initialize_run_event_sources(RunManifest &manifest) {
  manifest.deployment_audit_offset = 0;
  if (manifest.deployment_audit_path.empty())
    return;
  const std::filesystem::path path(manifest.deployment_audit_path);
  validate_source_file(path);
  if (std::filesystem::exists(path))
    manifest.deployment_audit_offset = std::filesystem::file_size(path);
}

std::vector<RunEvent> collect_run_source_events(const RunManifest &manifest,
                                                const std::string &stopped_at) {
  std::vector<RunEvent> events;
  const auto limit = manifest.max_imported_events;
  bool reached_limit = false;

  if (!manifest.health_history_path.empty()) {
    try {
      const auto source =
          read_bounded_lines(manifest.health_history_path, 0, true);
      if (source.truncated)
        append_warning(events, stopped_at,
                       "health history exceeded the 1 MiB import window",
                       limit);
      std::string previous_state;
      for (const auto &line : source.lines) {
        if (events.size() >= limit) {
          reached_limit = true;
          break;
        }
        try {
          const auto node = YAML::Load(line);
          const auto timestamp = scalar(node, "timestamp", 64);
          if (timestamp < manifest.started_at || timestamp > stopped_at)
            continue;
          const auto state = scalar(node, "state", 64);
          if (state == previous_state)
            continue;
          previous_state = state;
          const auto robot = optional_scalar(node, "robot_id", 256);
          events.push_back({"health_transition", timestamp,
                            "robot=" + robot + " state=" + state});
        } catch (const std::exception &error) {
          append_warning(events, stopped_at,
                         std::string("invalid health history entry: ") +
                             error.what(),
                         limit);
        }
      }
    } catch (const std::exception &error) {
      append_warning(
          events, stopped_at,
          std::string("health history import failed: ") + error.what(), limit);
    }
  }

  if (!manifest.deployment_audit_path.empty() && events.size() < limit) {
    try {
      const auto source =
          read_bounded_lines(manifest.deployment_audit_path,
                             manifest.deployment_audit_offset, false);
      if (source.rotated)
        append_warning(events, stopped_at,
                       "deployment audit rotated during capture", limit);
      if (source.truncated)
        append_warning(events, stopped_at,
                       "deployment audit exceeded the 1 MiB import window",
                       limit);
      for (const auto &line : source.lines) {
        if (events.size() >= limit) {
          reached_limit = true;
          break;
        }
        try {
          const auto node = YAML::Load(line);
          const auto timestamp = scalar(node, "timestamp", 64);
          if (timestamp < manifest.started_at || timestamp > stopped_at)
            continue;
          events.push_back(
              {"deployment_event", timestamp, deployment_message(node)});
        } catch (const std::exception &error) {
          append_warning(events, stopped_at,
                         std::string("invalid deployment audit entry: ") +
                             error.what(),
                         limit);
        }
      }
    } catch (const std::exception &error) {
      append_warning(events, stopped_at,
                     std::string("deployment audit import failed: ") +
                         error.what(),
                     limit);
    }
  }

  if (reached_limit) {
    const RunEvent warning{"event_import_warning", stopped_at,
                           "run event import reached its event limit"};
    if (events.size() < limit)
      events.push_back(warning);
    else
      events.back() = warning;
  }
  std::stable_sort(events.begin(), events.end(),
                   [](const auto &left, const auto &right) {
                     return left.timestamp < right.timestamp;
                   });
  return events;
}

} // namespace vektor
