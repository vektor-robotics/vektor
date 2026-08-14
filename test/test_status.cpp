#include "vektor/status.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST(Status, DerivesFleetHealthStates) {
  EXPECT_EQ(vektor::derive_health_state({}), vektor::HealthState::Unreachable);
  EXPECT_EQ(vektor::derive_health_state(
                {{vektor::CheckStatus::Pass, "node", "/planner", "ok"}}),
            vektor::HealthState::Healthy);
  EXPECT_EQ(vektor::derive_health_state(
                {{vektor::CheckStatus::Warn, "node", "/planner", "slow"}}),
            vektor::HealthState::Degraded);
  EXPECT_EQ(vektor::derive_health_state(
                {{vektor::CheckStatus::Fail, "node", "/planner", "missing"}}),
            vektor::HealthState::Unhealthy);
}

TEST(Status, SerializesStableJsonSchema) {
  vektor::StatusSnapshot snapshot;
  snapshot.timestamp = "2026-08-14T12:00:00Z";
  snapshot.robot_id = "robot-01";
  snapshot.hostname = "edge-host";
  snapshot.ros_domain_id = 7;
  snapshot.state = vektor::HealthState::Healthy;
  snapshot.duration = std::chrono::milliseconds(125);
  snapshot.checks.push_back({vektor::CheckStatus::Pass, "node", "/planner",
                             "node is present", std::chrono::milliseconds(2)});

  const auto json = vektor::status_to_json(snapshot);
  EXPECT_NE(json.find("\"schema_version\":1"), std::string::npos);
  EXPECT_NE(json.find("\"robot_id\":\"robot-01\""), std::string::npos);
  EXPECT_NE(json.find("\"state\":\"healthy\""), std::string::npos);
  EXPECT_NE(json.find("\"duration_ms\":125"), std::string::npos);
}

TEST(Status, KeepsOnlyMostRecentHistoryEntries) {
  const auto directory =
      std::filesystem::current_path() / "vektor_status_store_test";
  const auto path = directory / "status.jsonl";
  std::filesystem::remove_all(directory);
  vektor::SnapshotStore store(path, 2);

  vektor::StatusSnapshot snapshot;
  snapshot.timestamp = "first";
  snapshot.robot_id = "robot-01";
  snapshot.hostname = "edge-host";
  snapshot.state = vektor::HealthState::Healthy;
  store.append(snapshot);
  snapshot.timestamp = "second";
  store.append(snapshot);
  snapshot.timestamp = "third";
  store.append(snapshot);

  std::ifstream input(path);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line))
    lines.push_back(line);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_NE(lines[0].find("second"), std::string::npos);
  EXPECT_NE(lines[1].find("third"), std::string::npos);
  std::filesystem::remove_all(directory);
}
