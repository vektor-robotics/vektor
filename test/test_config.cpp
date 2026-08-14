#include "vektor/config.hpp"

#include <gtest/gtest.h>

#include <fstream>

TEST(Config, ParsesAllRequirementTypes) {
  const auto path = std::string("vektor_test_config.yaml");
  std::ofstream file(path);
  file << "required_nodes: [/planner]\n"
       << "required_topics:\n  - name: /cmd_vel\n    min_frequency_hz: 5\n"
       << "    sample_window_ms: 200\n    reliability: best_effort\n    "
          "qos_depth: 5\n"
       << "required_tf:\n  - target_frame: map\n    source_frame: base_link\n  "
          "  timeout_ms: 300\n"
       << "lifecycle:\n  - node: /controller\n    state: active\n"
       << "    service_timeout_ms: 400\n    request_timeout_ms: 500\n";
  file.close();
  const auto config = vektor::load_config(path);
  EXPECT_EQ(config.required_nodes.size(), 1u);
  EXPECT_EQ(config.required_topics.front().sample_window.count(), 200);
  EXPECT_EQ(config.required_topics.front().reliability,
            vektor::Reliability::BestEffort);
  EXPECT_EQ(config.required_topics.front().qos_depth, 5u);
  EXPECT_EQ(config.required_tf.front().source_frame, "base_link");
  EXPECT_EQ(config.required_tf.front().timeout.count(), 300);
  EXPECT_EQ(config.lifecycle.front().state, "active");
  EXPECT_EQ(config.lifecycle.front().service_timeout.count(), 400);
  EXPECT_EQ(config.lifecycle.front().request_timeout.count(), 500);
  std::remove(path.c_str());
}

TEST(Config, RejectsInvalidFrequencyRange) {
  const auto path = std::string("vektor_invalid_frequency.yaml");
  std::ofstream file(path);
  file << "required_topics:\n  - name: /cmd_vel\n    min_frequency_hz: 20\n"
       << "    max_frequency_hz: 10\n";
  file.close();
  EXPECT_THROW(vektor::load_config(path), std::runtime_error);
  std::remove(path.c_str());
}

TEST(Config, RejectsUnknownTopLevelField) {
  const auto path = std::string("vektor_unknown_field.yaml");
  std::ofstream file(path);
  file << "required_nodez: [/planner]\n";
  file.close();
  EXPECT_THROW(vektor::load_config(path), std::runtime_error);
  std::remove(path.c_str());
}
