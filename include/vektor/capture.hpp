#pragma once

#include "vektor/run.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vektor {

class RosbagRecorder {
public:
  explicit RosbagRecorder(std::filesystem::path executable = "ros2");

  std::int64_t start(const std::filesystem::path &bag_path,
                     const std::vector<std::string> &topics,
                     const std::string &storage_id,
                     const std::filesystem::path &log_path) const;
  void stop(std::int64_t pid, const std::filesystem::path &expected_bag_path,
            std::chrono::milliseconds timeout = std::chrono::seconds(10)) const;

private:
  std::filesystem::path executable_;
};

RunArtifact fingerprint_run_artifact(const std::filesystem::path &path,
                                     const std::string &kind);

} // namespace vektor
