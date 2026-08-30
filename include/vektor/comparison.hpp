#pragma once

#include "vektor/run.hpp"

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace vektor {

struct StringDifference {
  std::string key;
  std::optional<std::string> baseline;
  std::optional<std::string> candidate;
};

struct MetricDifference {
  std::string key;
  std::optional<double> baseline;
  std::optional<double> candidate;
  std::optional<double> delta;
};

struct EventDifference {
  std::string type;
  std::string message;
  std::size_t baseline_count{0};
  std::size_t candidate_count{0};
  std::int64_t delta{0};
};

struct RunComparison {
  unsigned int schema_version{1};
  std::string baseline_run_id;
  std::string candidate_run_id;
  std::string baseline_outcome;
  std::string candidate_outcome;
  std::vector<StringDifference> parameters;
  std::vector<MetricDifference> metrics;
  std::vector<EventDifference> events;

  bool different() const;
};

RunComparison compare_runs(const RunManifest &baseline,
                           const RunManifest &candidate);
std::string run_comparison_to_json(const RunComparison &comparison);
void print_run_comparison(const RunComparison &comparison,
                          std::ostream &output);

} // namespace vektor
