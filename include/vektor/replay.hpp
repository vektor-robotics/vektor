#pragma once

#include "vektor/run.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace vektor {

enum class ReplayAdapter { Rosbag2, Simulator };
enum class ReplayStatus { Completed, Failed, TimedOut };

struct ReplayDefinition {
  unsigned int schema_version{1};
  std::string replay_id;
  std::string source_run_id;
  ReplayAdapter adapter{ReplayAdapter::Rosbag2};
  unsigned int ros_domain_id{232};
  std::chrono::milliseconds timeout{std::chrono::minutes(10)};
  std::map<std::string, std::string> topic_remaps;
  std::filesystem::path qos_overrides;
  std::filesystem::path executable;
  std::vector<std::string> arguments;
};

struct ReplayManifest {
  unsigned int schema_version{1};
  std::string replay_id;
  std::string source_run_id;
  std::string source_artifact;
  RunArtifact source_bag;
  ReplayAdapter adapter{ReplayAdapter::Rosbag2};
  unsigned int adapter_version{1};
  unsigned int ros_domain_id{232};
  bool localhost_only{true};
  std::chrono::milliseconds timeout{std::chrono::minutes(10)};
  std::map<std::string, std::string> topic_remaps;
  std::vector<std::string> command;
  std::string started_at;
  std::string stopped_at;
  ReplayStatus status{ReplayStatus::Failed};
  int exit_code{-1};
  std::string log_path;
};

const char *replay_adapter_name(ReplayAdapter adapter);
const char *replay_status_name(ReplayStatus status);
ReplayDefinition load_replay_definition(const std::filesystem::path &path);
std::string replay_manifest_to_json(const ReplayManifest &manifest);
void print_replay_manifest(const ReplayManifest &manifest,
                           std::ostream &output);

class ReplayExecutor {
public:
  explicit ReplayExecutor(std::filesystem::path ros2_executable = "ros2");
  ReplayManifest execute(const ReplayDefinition &definition,
                         const RunManifest &source_run,
                         const std::filesystem::path &replay_directory) const;

private:
  std::filesystem::path ros2_executable_;
};

} // namespace vektor
