#include "vektor/agent.hpp"
#include "vektor/fleet.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <sstream>

namespace {
vektor::StatusSnapshot snapshot_for(const std::string &robot_id,
                                    vektor::HealthState state) {
  auto snapshot =
      vektor::make_status_snapshot(robot_id, {}, std::chrono::milliseconds(0));
  snapshot.state = state;
  return snapshot;
}

struct TestAgent {
  vektor::AgentStatusState state;
  vektor::GrpcAgentService service{state};
  std::unique_ptr<grpc::Server> server;
  int port{0};

  TestAgent(const std::string &robot_id, vektor::HealthState health) {
    state.publish(snapshot_for(robot_id, health));
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port);
    builder.RegisterService(&service);
    server = builder.BuildAndStart();
  }

  ~TestAgent() {
    if (server)
      server->Shutdown();
  }
};
} // namespace

TEST(FleetConfig, ParsesInventoryAndLabels) {
  const auto path = std::string("vektor_test_fleet.yaml");
  std::ofstream file(path);
  file << "fleet_id: test-fleet\nrequest_timeout_ms: 750\n"
       << "max_snapshot_age_ms: 30000\n"
       << "transport:\n  insecure: true\nrobots:\n"
       << "  - id: robot-1\n    endpoint: 127.0.0.1:50051\n"
       << "    labels:\n      site: berlin\n      ring: canary\n";
  file.close();

  const auto config = vektor::load_fleet_config(path);
  EXPECT_EQ(config.fleet_id, "test-fleet");
  EXPECT_EQ(config.request_timeout.count(), 750);
  EXPECT_EQ(config.max_snapshot_age.count(), 30000);
  ASSERT_EQ(config.robots.size(), 1U);
  EXPECT_EQ(config.robots.front().labels.at("ring"), "canary");
  std::remove(path.c_str());
}

TEST(FleetConfig, RejectsInsecureNonLoopbackEndpoint) {
  const auto path = std::string("vektor_invalid_fleet.yaml");
  std::ofstream file(path);
  file << "fleet_id: test-fleet\ntransport:\n  insecure: true\nrobots:\n"
       << "  - id: robot-1\n    endpoint: 10.0.0.8:50051\n";
  file.close();
  EXPECT_THROW(vektor::load_fleet_config(path), std::runtime_error);
  std::remove(path.c_str());
}

TEST(FleetSelection, MatchesAllLabelsAndAppliesStableLimit) {
  vektor::FleetConfig config;
  config.robots = {
      {"robot-1", "127.0.0.1:1", {{"site", "berlin"}, {"ring", "canary"}}},
      {"robot-2", "127.0.0.1:2", {{"site", "berlin"}, {"ring", "stable"}}},
      {"robot-3", "127.0.0.1:3", {{"site", "munich"}, {"ring", "canary"}}},
  };
  const auto selected = vektor::select_fleet_robots(
      config, {vektor::parse_label_selector("site=berlin")}, 1);
  ASSERT_EQ(selected.size(), 1U);
  EXPECT_EQ(selected.front().id, "robot-1");
}

TEST(FleetSelection, RejectsEmptySelection) {
  vektor::FleetConfig config;
  config.robots = {{"robot-1", "127.0.0.1:1", {{"site", "berlin"}}}};
  EXPECT_THROW(vektor::select_fleet_robots(
                   config, {vektor::parse_label_selector("site=munich")}),
               std::invalid_argument);
}

TEST(FleetHealth, UnreachableTakesAggregatePrecedence) {
  std::vector<vektor::FleetRobotStatus> robots(3);
  robots[0].state = vektor::HealthState::Healthy;
  robots[1].state = vektor::HealthState::Unhealthy;
  robots[2].state = vektor::HealthState::Unreachable;
  EXPECT_EQ(vektor::aggregate_fleet_state(robots),
            vektor::HealthState::Unreachable);
}

TEST(FleetPolling, AggregatesConcurrentAgentResponses) {
  TestAgent first("robot-1", vektor::HealthState::Healthy);
  TestAgent second("robot-2", vektor::HealthState::Degraded);
  ASSERT_GT(first.port, 0);
  ASSERT_GT(second.port, 0);

  vektor::FleetConfig config;
  config.fleet_id = "test-fleet";
  config.request_timeout = std::chrono::milliseconds(1000);
  config.transport.insecure = true;
  config.robots = {
      {"robot-1",
       "127.0.0.1:" + std::to_string(first.port),
       {{"ring", "canary"}}},
      {"robot-2",
       "127.0.0.1:" + std::to_string(second.port),
       {{"ring", "stable"}}},
  };

  const auto report = vektor::poll_fleet(config);
  EXPECT_EQ(report.state, vektor::HealthState::Degraded);
  EXPECT_EQ(report.inventory_size, 2U);
  ASSERT_EQ(report.robots.size(), 2U);
  EXPECT_EQ(report.robots[0].snapshot->sequence(), 1U);

  const auto json = vektor::fleet_report_to_json(report);
  EXPECT_NE(json.find("\"fleet_id\":\"test-fleet\""), std::string::npos);
  EXPECT_NE(json.find("\"ring\":\"canary\""), std::string::npos);
}

TEST(FleetPolling, RejectsAgentIdentityMismatch) {
  TestAgent agent("different-robot", vektor::HealthState::Healthy);
  ASSERT_GT(agent.port, 0);
  vektor::FleetConfig config;
  config.fleet_id = "test-fleet";
  config.transport.insecure = true;
  config.robots = {
      {"expected-robot", "127.0.0.1:" + std::to_string(agent.port), {}}};

  const auto report = vektor::poll_fleet(config);
  ASSERT_EQ(report.robots.size(), 1U);
  EXPECT_EQ(report.state, vektor::HealthState::Unhealthy);
  EXPECT_NE(report.robots.front().error.find("identity mismatch"),
            std::string::npos);
}

TEST(FleetPolling, RejectsStaleAgentSnapshot) {
  TestAgent agent("robot-1", vektor::HealthState::Healthy);
  auto stale = snapshot_for("robot-1", vektor::HealthState::Healthy);
  stale.timestamp = "2000-01-01T00:00:00Z";
  agent.state.publish(std::move(stale));

  vektor::FleetConfig config;
  config.fleet_id = "test-fleet";
  config.transport.insecure = true;
  config.robots = {{"robot-1", "127.0.0.1:" + std::to_string(agent.port), {}}};
  const auto report = vektor::poll_fleet(config);
  EXPECT_EQ(report.state, vektor::HealthState::Unreachable);
  EXPECT_EQ(report.robots.front().error, "agent snapshot is stale");
}
