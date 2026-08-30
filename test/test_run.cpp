#include "vektor/capture.hpp"
#include "vektor/comparison.hpp"
#include "vektor/run.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {
constexpr auto kArtifact =
    "registry.example/robot@sha256:"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

vektor::RunManifest definition(const std::string &run_id) {
  vektor::RunManifest manifest;
  manifest.run_id = run_id;
  manifest.name = "Navigation baseline";
  manifest.artifact = kArtifact;
  manifest.workload_id = "navigation";
  manifest.policy = "policy.yaml";
  manifest.policy_sha256 =
      "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  manifest.parameters["speed"] = "0.5";
  manifest.environment["site"] = "lab";
  manifest.topics = {"/tf", "/odom"};
  manifest.health_history_path = "vektor_test_health.jsonl";
  manifest.deployment_audit_path = "vektor_test_deployment_audit.jsonl";
  manifest.robot_id = "robot-1";
  manifest.operator_id = "operator@example.com";
  return manifest;
}
} // namespace

TEST(RunManifest, StartsPersistsAndStopsRun) {
  const auto directory = std::filesystem::path("vektor_test_runs");
  std::filesystem::remove_all(directory);
  const vektor::RunStore store(directory);

  const auto started = store.start(definition("baseline-001"));
  EXPECT_EQ(started.schema_version, 1U);
  EXPECT_EQ(started.status, vektor::RunStatus::Active);
  EXPECT_FALSE(started.started_at.empty());
  EXPECT_TRUE(std::filesystem::exists(store.path_for("baseline-001")));

  const auto loaded = store.get("baseline-001");
  EXPECT_EQ(loaded.artifact, kArtifact);
  EXPECT_EQ(loaded.parameters.at("speed"), "0.5");
  EXPECT_EQ(loaded.environment.at("site"), "lab");

  const auto stopped = store.stop("baseline-001", "passed", {"no drift"},
                                  {{"goal_error_m", 0.125}});
  EXPECT_EQ(stopped.status, vektor::RunStatus::Completed);
  EXPECT_EQ(stopped.outcome, "passed");
  EXPECT_FALSE(stopped.stopped_at.empty());
  ASSERT_EQ(stopped.annotations.size(), 1U);
  EXPECT_EQ(stopped.annotations.front(), "no drift");
  EXPECT_DOUBLE_EQ(stopped.metrics.at("goal_error_m"), 0.125);
  EXPECT_NE(vektor::run_manifest_to_json(stopped).find(
                "\"metrics\":{\"goal_error_m\":0.125}"),
            std::string::npos);
  EXPECT_THROW(store.stop("baseline-001", "passed"), std::runtime_error);
  std::filesystem::remove_all(directory);
}

TEST(RunManifest, RejectsDuplicateAndUnsafeIds) {
  const auto directory = std::filesystem::path("vektor_test_run_ids");
  std::filesystem::remove_all(directory);
  const vektor::RunStore store(directory);
  store.start(definition("run-1"));
  EXPECT_THROW(store.start(definition("run-1")), std::runtime_error);
  EXPECT_THROW(store.start(definition("../escape")), std::invalid_argument);
  EXPECT_THROW(store.get("../escape"), std::invalid_argument);
  std::filesystem::remove_all(directory);
}

TEST(RunManifest, LoadsVersionedDefinitionAndRendersStableJson) {
  const auto path = std::filesystem::path("vektor_test_run_definition.yaml");
  const auto policy_path = std::filesystem::path("policy.yaml");
  {
    std::ofstream policy(policy_path);
    policy << "schema_version: 1\n";
  }
  {
    std::ofstream output(path);
    output << "schema_version: 1\n"
              "run_id: run-2\n"
              "name: Regression run\n"
              "artifact: "
           << kArtifact
           << "\n"
              "workload_id: navigation\n"
              "policy: policy.yaml\n"
              "parameters: {speed: '0.4'}\n"
              "environment: {site: lab}\n"
              "topics: [/tf, /odom]\n"
              "storage_id: sqlite3\n"
              "event_sources:\n"
              "  health_history: health.jsonl\n"
              "  deployment_audit: audit.jsonl\n"
              "  max_events: 32\n"
              "robot_id: robot-1\n"
              "operator: operator@example.com\n";
  }
  const auto manifest = vektor::load_run_definition(path);
  const auto json = vektor::run_manifest_to_json(manifest);
  EXPECT_NE(json.find("\"schema_version\":1"), std::string::npos);
  EXPECT_NE(json.find("\"run_id\":\"run-2\""), std::string::npos);
  EXPECT_NE(json.find("\"parameters\":{\"speed\":\"0.4\"}"), std::string::npos);
  EXPECT_NE(json.find("\"policy_sha256\":\"sha256:"), std::string::npos);
  std::filesystem::remove(path);
  std::filesystem::remove(policy_path);
}

TEST(RunManifest, RejectsUnpinnedArtifactAndUnknownSchema) {
  const auto path = std::filesystem::path("vektor_test_bad_run.yaml");
  {
    std::ofstream output(path);
    output << "schema_version: 2\n";
  }
  EXPECT_THROW(vektor::load_run_definition(path), std::invalid_argument);
  {
    std::ofstream output(path, std::ios::trunc);
    output << "schema_version: 1\n"
              "run_id: run-3\nname: Bad artifact\nartifact: latest\n"
              "workload_id: navigation\npolicy: policy.yaml\n"
              "topics: [/tf]\n"
              "event_sources: {health_history: health.jsonl, "
              "deployment_audit: audit.jsonl}\n"
              "robot_id: robot-1\noperator: operator@example.com\n";
  }
  EXPECT_THROW(vektor::load_run_definition(path), std::invalid_argument);
  std::filesystem::remove(path);
}

TEST(RunManifest, RejectsUnboundedOrUnsafeTopicSelection) {
  const auto directory = std::filesystem::path("vektor_test_run_topics");
  std::filesystem::remove_all(directory);
  const vektor::RunStore store(directory);
  auto manifest = definition("run-topics");
  manifest.topics.clear();
  EXPECT_THROW(store.start(manifest), std::invalid_argument);
  manifest.topics = {"relative/topic"};
  EXPECT_THROW(store.start(manifest), std::invalid_argument);
  manifest.topics.assign(65, "/topic");
  EXPECT_THROW(store.start(manifest), std::invalid_argument);
}

TEST(RunArtifact, FingerprintsDirectoryContentDeterministically) {
  const auto directory = std::filesystem::path("vektor_test_bag_artifact");
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory / "nested");
  {
    std::ofstream data(directory / "data.db3", std::ios::binary);
    data << "bag-data";
    std::ofstream metadata(directory / "nested" / "metadata.yaml");
    metadata << "version: 9\n";
  }
  const auto first = vektor::fingerprint_run_artifact(directory, "rosbag2");
  const auto second = vektor::fingerprint_run_artifact(directory, "rosbag2");
  EXPECT_EQ(first.sha256, second.sha256);
  EXPECT_EQ(first.size_bytes, 19U);
  EXPECT_EQ(first.kind, "rosbag2");
  EXPECT_FALSE(first.uri.empty());
  std::filesystem::remove_all(directory);
}

TEST(RunEvents, ImportsOnlyRunHealthTransitionsAndNewDeploymentEvents) {
  const auto directory = std::filesystem::path("vektor_test_run_events");
  const auto health = std::filesystem::path("vektor_test_event_health.jsonl");
  const auto audit = std::filesystem::path("vektor_test_event_audit.jsonl");
  std::filesystem::remove_all(directory);
  std::filesystem::remove(health);
  std::filesystem::remove(audit);
  {
    std::ofstream existing(audit);
    existing << "{\"schema_version\":1,\"timestamp\":\"old\","
                "\"action\":\"deployment.prepare\","
                "\"outcome\":\"started\"}\n";
  }

  auto run = definition("event-run");
  run.health_history_path = health.string();
  run.deployment_audit_path = audit.string();
  const vektor::RunStore store(directory);
  const auto started = store.start(run);
  {
    std::ofstream history(health);
    history << "{\"schema_version\":1,\"timestamp\":\"" << started.started_at
            << "\",\"robot_id\":\"robot-1\",\"state\":\"healthy\"}\n"
            << "{\"schema_version\":1,\"timestamp\":\"" << started.started_at
            << "\",\"robot_id\":\"robot-1\",\"state\":\"healthy\"}\n"
            << "{\"schema_version\":1,\"timestamp\":\"" << started.started_at
            << "\",\"robot_id\":\"robot-1\",\"state\":\"unhealthy\"}\n";
  }
  {
    std::ofstream appended(audit, std::ios::app);
    appended << "{\"schema_version\":1,\"timestamp\":\"" << started.started_at
             << "\",\"actor\":\"agent\","
                "\"action\":\"deployment.activate\","
                "\"outcome\":\"succeeded\","
                "\"deployment_id\":\"release-2\","
                "\"phase\":\"active\",\"operation\":\"none\","
                "\"message\":\"ready\"}\n";
  }

  const auto completed = store.stop("event-run", "passed", {"reviewed"});
  EXPECT_EQ(std::count_if(completed.events.begin(), completed.events.end(),
                          [](const auto &event) {
                            return event.type == "health_transition";
                          }),
            2);
  EXPECT_EQ(std::count_if(completed.events.begin(), completed.events.end(),
                          [](const auto &event) {
                            return event.type == "deployment_event";
                          }),
            1);
  EXPECT_EQ(completed.annotations, std::vector<std::string>({"reviewed"}));
  EXPECT_TRUE(std::none_of(
      completed.events.begin(), completed.events.end(), [](const auto &event) {
        return event.message.find("deployment.prepare") != std::string::npos;
      }));

  std::filesystem::remove_all(directory);
  std::filesystem::remove(health);
  std::filesystem::remove(audit);
}

TEST(RunEvents, PreservesWarningWhenImportedEventLimitIsReached) {
  const auto directory = std::filesystem::path("vektor_test_run_event_limit");
  const auto health = std::filesystem::path("vektor_test_event_limit.jsonl");
  const auto audit =
      std::filesystem::path("vektor_test_event_limit_audit.jsonl");
  std::filesystem::remove_all(directory);
  std::filesystem::remove(health);
  std::filesystem::remove(audit);

  auto run = definition("event-limit-run");
  run.health_history_path = health.string();
  run.deployment_audit_path = audit.string();
  run.max_imported_events = 2;
  const vektor::RunStore store(directory);
  const auto started = store.start(run);
  {
    std::ofstream history(health);
    history << "{\"timestamp\":\"" << started.started_at
            << "\",\"state\":\"healthy\"}\n"
            << "{\"timestamp\":\"" << started.started_at
            << "\",\"state\":\"unhealthy\"}\n"
            << "{\"timestamp\":\"" << started.started_at
            << "\",\"state\":\"healthy\"}\n";
  }

  const auto completed = store.stop("event-limit-run", "passed");
  EXPECT_EQ(std::count_if(completed.events.begin(), completed.events.end(),
                          [](const auto &event) {
                            return event.type == "event_import_warning" &&
                                   event.message.find("event limit") !=
                                       std::string::npos;
                          }),
            1);

  std::filesystem::remove_all(directory);
  std::filesystem::remove(health);
  std::filesystem::remove(audit);
}

TEST(RunComparison, ProducesStableOutcomeParameterMetricAndEventDifferences) {
  auto baseline = definition("baseline-run");
  baseline.status = vektor::RunStatus::Completed;
  baseline.outcome = "passed";
  baseline.parameters = {{"mode", "safe"}, {"speed", "0.5"}};
  baseline.metrics = {{"elapsed_s", 10.0}, {"goal_error_m", 0.4}};
  baseline.events = {{"health_transition", "2026-08-30T10:00:00Z",
                      "robot=robot-1 state=healthy"},
                     {"deployment_event", "2026-08-30T10:00:01Z",
                      "action=deployment.activate outcome=succeeded"}};

  auto candidate = definition("candidate-run");
  candidate.status = vektor::RunStatus::Completed;
  candidate.outcome = "failed";
  candidate.parameters = {{"controller", "mpc"}, {"speed", "0.7"}};
  candidate.metrics = {{"elapsed_s", 8.5}, {"path_length_m", 12.0}};
  candidate.events = {{"health_transition", "2026-08-30T11:00:00Z",
                       "robot=robot-1 state=healthy"},
                      {"health_transition", "2026-08-30T11:00:01Z",
                       "robot=robot-1 state=unhealthy"}};

  const auto comparison = vektor::compare_runs(baseline, candidate);
  EXPECT_TRUE(comparison.different());
  ASSERT_EQ(comparison.parameters.size(), 3U);
  EXPECT_EQ(comparison.parameters.front().key, "controller");
  ASSERT_EQ(comparison.metrics.size(), 3U);
  EXPECT_EQ(comparison.metrics.front().key, "elapsed_s");
  ASSERT_TRUE(comparison.metrics.front().delta);
  EXPECT_DOUBLE_EQ(*comparison.metrics.front().delta, -1.5);
  ASSERT_EQ(comparison.events.size(), 2U);
  EXPECT_EQ(comparison.events.front().type, "deployment_event");
  const auto json = vektor::run_comparison_to_json(comparison);
  EXPECT_NE(json.find("\"schema_version\":1"), std::string::npos);
  EXPECT_NE(json.find("\"changed\":true"), std::string::npos);
  EXPECT_NE(json.find("\"name\":\"elapsed_s\""), std::string::npos);
  EXPECT_NE(json.find("\"delta\":-1.5"), std::string::npos);
}

TEST(RunComparison, RejectsAnActiveRun) {
  auto baseline = definition("baseline-active");
  auto candidate = definition("candidate-completed");
  candidate.status = vektor::RunStatus::Completed;
  EXPECT_THROW(vektor::compare_runs(baseline, candidate),
               std::invalid_argument);
}
