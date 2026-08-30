#pragma once

#include "vektor/run.hpp"

#include <filesystem>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace vektor {

enum class MetricObjective { Minimize, Maximize };

struct MetricPolicy {
  std::string name;
  MetricObjective objective{MetricObjective::Minimize};
  double weight{1.0};
  double scale{1.0};
  double tolerance{0.0};
};

struct ExperimentCandidateDefinition {
  std::string candidate_id;
  std::string run_id;
};

struct ExperimentDefinition {
  unsigned int schema_version{1};
  std::string experiment_id;
  std::string baseline_run_id;
  std::string required_outcome;
  std::vector<MetricPolicy> metrics;
  std::vector<ExperimentCandidateDefinition> candidates;
};

struct MetricEvaluation {
  std::string name;
  MetricObjective objective{MetricObjective::Minimize};
  double weight{1.0};
  double scale{1.0};
  double tolerance{0.0};
  double baseline{0.0};
  double candidate{0.0};
  double delta{0.0};
  double contribution{0.0};
};

struct CandidateScore {
  std::string candidate_id;
  std::string run_id;
  std::string artifact;
  std::map<std::string, std::string> parameters;
  std::string outcome;
  bool eligible{true};
  std::vector<std::string> rejection_reasons;
  std::vector<MetricEvaluation> metrics;
  double score{0.0};
  std::size_t rank{0};
};

struct ExperimentManifest {
  unsigned int schema_version{1};
  std::string experiment_id;
  std::string baseline_run_id;
  std::string baseline_artifact;
  std::map<std::string, std::string> baseline_parameters;
  std::string required_outcome;
  std::vector<MetricPolicy> policy;
  std::vector<CandidateScore> candidates;
  bool automatic_deployment{false};
  std::string created_at;
};

const char *metric_objective_name(MetricObjective objective);
ExperimentDefinition
load_experiment_definition(const std::filesystem::path &path);
ExperimentManifest
score_experiment(const ExperimentDefinition &definition, const RunStore &runs,
                 const std::filesystem::path &experiment_directory);
std::string experiment_manifest_to_json(const ExperimentManifest &manifest);
void print_experiment_manifest(const ExperimentManifest &manifest,
                               std::ostream &output);

} // namespace vektor
