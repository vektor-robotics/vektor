#include "vektor/evaluation.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace vektor {
namespace {
constexpr std::size_t kMaxMetrics = 32;
constexpr std::size_t kMaxCandidates = 32;

std::string json_escape(const std::string &value) {
  std::ostringstream output;
  for (const auto character : value) {
    switch (character) {
    case '\\':
      output << "\\\\";
      break;
    case '"':
      output << "\\\"";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      output << character;
    }
  }
  return output.str();
}

std::string number(double value) {
  std::ostringstream output;
  output << std::setprecision(17) << value;
  return output.str();
}

std::string utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

void reject_unknown(const YAML::Node &node,
                    const std::set<std::string> &allowed,
                    const std::string &field) {
  if (!node.IsMap())
    throw std::invalid_argument(field + " must be a map");
  for (const auto &entry : node) {
    const auto key = entry.first.as<std::string>();
    if (!allowed.contains(key))
      throw std::invalid_argument("unknown " + field + " field '" + key + "'");
  }
}

std::string required_scalar(const YAML::Node &node, const char *field,
                            const std::string &context) {
  if (!node[field] || !node[field].IsScalar())
    throw std::invalid_argument(context + " requires '" + field + "'");
  const auto value = node[field].as<std::string>();
  if (value.empty())
    throw std::invalid_argument(context + " field '" + field +
                                "' cannot be empty");
  return value;
}

void validate_id(const std::string &id, const char *field) {
  static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$");
  if (!std::regex_match(id, pattern))
    throw std::invalid_argument(std::string("invalid ") + field + " '" + id +
                                "'");
}

MetricObjective parse_objective(const std::string &value) {
  if (value == "minimize")
    return MetricObjective::Minimize;
  if (value == "maximize")
    return MetricObjective::Maximize;
  throw std::invalid_argument(
      "experiment metric objective must be minimize or maximize");
}

void validate_definition(const ExperimentDefinition &definition) {
  validate_id(definition.experiment_id, "experiment ID");
  validate_id(definition.baseline_run_id, "baseline run ID");
  if (definition.metrics.empty() || definition.metrics.size() > kMaxMetrics)
    throw std::invalid_argument("experiment requires 1 to 32 metric policies");
  if (definition.candidates.size() < 2 ||
      definition.candidates.size() > kMaxCandidates)
    throw std::invalid_argument("experiment requires 2 to 32 candidates");

  static const std::regex metric_pattern("^[A-Za-z][A-Za-z0-9_.-]{0,127}$");
  std::set<std::string> metric_names;
  for (const auto &metric : definition.metrics) {
    if (!std::regex_match(metric.name, metric_pattern))
      throw std::invalid_argument("invalid experiment metric '" + metric.name +
                                  "'");
    if (!metric_names.insert(metric.name).second)
      throw std::invalid_argument("duplicate experiment metric '" +
                                  metric.name + "'");
    if (!std::isfinite(metric.weight) || metric.weight <= 0.0 ||
        !std::isfinite(metric.scale) || metric.scale <= 0.0 ||
        !std::isfinite(metric.tolerance) || metric.tolerance < 0.0)
      throw std::invalid_argument("metric weight and scale must be positive "
                                  "and tolerance non-negative");
  }

  std::set<std::string> candidate_ids;
  std::set<std::string> run_ids;
  for (const auto &candidate : definition.candidates) {
    validate_id(candidate.candidate_id, "candidate ID");
    validate_id(candidate.run_id, "candidate run ID");
    if (!candidate_ids.insert(candidate.candidate_id).second)
      throw std::invalid_argument("duplicate candidate ID '" +
                                  candidate.candidate_id + "'");
    if (!run_ids.insert(candidate.run_id).second)
      throw std::invalid_argument("duplicate candidate run ID '" +
                                  candidate.run_id + "'");
    if (candidate.run_id == definition.baseline_run_id)
      throw std::invalid_argument("baseline run cannot also be a candidate");
  }
}

void require_completed(const RunManifest &run, const std::string &role) {
  if (run.status != RunStatus::Completed)
    throw std::invalid_argument(role + " run '" + run.run_id +
                                "' must be completed");
}

YAML::Node encode(const ExperimentManifest &manifest) {
  YAML::Node node;
  node["schema_version"] = manifest.schema_version;
  node["experiment_id"] = manifest.experiment_id;
  node["baseline_run_id"] = manifest.baseline_run_id;
  node["baseline_artifact"] = manifest.baseline_artifact;
  node["baseline_parameters"] = manifest.baseline_parameters;
  node["required_outcome"] = manifest.required_outcome;
  node["automatic_deployment"] = manifest.automatic_deployment;
  node["created_at"] = manifest.created_at;
  for (const auto &metric : manifest.policy) {
    YAML::Node item;
    item["name"] = metric.name;
    item["objective"] = metric_objective_name(metric.objective);
    item["weight"] = metric.weight;
    item["scale"] = metric.scale;
    item["tolerance"] = metric.tolerance;
    node["policy"].push_back(item);
  }
  for (const auto &candidate : manifest.candidates) {
    YAML::Node item;
    item["candidate_id"] = candidate.candidate_id;
    item["run_id"] = candidate.run_id;
    item["artifact"] = candidate.artifact;
    item["parameters"] = candidate.parameters;
    item["outcome"] = candidate.outcome;
    item["eligible"] = candidate.eligible;
    item["rejection_reasons"] = candidate.rejection_reasons;
    item["score"] = candidate.score;
    item["rank"] = candidate.rank;
    for (const auto &metric : candidate.metrics) {
      YAML::Node evaluation;
      evaluation["name"] = metric.name;
      evaluation["objective"] = metric_objective_name(metric.objective);
      evaluation["weight"] = metric.weight;
      evaluation["scale"] = metric.scale;
      evaluation["tolerance"] = metric.tolerance;
      evaluation["baseline"] = metric.baseline;
      evaluation["candidate"] = metric.candidate;
      evaluation["delta"] = metric.delta;
      evaluation["contribution"] = metric.contribution;
      item["metrics"].push_back(evaluation);
    }
    node["candidates"].push_back(item);
  }
  return node;
}

void append_string_map(std::ostringstream &output,
                       const std::map<std::string, std::string> &values) {
  output << '{';
  std::size_t index = 0;
  for (const auto &[key, value] : values) {
    if (index++ != 0)
      output << ',';
    output << '"' << json_escape(key) << "\":\"" << json_escape(value) << '"';
  }
  output << '}';
}
} // namespace

const char *metric_objective_name(MetricObjective objective) {
  return objective == MetricObjective::Minimize ? "minimize" : "maximize";
}

ExperimentDefinition
load_experiment_definition(const std::filesystem::path &path) {
  YAML::Node node;
  try {
    node = YAML::LoadFile(path.string());
  } catch (const YAML::Exception &error) {
    throw std::invalid_argument("failed to load experiment definition: " +
                                std::string(error.what()));
  }
  if (!node["schema_version"] || node["schema_version"].as<unsigned int>() != 1)
    throw std::invalid_argument(
        "experiment definition requires schema_version: 1");
  reject_unknown(node,
                 {"schema_version", "experiment_id", "baseline_run_id",
                  "required_outcome", "metrics", "candidates"},
                 "experiment definition");

  ExperimentDefinition definition;
  definition.experiment_id =
      required_scalar(node, "experiment_id", "experiment definition");
  definition.baseline_run_id =
      required_scalar(node, "baseline_run_id", "experiment definition");
  if (node["required_outcome"])
    definition.required_outcome =
        required_scalar(node, "required_outcome", "experiment definition");
  if (!node["metrics"] || !node["metrics"].IsSequence())
    throw std::invalid_argument("experiment metrics must be a sequence");
  for (std::size_t index = 0; index < node["metrics"].size(); ++index) {
    const auto item = node["metrics"][index];
    const auto context = "experiment metrics[" + std::to_string(index) + "]";
    reject_unknown(item, {"name", "objective", "weight", "scale", "tolerance"},
                   context);
    MetricPolicy metric;
    metric.name = required_scalar(item, "name", context);
    metric.objective =
        parse_objective(required_scalar(item, "objective", context));
    metric.weight = item["weight"] ? item["weight"].as<double>() : 1.0;
    metric.scale = item["scale"] ? item["scale"].as<double>() : 1.0;
    metric.tolerance = item["tolerance"] ? item["tolerance"].as<double>() : 0.0;
    definition.metrics.push_back(metric);
  }
  if (!node["candidates"] || !node["candidates"].IsSequence())
    throw std::invalid_argument("experiment candidates must be a sequence");
  for (std::size_t index = 0; index < node["candidates"].size(); ++index) {
    const auto item = node["candidates"][index];
    const auto context = "experiment candidates[" + std::to_string(index) + "]";
    reject_unknown(item, {"candidate_id", "run_id"}, context);
    definition.candidates.push_back(
        {required_scalar(item, "candidate_id", context),
         required_scalar(item, "run_id", context)});
  }
  validate_definition(definition);
  return definition;
}

ExperimentManifest
score_experiment(const ExperimentDefinition &definition, const RunStore &runs,
                 const std::filesystem::path &experiment_directory) {
  validate_definition(definition);
  const auto root =
      std::filesystem::absolute(experiment_directory).lexically_normal();
  std::filesystem::create_directories(root);
  const auto path = root / (definition.experiment_id + ".yaml");
  if (std::filesystem::exists(path))
    throw std::runtime_error("experiment '" + definition.experiment_id +
                             "' already exists");

  const auto baseline = runs.get(definition.baseline_run_id);
  require_completed(baseline, "baseline");
  for (const auto &metric : definition.metrics)
    if (!baseline.metrics.contains(metric.name))
      throw std::invalid_argument("baseline run is missing metric '" +
                                  metric.name + "'");

  ExperimentManifest manifest;
  manifest.experiment_id = definition.experiment_id;
  manifest.baseline_run_id = baseline.run_id;
  manifest.baseline_artifact = baseline.artifact;
  manifest.baseline_parameters = baseline.parameters;
  manifest.required_outcome = definition.required_outcome;
  manifest.policy = definition.metrics;
  manifest.created_at = utc_timestamp();

  for (const auto &candidate_definition : definition.candidates) {
    const auto run = runs.get(candidate_definition.run_id);
    require_completed(run, "candidate");
    CandidateScore candidate;
    candidate.candidate_id = candidate_definition.candidate_id;
    candidate.run_id = run.run_id;
    candidate.artifact = run.artifact;
    candidate.parameters = run.parameters;
    candidate.outcome = run.outcome;
    if (!definition.required_outcome.empty() &&
        run.outcome != definition.required_outcome) {
      candidate.eligible = false;
      candidate.rejection_reasons.push_back(
          "outcome '" + run.outcome + "' does not match required outcome '" +
          definition.required_outcome + "'");
    }
    for (const auto &policy : definition.metrics) {
      const auto value = run.metrics.find(policy.name);
      if (value == run.metrics.end()) {
        candidate.eligible = false;
        candidate.rejection_reasons.push_back("missing metric '" + policy.name +
                                              "'");
        continue;
      }
      const auto baseline_value = baseline.metrics.at(policy.name);
      const auto delta = value->second - baseline_value;
      double directional =
          policy.objective == MetricObjective::Maximize ? delta : -delta;
      if (std::abs(delta) <= policy.tolerance)
        directional = 0.0;
      const auto contribution = policy.weight * directional / policy.scale;
      if (!std::isfinite(delta) || !std::isfinite(contribution) ||
          !std::isfinite(candidate.score + contribution))
        throw std::invalid_argument("metric '" + policy.name +
                                    "' produced a non-finite score");
      candidate.metrics.push_back({policy.name, policy.objective, policy.weight,
                                   policy.scale, policy.tolerance,
                                   baseline_value, value->second, delta,
                                   contribution});
      candidate.score += contribution;
    }
    manifest.candidates.push_back(std::move(candidate));
  }

  std::sort(manifest.candidates.begin(), manifest.candidates.end(),
            [](const CandidateScore &left, const CandidateScore &right) {
              if (left.eligible != right.eligible)
                return left.eligible > right.eligible;
              if (left.eligible && left.score != right.score)
                return left.score > right.score;
              return left.candidate_id < right.candidate_id;
            });
  std::size_t rank = 1;
  for (auto &candidate : manifest.candidates)
    if (candidate.eligible)
      candidate.rank = rank++;

  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output)
      throw std::runtime_error("failed to persist experiment manifest");
    output << encode(manifest);
    if (!output)
      throw std::runtime_error("failed to persist experiment manifest");
  }
  std::filesystem::rename(temporary, path);
  return manifest;
}

std::string experiment_manifest_to_json(const ExperimentManifest &manifest) {
  std::ostringstream output;
  output << "{\"schema_version\":" << manifest.schema_version
         << ",\"experiment_id\":\"" << json_escape(manifest.experiment_id)
         << "\",\"baseline_run_id\":\"" << json_escape(manifest.baseline_run_id)
         << "\",\"baseline_artifact\":\""
         << json_escape(manifest.baseline_artifact)
         << "\",\"baseline_parameters\":";
  append_string_map(output, manifest.baseline_parameters);
  output << ",\"required_outcome\":\"" << json_escape(manifest.required_outcome)
         << "\",\"automatic_deployment\":"
         << (manifest.automatic_deployment ? "true" : "false")
         << ",\"policy\":[";
  for (std::size_t index = 0; index < manifest.policy.size(); ++index) {
    if (index != 0)
      output << ',';
    const auto &metric = manifest.policy[index];
    output << "{\"name\":\"" << json_escape(metric.name)
           << "\",\"objective\":\"" << metric_objective_name(metric.objective)
           << "\",\"weight\":" << number(metric.weight)
           << ",\"scale\":" << number(metric.scale)
           << ",\"tolerance\":" << number(metric.tolerance) << '}';
  }
  output << "],\"candidates\":[";
  for (std::size_t index = 0; index < manifest.candidates.size(); ++index) {
    if (index != 0)
      output << ',';
    const auto &candidate = manifest.candidates[index];
    output << "{\"candidate_id\":\"" << json_escape(candidate.candidate_id)
           << "\",\"run_id\":\"" << json_escape(candidate.run_id)
           << "\",\"artifact\":\"" << json_escape(candidate.artifact)
           << "\",\"parameters\":";
    append_string_map(output, candidate.parameters);
    output << ",\"outcome\":\"" << json_escape(candidate.outcome)
           << "\",\"eligible\":" << (candidate.eligible ? "true" : "false")
           << ",\"rejection_reasons\":[";
    for (std::size_t reason = 0; reason < candidate.rejection_reasons.size();
         ++reason) {
      if (reason != 0)
        output << ',';
      output << '"' << json_escape(candidate.rejection_reasons[reason]) << '"';
    }
    output << "],\"metrics\":[";
    for (std::size_t metric_index = 0; metric_index < candidate.metrics.size();
         ++metric_index) {
      if (metric_index != 0)
        output << ',';
      const auto &metric = candidate.metrics[metric_index];
      output << "{\"name\":\"" << json_escape(metric.name)
             << "\",\"objective\":\"" << metric_objective_name(metric.objective)
             << "\",\"weight\":" << number(metric.weight)
             << ",\"scale\":" << number(metric.scale)
             << ",\"tolerance\":" << number(metric.tolerance)
             << ",\"baseline\":" << number(metric.baseline)
             << ",\"candidate\":" << number(metric.candidate)
             << ",\"delta\":" << number(metric.delta)
             << ",\"contribution\":" << number(metric.contribution) << '}';
    }
    output << "],\"score\":" << number(candidate.score)
           << ",\"rank\":" << candidate.rank << '}';
  }
  output << "],\"created_at\":\"" << json_escape(manifest.created_at) << "\"}";
  return output.str();
}

void print_experiment_manifest(const ExperimentManifest &manifest,
                               std::ostream &output) {
  output << "experiment: " << manifest.experiment_id << '\n'
         << "baseline: " << manifest.baseline_run_id << '\n'
         << "automatic deployment: disabled\n";
  for (const auto &candidate : manifest.candidates) {
    output << (candidate.eligible ? std::to_string(candidate.rank) : "-")
           << ". " << candidate.candidate_id << " (run " << candidate.run_id
           << "): score " << number(candidate.score)
           << (candidate.eligible ? "" : " [ineligible]") << '\n';
    for (const auto &reason : candidate.rejection_reasons)
      output << "   reason: " << reason << '\n';
  }
}

} // namespace vektor
