#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace vektor {

enum class Reliability { SystemDefault, Reliable, BestEffort };

struct NodeRequirement {
  std::string name;
};

struct TopicRequirement {
  std::string name;
  double min_frequency_hz{0.0};
  double max_frequency_hz{0.0};
  std::chrono::milliseconds sample_window{1000};
  Reliability reliability{Reliability::SystemDefault};
  std::size_t qos_depth{10};
};

struct TfRequirement {
  std::string target_frame;
  std::string source_frame;
  std::chrono::milliseconds timeout{1000};
};

struct LifecycleRequirement {
  std::string node;
  std::string state{"active"};
  std::chrono::milliseconds service_timeout{1000};
  std::chrono::milliseconds request_timeout{1000};
};

struct CheckConfig {
  std::string robot_id;
  std::chrono::milliseconds discovery_timeout{500};
  std::vector<NodeRequirement> required_nodes;
  std::vector<TopicRequirement> required_topics;
  std::vector<TfRequirement> required_tf;
  std::vector<LifecycleRequirement> lifecycle;
};

CheckConfig load_config(const std::string &path);

} // namespace vektor
