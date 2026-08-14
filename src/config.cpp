#include "vektor/config.hpp"

#include <set>
#include <sstream>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

namespace vektor {

namespace {
[[noreturn]] void invalid(const std::string &field,
                          const std::string &message) {
  throw std::runtime_error("invalid config at '" + field + "': " + message);
}

template <typename T>
T optional(const YAML::Node &node, const char *key, T fallback) {
  return node[key] ? node[key].as<T>() : fallback;
}

void require_sequence(const YAML::Node &node, const std::string &field) {
  if (!node.IsSequence())
    invalid(field, "expected a sequence");
}

std::string require_string(const YAML::Node &node, const std::string &field) {
  if (!node || !node.IsScalar())
    invalid(field, "expected a non-empty string");
  const auto value = node.as<std::string>();
  if (value.empty())
    invalid(field, "must not be empty");
  return value;
}

std::chrono::milliseconds positive_milliseconds(const YAML::Node &node,
                                                const char *key, int fallback,
                                                const std::string &field) {
  const auto value = optional(node, key, fallback);
  if (value <= 0)
    invalid(field, "must be greater than zero");
  return std::chrono::milliseconds(value);
}

Reliability parse_reliability(const YAML::Node &node,
                              const std::string &field) {
  if (!node)
    return Reliability::SystemDefault;
  const auto value = node.as<std::string>();
  if (value == "system_default")
    return Reliability::SystemDefault;
  if (value == "reliable")
    return Reliability::Reliable;
  if (value == "best_effort")
    return Reliability::BestEffort;
  invalid(field, "expected system_default, reliable, or best_effort");
}
} // namespace

CheckConfig load_config(const std::string &path) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path);
  } catch (const YAML::Exception &error) {
    throw std::runtime_error("failed to load config '" + path +
                             "': " + error.what());
  }
  if (!root.IsMap())
    invalid("root", "expected a mapping");

  static const std::set<std::string> allowed_keys{
      "required_nodes", "required_topics", "required_tf", "lifecycle"};
  for (const auto &entry : root) {
    const auto key = entry.first.as<std::string>();
    if (!allowed_keys.contains(key))
      invalid(key, "unknown top-level field");
  }
  CheckConfig config;

  if (const auto nodes = root["required_nodes"]; nodes) {
    require_sequence(nodes, "required_nodes");
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      config.required_nodes.push_back({require_string(
          nodes[index], "required_nodes[" + std::to_string(index) + "]")});
    }
  }
  if (const auto topics = root["required_topics"]; topics) {
    require_sequence(topics, "required_topics");
    for (std::size_t index = 0; index < topics.size(); ++index) {
      const auto item = topics[index];
      const auto field = "required_topics[" + std::to_string(index) + "]";
      TopicRequirement requirement;
      if (item.IsScalar()) {
        requirement.name = require_string(item, field);
      } else if (item.IsMap()) {
        requirement.name = require_string(item["name"], field + ".name");
        requirement.min_frequency_hz = optional(item, "min_frequency_hz", 0.0);
        requirement.max_frequency_hz = optional(item, "max_frequency_hz", 0.0);
        requirement.sample_window = positive_milliseconds(
            item, "sample_window_ms", 1000, field + ".sample_window_ms");
        requirement.reliability =
            parse_reliability(item["reliability"], field + ".reliability");
        const auto depth = optional(item, "qos_depth", 10);
        if (depth <= 0)
          invalid(field + ".qos_depth", "must be greater than zero");
        requirement.qos_depth = static_cast<std::size_t>(depth);
        if (requirement.min_frequency_hz < 0.0 ||
            requirement.max_frequency_hz < 0.0) {
          invalid(field, "frequency limits must not be negative");
        }
        if (requirement.max_frequency_hz > 0.0 &&
            requirement.min_frequency_hz > requirement.max_frequency_hz) {
          invalid(field, "min_frequency_hz must not exceed max_frequency_hz");
        }
      } else {
        invalid(field, "expected a topic name or mapping");
      }
      config.required_topics.push_back(requirement);
    }
  }
  if (const auto tf = root["required_tf"]; tf) {
    require_sequence(tf, "required_tf");
    for (std::size_t index = 0; index < tf.size(); ++index) {
      const auto item = tf[index];
      const auto field = "required_tf[" + std::to_string(index) + "]";
      if (!item.IsMap())
        invalid(field, "expected a mapping");
      config.required_tf.push_back(
          {require_string(item["target_frame"], field + ".target_frame"),
           require_string(item["source_frame"], field + ".source_frame"),
           positive_milliseconds(item, "timeout_ms", 1000,
                                 field + ".timeout_ms")});
    }
  }
  if (const auto lifecycle = root["lifecycle"]; lifecycle) {
    require_sequence(lifecycle, "lifecycle");
    for (std::size_t index = 0; index < lifecycle.size(); ++index) {
      const auto item = lifecycle[index];
      const auto field = "lifecycle[" + std::to_string(index) + "]";
      if (!item.IsMap())
        invalid(field, "expected a mapping");
      config.lifecycle.push_back(
          {require_string(item["node"], field + ".node"),
           require_string(item["state"] ? item["state"] : YAML::Node("active"),
                          field + ".state"),
           positive_milliseconds(item, "service_timeout_ms", 1000,
                                 field + ".service_timeout_ms"),
           positive_milliseconds(item, "request_timeout_ms", 1000,
                                 field + ".request_timeout_ms")});
    }
  }
  return config;
}

} // namespace vektor
