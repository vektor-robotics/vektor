#include "vektor/evaluation.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {
constexpr auto kArtifact =
    "registry.example/robot@sha256:"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

vektor::RunManifest run_definition(const std::string &run_id) {
  vektor::RunManifest manifest;
  manifest.run_id = run_id;
  manifest.name = run_id;
  manifest.artifact = kArtifact;
  manifest.workload_id = "navigation";
  manifest.policy = "policy.yaml";
  manifest.policy_sha256 =
      "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  manifest.parameters["speed"] = "0.5";
  manifest.environment["site"] = "lab";
  manifest.topics = {"/tf"};
  manifest.health_history_path = "health.jsonl";
  manifest.deployment_audit_path = "audit.jsonl";
  manifest.robot_id = "robot-1";
  manifest.operator_id = "operator@example.com";
  return manifest;
}

void completed_run(const vektor::RunStore &store, const std::string &run_id,
                   const std::string &outcome,
                   const std::map<std::string, double> &metrics) {
  store.start(run_definition(run_id));
  store.stop(run_id, outcome, {}, metrics);
}

vektor::ExperimentDefinition experiment_definition() {
  vektor::ExperimentDefinition definition;
  definition.experiment_id = "navigation-candidates-001";
  definition.baseline_run_id = "baseline-001";
  definition.required_outcome = "passed";
  definition.metrics = {
      {"goal_error_m", vektor::MetricObjective::Minimize, 1.0, 0.1, 0.01},
      {"elapsed_time_s", vektor::MetricObjective::Minimize, 0.25, 10.0, 0.5}};
  definition.candidates = {{"candidate-a", "candidate-001"},
                           {"candidate-b", "candidate-002"}};
  return definition;
}
} // namespace

TEST(Evaluation, ScoresAndRanksCandidatesDeterministically) {
  const auto run_directory = std::filesystem::path("vektor_test_score_runs");
  const auto experiment_directory =
      std::filesystem::path("vektor_test_experiments");
  std::filesystem::remove_all(run_directory);
  std::filesystem::remove_all(experiment_directory);
  const vektor::RunStore store(run_directory);
  completed_run(store, "baseline-001", "passed",
                {{"goal_error_m", 0.5}, {"elapsed_time_s", 100.0}});
  completed_run(store, "candidate-001", "passed",
                {{"goal_error_m", 0.3}, {"elapsed_time_s", 105.0}});
  completed_run(store, "candidate-002", "passed",
                {{"goal_error_m", 0.4}, {"elapsed_time_s", 90.0}});

  const auto manifest = vektor::score_experiment(experiment_definition(), store,
                                                 experiment_directory);
  ASSERT_EQ(manifest.candidates.size(), 2U);
  EXPECT_EQ(manifest.candidates[0].candidate_id, "candidate-a");
  EXPECT_EQ(manifest.candidates[0].rank, 1U);
  EXPECT_NEAR(manifest.candidates[0].score, 1.875, 1e-12);
  EXPECT_EQ(manifest.candidates[1].rank, 2U);
  EXPECT_NEAR(manifest.candidates[1].score, 1.25, 1e-12);
  EXPECT_FALSE(manifest.automatic_deployment);
  EXPECT_TRUE(std::filesystem::exists(experiment_directory /
                                      "navigation-candidates-001.yaml"));
  const auto json = vektor::experiment_manifest_to_json(manifest);
  EXPECT_NE(json.find("\"automatic_deployment\":false"), std::string::npos);
  EXPECT_NE(json.find("\"contribution\":2"), std::string::npos);
  EXPECT_THROW(vektor::score_experiment(experiment_definition(), store,
                                        experiment_directory),
               std::runtime_error);

  std::filesystem::remove_all(run_directory);
  std::filesystem::remove_all(experiment_directory);
}

TEST(Evaluation, RejectsCandidatesThatMissOutcomeOrMetricRequirements) {
  const auto run_directory =
      std::filesystem::path("vektor_test_ineligible_runs");
  const auto experiment_directory =
      std::filesystem::path("vektor_test_ineligible_experiments");
  std::filesystem::remove_all(run_directory);
  std::filesystem::remove_all(experiment_directory);
  const vektor::RunStore store(run_directory);
  completed_run(store, "baseline-001", "passed",
                {{"goal_error_m", 0.5}, {"elapsed_time_s", 100.0}});
  completed_run(store, "candidate-001", "failed",
                {{"goal_error_m", 0.3}, {"elapsed_time_s", 105.0}});
  completed_run(store, "candidate-002", "passed", {{"goal_error_m", 0.4}});

  const auto manifest = vektor::score_experiment(experiment_definition(), store,
                                                 experiment_directory);
  ASSERT_EQ(manifest.candidates.size(), 2U);
  EXPECT_FALSE(manifest.candidates[0].eligible);
  EXPECT_FALSE(manifest.candidates[1].eligible);
  EXPECT_EQ(manifest.candidates[0].rank, 0U);
  EXPECT_EQ(manifest.candidates[1].rank, 0U);
  EXPECT_EQ(manifest.candidates[0].rejection_reasons.size(), 1U);
  EXPECT_EQ(manifest.candidates[1].rejection_reasons.size(), 1U);

  std::filesystem::remove_all(run_directory);
  std::filesystem::remove_all(experiment_directory);
}

TEST(Evaluation, LoadsStrictVersionedDefinition) {
  const auto path = std::filesystem::path("vektor_test_experiment.yaml");
  {
    std::ofstream output(path);
    output << "schema_version: 1\n"
              "experiment_id: experiment-001\n"
              "baseline_run_id: baseline-001\n"
              "required_outcome: passed\n"
              "metrics:\n"
              "  - {name: success_rate, objective: maximize, weight: 2, "
              "scale: 0.1, tolerance: 0.01}\n"
              "candidates:\n"
              "  - {candidate_id: a, run_id: candidate-001}\n"
              "  - {candidate_id: b, run_id: candidate-002}\n";
  }
  const auto definition = vektor::load_experiment_definition(path);
  EXPECT_EQ(definition.metrics[0].objective, vektor::MetricObjective::Maximize);
  EXPECT_DOUBLE_EQ(definition.metrics[0].weight, 2.0);

  {
    std::ofstream output(path, std::ios::trunc);
    output << "schema_version: 1\nexperiment_id: bad\n"
              "baseline_run_id: baseline\nunknown: true\n"
              "metrics: []\ncandidates: []\n";
  }
  EXPECT_THROW(vektor::load_experiment_definition(path), std::invalid_argument);
  std::filesystem::remove(path);
}
