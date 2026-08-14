#pragma once

#include "vektor/config.hpp"

#include <rclcpp/rclcpp.hpp>
#include <string>
#include <vector>

namespace vektor {

enum class CheckStatus { Pass, Fail, Warn };

struct CheckResult {
  CheckStatus status;
  std::string category;
  std::string target;
  std::string message;
};

class HealthInspector {
public:
  explicit HealthInspector(const rclcpp::Node::SharedPtr &node);
  std::vector<CheckResult> inspect(const CheckConfig &config);

private:
  CheckResult check_node(const NodeRequirement &requirement);
  std::vector<CheckResult>
  check_topics(const std::vector<TopicRequirement> &requirements);
  CheckResult check_tf(const TfRequirement &requirement);
  CheckResult check_lifecycle(const LifecycleRequirement &requirement);

  rclcpp::Node::SharedPtr node_;
};

} // namespace vektor
