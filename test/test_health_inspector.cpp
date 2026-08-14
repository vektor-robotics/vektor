#include "vektor/health_inspector.hpp"

#include <geometry_msgs/msg/transform_stamped.hpp>
#include <gtest/gtest.h>
#include <lifecycle_msgs/srv/get_state.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <tf2_ros/static_transform_broadcaster.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

class HealthInspectorTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { rclcpp::init(0, nullptr); }

  static void TearDownTestSuite() { rclcpp::shutdown(); }

  static vektor::CheckConfig fast_config() {
    vektor::CheckConfig config;
    config.discovery_timeout = 1ms;
    return config;
  }
};

TEST_F(HealthInspectorTest, FindsFullyQualifiedNodeName) {
  auto inspector_node = std::make_shared<rclcpp::Node>("inspector_node");
  auto subject = std::make_shared<rclcpp::Node>("controller", "/robot_1");
  auto config = fast_config();
  config.required_nodes.push_back({"/robot_1/controller"});

  std::vector<vektor::CheckResult> results;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  do {
    results = vektor::HealthInspector(inspector_node).inspect(config);
    if (results.front().status == vektor::CheckStatus::Pass)
      break;
    std::this_thread::sleep_for(50ms);
  } while (std::chrono::steady_clock::now() < deadline);

  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results.front().status, vektor::CheckStatus::Pass);
}

TEST_F(HealthInspectorTest, MeasuresTopicFrequencyWithConfiguredQos) {
  auto inspector_node = std::make_shared<rclcpp::Node>("frequency_inspector");
  auto publisher_node = std::make_shared<rclcpp::Node>("frequency_source");
  auto publisher = publisher_node->create_publisher<std_msgs::msg::String>(
      "/vektor_test/heartbeat", rclcpp::QoS(10).best_effort());

  const auto discovery_deadline = std::chrono::steady_clock::now() + 2s;
  while (inspector_node->get_topic_names_and_types().count(
             "/vektor_test/heartbeat") == 0 &&
         std::chrono::steady_clock::now() < discovery_deadline) {
    std::this_thread::sleep_for(50ms);
  }

  std::atomic<bool> publishing{true};
  std::thread publish_thread([&]() {
    std_msgs::msg::String message;
    message.data = "healthy";
    while (publishing.load()) {
      publisher->publish(message);
      std::this_thread::sleep_for(50ms);
    }
  });

  auto config = fast_config();
  vektor::TopicRequirement topic;
  topic.name = "/vektor_test/heartbeat";
  topic.min_frequency_hz = 10.0;
  topic.sample_window = 750ms;
  topic.reliability = vektor::Reliability::BestEffort;
  config.required_topics.push_back(topic);
  const auto results = vektor::HealthInspector(inspector_node).inspect(config);
  publishing.store(false);
  publish_thread.join();

  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results.front().status, vektor::CheckStatus::Pass)
      << results.front().message;
}

TEST_F(HealthInspectorTest, SamplesMultipleTopicsConcurrently) {
  auto inspector_node = std::make_shared<rclcpp::Node>("parallel_inspector");
  auto publisher_node = std::make_shared<rclcpp::Node>("parallel_source");
  auto first = publisher_node->create_publisher<std_msgs::msg::String>(
      "/vektor_test/first", rclcpp::QoS(10).best_effort());
  auto second = publisher_node->create_publisher<std_msgs::msg::String>(
      "/vektor_test/second", rclcpp::QoS(10).best_effort());

  const auto discovery_deadline = std::chrono::steady_clock::now() + 2s;
  while ((inspector_node->get_topic_names_and_types().count(
              "/vektor_test/first") == 0 ||
          inspector_node->get_topic_names_and_types().count(
              "/vektor_test/second") == 0) &&
         std::chrono::steady_clock::now() < discovery_deadline) {
    std::this_thread::sleep_for(50ms);
  }

  std::atomic<bool> publishing{true};
  std::thread publish_thread([&]() {
    std_msgs::msg::String message;
    message.data = "healthy";
    while (publishing.load()) {
      first->publish(message);
      second->publish(message);
      std::this_thread::sleep_for(50ms);
    }
  });

  auto config = fast_config();
  for (const auto *name : {"/vektor_test/first", "/vektor_test/second"}) {
    vektor::TopicRequirement topic;
    topic.name = name;
    topic.min_frequency_hz = 10.0;
    topic.sample_window = 1s;
    topic.reliability = vektor::Reliability::BestEffort;
    config.required_topics.push_back(topic);
  }

  const auto started_at = std::chrono::steady_clock::now();
  const auto results = vektor::HealthInspector(inspector_node).inspect(config);
  const auto elapsed = std::chrono::steady_clock::now() - started_at;
  publishing.store(false);
  publish_thread.join();

  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].status, vektor::CheckStatus::Pass) << results[0].message;
  EXPECT_EQ(results[1].status, vektor::CheckStatus::Pass) << results[1].message;
  EXPECT_LT(elapsed, 1700ms);
}

TEST_F(HealthInspectorTest, FindsStaticTransformWithinTimeout) {
  auto inspector_node = std::make_shared<rclcpp::Node>("tf_inspector");
  auto broadcaster_node = std::make_shared<rclcpp::Node>("tf_source");
  tf2_ros::StaticTransformBroadcaster broadcaster(broadcaster_node);
  geometry_msgs::msg::TransformStamped transform;
  transform.header.stamp = broadcaster_node->now();
  transform.header.frame_id = "vektor_map";
  transform.child_frame_id = "vektor_base_link";
  transform.transform.rotation.w = 1.0;
  broadcaster.sendTransform(transform);

  auto config = fast_config();
  config.required_tf.push_back(
      {"vektor_map", "vektor_base_link", std::chrono::milliseconds(1500)});
  const auto results = vektor::HealthInspector(inspector_node).inspect(config);

  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results.front().status, vektor::CheckStatus::Pass)
      << results.front().message;
}

TEST_F(HealthInspectorTest, ReadsLifecycleStateService) {
  auto inspector_node = std::make_shared<rclcpp::Node>("lifecycle_inspector");
  auto service_node = std::make_shared<rclcpp::Node>("lifecycle_source");
  auto service = service_node->create_service<lifecycle_msgs::srv::GetState>(
      "/vektor_test_controller/get_state",
      [](const std::shared_ptr<lifecycle_msgs::srv::GetState::Request>,
         std::shared_ptr<lifecycle_msgs::srv::GetState::Response> response) {
        response->current_state.id = 3;
        response->current_state.label = "active";
      });
  rclcpp::executors::SingleThreadedExecutor service_executor;
  service_executor.add_node(service_node);
  std::thread service_thread([&]() { service_executor.spin(); });

  auto config = fast_config();
  config.lifecycle.push_back(
      {"/vektor_test_controller", "active", 1500ms, 1500ms});
  const auto results = vektor::HealthInspector(inspector_node).inspect(config);

  service_executor.cancel();
  service_thread.join();
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results.front().status, vektor::CheckStatus::Pass)
      << results.front().message;
}
