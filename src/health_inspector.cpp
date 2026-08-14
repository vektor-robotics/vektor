#include "vektor/health_inspector.hpp"

#include <lifecycle_msgs/srv/get_state.hpp>
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <thread>

namespace vektor {

namespace {
std::string fully_qualified_node_name(const std::string &name,
                                      const std::string &node_namespace) {
  const auto prefix =
      node_namespace.empty() || node_namespace == "/" ? "" : node_namespace;
  return prefix + "/" + name;
}

std::string normalize_node_name(const std::string &name) {
  return name.front() == '/' ? name : "/" + name;
}

std::chrono::milliseconds
elapsed_since(std::chrono::steady_clock::time_point started_at) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started_at);
}
} // namespace

HealthInspector::HealthInspector(const rclcpp::Node::SharedPtr &node)
    : node_(node) {}

CheckResult HealthInspector::check_node(const NodeRequirement &requirement) {
  const auto started_at = std::chrono::steady_clock::now();
  const auto graph =
      node_->get_node_graph_interface()->get_node_names_and_namespaces();
  const auto expected = normalize_node_name(requirement.name);
  const bool present =
      std::any_of(graph.begin(), graph.end(), [&](const auto &entry) {
        return fully_qualified_node_name(entry.first, entry.second) == expected;
      });
  return {present ? CheckStatus::Pass : CheckStatus::Fail, "node",
          requirement.name,
          present ? "node is present" : "required node is not present",
          elapsed_since(started_at)};
}

std::vector<CheckResult> HealthInspector::check_topics(
    const std::vector<TopicRequirement> &requirements) {
  const auto topics = node_->get_topic_names_and_types();
  std::vector<CheckResult> results;
  results.reserve(requirements.size());
  for (const auto &requirement : requirements) {
    results.push_back(
        {CheckStatus::Fail, "topic", requirement.name, "not checked"});
  }

  struct Monitor {
    std::size_t result_index;
    const TopicRequirement *requirement;
    std::size_t samples{0};
    std::chrono::steady_clock::time_point deadline{
        std::chrono::steady_clock::time_point::max()};
    std::shared_ptr<rclcpp::GenericSubscription> subscription;
  };
  std::vector<std::shared_ptr<Monitor>> monitors;

  for (std::size_t index = 0; index < requirements.size(); ++index) {
    const auto &requirement = requirements[index];
    const auto topic = topics.find(requirement.name);
    if (topic == topics.end() || topic->second.empty()) {
      results[index].message = "topic does not exist";
      continue;
    }
    if (requirement.min_frequency_hz <= 0.0 &&
        requirement.max_frequency_hz <= 0.0) {
      results[index] = {CheckStatus::Pass, "topic", requirement.name,
                        "topic exists", std::chrono::milliseconds(0)};
      continue;
    }

    auto monitor = std::make_shared<Monitor>();
    monitor->result_index = index;
    monitor->requirement = &requirement;
    auto qos = rclcpp::QoS(rclcpp::KeepLast(requirement.qos_depth));
    if (requirement.reliability == Reliability::Reliable)
      qos.reliable();
    if (requirement.reliability == Reliability::BestEffort)
      qos.best_effort();
    monitor->subscription = node_->create_generic_subscription(
        requirement.name, topic->second.front(), qos,
        [monitor](std::shared_ptr<rclcpp::SerializedMessage>) {
          if (std::chrono::steady_clock::now() <= monitor->deadline)
            ++monitor->samples;
        });
    monitors.push_back(std::move(monitor));
  }

  if (monitors.empty())
    return results;

  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node_);

  const auto discovery_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  while (rclcpp::ok() &&
         std::chrono::steady_clock::now() < discovery_deadline) {
    const bool all_matched =
        std::all_of(monitors.begin(), monitors.end(), [](const auto &monitor) {
          return monitor->subscription->get_publisher_count() > 0;
        });
    if (all_matched)
      break;
    executor.spin_some(std::chrono::milliseconds(20));
  }

  const auto started_at = std::chrono::steady_clock::now();
  auto final_deadline = started_at;
  for (auto &monitor : monitors) {
    monitor->deadline = started_at + monitor->requirement->sample_window;
    final_deadline = std::max(final_deadline, monitor->deadline);
  }
  while (rclcpp::ok() && std::chrono::steady_clock::now() < final_deadline)
    executor.spin_some(std::chrono::milliseconds(20));

  executor.remove_node(node_);

  for (const auto &monitor : monitors) {
    const auto &requirement = *monitor->requirement;
    const double seconds =
        std::max(0.001, requirement.sample_window.count() / 1000.0);
    const double frequency = static_cast<double>(monitor->samples) / seconds;
    const bool min_ok = requirement.min_frequency_hz <= 0.0 ||
                        frequency >= requirement.min_frequency_hz;
    const bool max_ok = requirement.max_frequency_hz <= 0.0 ||
                        frequency <= requirement.max_frequency_hz;
    std::ostringstream message;
    message << "measured " << frequency << " Hz (" << monitor->samples
            << " samples)";
    results[monitor->result_index] = {
        min_ok && max_ok ? CheckStatus::Pass : CheckStatus::Fail, "topic",
        requirement.name,
        min_ok && max_ok
            ? message.str()
            : message.str() + "; outside configured frequency limits",
        requirement.sample_window};
    monitor->subscription.reset();
  }
  return results;
}

CheckResult HealthInspector::check_tf(const TfRequirement &requirement) {
  const auto started_at = std::chrono::steady_clock::now();
  tf2_ros::Buffer buffer(node_->get_clock());
  tf2_ros::TransformListener listener(buffer, node_, false);
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node_);
  const auto deadline = std::chrono::steady_clock::now() + requirement.timeout;
  std::string last_error = "transform was not available before timeout";
  while (rclcpp::ok() && std::chrono::steady_clock::now() < deadline) {
    executor.spin_some(std::chrono::milliseconds(20));
    try {
      buffer.lookupTransform(requirement.target_frame, requirement.source_frame,
                             tf2::TimePointZero);
      executor.remove_node(node_);
      return {CheckStatus::Pass, "tf",
              requirement.target_frame + " <- " + requirement.source_frame,
              "transform is available", elapsed_since(started_at)};
    } catch (const tf2::TransformException &error) {
      last_error = error.what();
    }
  }
  executor.remove_node(node_);
  return {CheckStatus::Fail, "tf",
          requirement.target_frame + " <- " + requirement.source_frame,
          last_error, elapsed_since(started_at)};
}

CheckResult
HealthInspector::check_lifecycle(const LifecycleRequirement &requirement) {
  const auto started_at = std::chrono::steady_clock::now();
  const auto service = normalize_node_name(requirement.node) + "/get_state";
  auto client = node_->create_client<lifecycle_msgs::srv::GetState>(service);
  if (!client->wait_for_service(requirement.service_timeout)) {
    return {CheckStatus::Fail, "lifecycle", requirement.node,
            "get_state service unavailable", elapsed_since(started_at)};
  }
  auto future = client->async_send_request(
      std::make_shared<lifecycle_msgs::srv::GetState::Request>());
  if (rclcpp::spin_until_future_complete(node_, future,
                                         requirement.request_timeout) !=
      rclcpp::FutureReturnCode::SUCCESS) {
    return {CheckStatus::Fail, "lifecycle", requirement.node,
            "get_state request timed out", elapsed_since(started_at)};
  }
  const auto response = future.get();
  const bool ok = response->current_state.label == requirement.state;
  return {ok ? CheckStatus::Pass : CheckStatus::Fail, "lifecycle",
          requirement.node,
          "state is " + response->current_state.label + ", expected " +
              requirement.state,
          elapsed_since(started_at)};
}

std::vector<CheckResult> HealthInspector::inspect(const CheckConfig &config) {
  std::this_thread::sleep_for(config.discovery_timeout);
  std::vector<CheckResult> results;
  for (const auto &item : config.required_nodes)
    results.push_back(check_node(item));
  const auto topic_results = check_topics(config.required_topics);
  results.insert(results.end(), topic_results.begin(), topic_results.end());
  for (const auto &item : config.required_tf)
    results.push_back(check_tf(item));
  for (const auto &item : config.lifecycle)
    results.push_back(check_lifecycle(item));
  return results;
}

} // namespace vektor
