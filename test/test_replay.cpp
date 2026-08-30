#include "vektor/capture.hpp"
#include "vektor/replay.hpp"
#include "vektor/run.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {
constexpr auto kArtifact =
    "registry.example/robot@sha256:"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

vektor::RunManifest completed_run(const std::filesystem::path &root,
                                  const std::string &run_id) {
  const auto bag = root / "bag";
  std::filesystem::create_directories(bag);
  std::ofstream(bag / "data.db3") << "replay-data";

  vektor::RunManifest definition;
  definition.run_id = run_id;
  definition.name = "Replay source";
  definition.artifact = kArtifact;
  definition.workload_id = "navigation";
  definition.policy = "policy.yaml";
  definition.policy_sha256 =
      "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  definition.topics = {"/odom"};
  definition.health_history_path = (root / "health.jsonl").string();
  definition.deployment_audit_path = (root / "audit.jsonl").string();
  definition.robot_id = "robot-1";
  definition.operator_id = "operator@example.com";
  const vektor::RunStore store(root / "runs");
  store.start(definition);
  return store.complete_capture(
      run_id, "passed", {}, vektor::fingerprint_run_artifact(bag, "rosbag2"));
}

vektor::ReplayDefinition replay_definition(const std::string &replay_id,
                                           const std::string &run_id) {
  vektor::ReplayDefinition definition;
  definition.replay_id = replay_id;
  definition.source_run_id = run_id;
  definition.ros_domain_id = 231;
  definition.timeout = std::chrono::seconds(5);
  definition.topic_remaps["/odom"] = "/vektor/replay/odom";
  return definition;
}
} // namespace

TEST(Replay, ExecutesRosbag2AdapterAndPersistsProvenance) {
  const auto root = std::filesystem::path("vektor_test_replay_rosbag");
  std::filesystem::remove_all(root);
  const auto source = completed_run(root, "source-run");
  const vektor::ReplayExecutor executor("/bin/true");
  const auto manifest = executor.execute(
      replay_definition("replay-001", source.run_id), source, root / "replays");

  EXPECT_EQ(manifest.status, vektor::ReplayStatus::Completed);
  EXPECT_EQ(manifest.exit_code, 0);
  EXPECT_EQ(manifest.adapter, vektor::ReplayAdapter::Rosbag2);
  EXPECT_EQ(manifest.source_bag.sha256, source.artifacts.front().sha256);
  ASSERT_GE(manifest.command.size(), 7U);
  EXPECT_EQ(manifest.command[0], "/bin/true");
  EXPECT_EQ(manifest.command[1], "bag");
  EXPECT_EQ(manifest.command[2], "play");
  EXPECT_NE(vektor::replay_manifest_to_json(manifest).find(
                "\"status\":\"completed\""),
            std::string::npos);
  EXPECT_TRUE(std::filesystem::exists(root / "replays" / "replay-001.yaml"));
  std::filesystem::remove_all(root);
}

TEST(Replay, ExpandsSimulatorArgumentsWithoutUsingAShell) {
  const auto root = std::filesystem::path("vektor_test_replay_simulator");
  std::filesystem::remove_all(root);
  const auto source = completed_run(root, "sim-source");
  auto definition = replay_definition("sim-replay", source.run_id);
  definition.adapter = vektor::ReplayAdapter::Simulator;
  definition.executable = "/bin/true";
  definition.arguments = {"--bag",    "${bag_path}",
                          "--run",    "${source_run_id}",
                          "--output", "${output_dir}"};
  const vektor::ReplayExecutor executor;
  const auto manifest = executor.execute(definition, source, root / "replays");

  EXPECT_EQ(manifest.status, vektor::ReplayStatus::Completed);
  ASSERT_EQ(manifest.command.size(), 7U);
  EXPECT_EQ(manifest.command[2], source.artifacts.front().uri);
  EXPECT_EQ(manifest.command[4], source.run_id);
  EXPECT_EQ(manifest.command[6],
            std::filesystem::absolute(root / "replays" / "sim-replay")
                .lexically_normal()
                .string());
  std::filesystem::remove_all(root);
}

TEST(Replay, RejectsTamperedSourceBagAndDuplicateReplay) {
  const auto root = std::filesystem::path("vektor_test_replay_integrity");
  std::filesystem::remove_all(root);
  const auto source = completed_run(root, "integrity-source");
  const auto definition = replay_definition("integrity-replay", source.run_id);
  const vektor::ReplayExecutor executor("/bin/true");
  executor.execute(definition, source, root / "replays");
  EXPECT_THROW(executor.execute(definition, source, root / "replays"),
               std::runtime_error);

  std::ofstream(root / "bag" / "data.db3", std::ios::app) << "tampered";
  auto another = definition;
  another.replay_id = "tampered-replay";
  EXPECT_THROW(executor.execute(another, source, root / "replays"),
               std::runtime_error);
  std::filesystem::remove_all(root);
}

TEST(Replay, BoundsExecutionTime) {
  const auto root = std::filesystem::path("vektor_test_replay_timeout");
  std::filesystem::remove_all(root);
  const auto source = completed_run(root, "timeout-source");
  auto definition = replay_definition("timeout-replay", source.run_id);
  definition.adapter = vektor::ReplayAdapter::Simulator;
  definition.executable = "/bin/sleep";
  definition.arguments = {"10"};
  definition.timeout = std::chrono::seconds(1);
  const vektor::ReplayExecutor executor;
  const auto manifest = executor.execute(definition, source, root / "replays");

  EXPECT_EQ(manifest.status, vektor::ReplayStatus::TimedOut);
  EXPECT_EQ(manifest.exit_code, -1);
  std::filesystem::remove_all(root);
}

TEST(Replay, LoadsVersionedDefinitionAndRejectsUnsafeDomain) {
  const auto path = std::filesystem::path("vektor_test_replay.yaml");
  {
    std::ofstream output(path);
    output << "schema_version: 1\n"
              "replay_id: replay-config\n"
              "source_run_id: source-run\n"
              "adapter: simulator\n"
              "ros_domain_id: 230\n"
              "timeout_seconds: 30\n"
              "simulator:\n"
              "  executable: /bin/true\n"
              "  arguments: ['${source_run_id}']\n";
  }
  const auto definition = vektor::load_replay_definition(path);
  EXPECT_EQ(definition.adapter, vektor::ReplayAdapter::Simulator);
  EXPECT_EQ(definition.ros_domain_id, 230U);
  EXPECT_EQ(definition.arguments.front(), "${source_run_id}");

  {
    std::ofstream output(path, std::ios::trunc);
    output << "schema_version: 1\nreplay_id: bad\nsource_run_id: source\n"
              "adapter: rosbag2\nros_domain_id: 0\n";
  }
  EXPECT_THROW(vektor::load_replay_definition(path), std::invalid_argument);
  std::filesystem::remove(path);
}
