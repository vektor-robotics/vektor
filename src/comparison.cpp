#include "vektor/comparison.hpp"

#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace vektor {
namespace {
std::string json_escape(std::string_view value) {
  std::ostringstream output;
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\b':
      output << "\\b";
      break;
    case '\f':
      output << "\\f";
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
      if (character < 0x20)
        output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
               << static_cast<unsigned int>(character) << std::dec;
      else
        output << static_cast<char>(character);
    }
  }
  return output.str();
}

template <typename Value>
std::optional<Value> find_value(const std::map<std::string, Value> &values,
                                const std::string &key) {
  const auto found = values.find(key);
  if (found == values.end())
    return std::nullopt;
  return found->second;
}

template <typename Value>
std::set<std::string> combined_keys(
    const std::map<std::string, Value> &baseline,
    const std::map<std::string, Value> &candidate) {
  std::set<std::string> keys;
  for (const auto &[key, value] : baseline) {
    static_cast<void>(value);
    keys.insert(key);
  }
  for (const auto &[key, value] : candidate) {
    static_cast<void>(value);
    keys.insert(key);
  }
  return keys;
}

void append_optional_string(std::ostringstream &output,
                            const std::optional<std::string> &value) {
  if (value)
    output << '"' << json_escape(*value) << '"';
  else
    output << "null";
}

void append_optional_number(std::ostringstream &output,
                            const std::optional<double> &value) {
  if (value)
    output << std::setprecision(17) << *value;
  else
    output << "null";
}
} // namespace

bool RunComparison::different() const {
  return baseline_outcome != candidate_outcome || !parameters.empty() ||
         !metrics.empty() || !events.empty();
}

RunComparison compare_runs(const RunManifest &baseline,
                           const RunManifest &candidate) {
  if (baseline.status != RunStatus::Completed ||
      candidate.status != RunStatus::Completed)
    throw std::invalid_argument("run comparison requires two completed runs");

  RunComparison comparison;
  comparison.baseline_run_id = baseline.run_id;
  comparison.candidate_run_id = candidate.run_id;
  comparison.baseline_outcome = baseline.outcome;
  comparison.candidate_outcome = candidate.outcome;

  for (const auto &key : combined_keys(baseline.parameters,
                                       candidate.parameters)) {
    const auto left = find_value(baseline.parameters, key);
    const auto right = find_value(candidate.parameters, key);
    if (left != right)
      comparison.parameters.push_back({key, left, right});
  }

  for (const auto &key : combined_keys(baseline.metrics, candidate.metrics)) {
    const auto left = find_value(baseline.metrics, key);
    const auto right = find_value(candidate.metrics, key);
    if (left != right)
      comparison.metrics.push_back(
          {key, left, right, left && right
                                 ? std::optional<double>(*right - *left)
                                 : std::nullopt});
  }

  using EventKey = std::pair<std::string, std::string>;
  std::map<EventKey, std::pair<std::size_t, std::size_t>> counts;
  for (const auto &event : baseline.events)
    ++counts[{event.type, event.message}].first;
  for (const auto &event : candidate.events)
    ++counts[{event.type, event.message}].second;
  for (const auto &[key, count] : counts) {
    if (count.first == count.second)
      continue;
    comparison.events.push_back(
        {key.first, key.second, count.first, count.second,
         static_cast<std::int64_t>(count.second) -
             static_cast<std::int64_t>(count.first)});
  }
  return comparison;
}

std::string run_comparison_to_json(const RunComparison &comparison) {
  std::ostringstream output;
  output << "{\"schema_version\":" << comparison.schema_version
         << ",\"baseline_run_id\":\""
         << json_escape(comparison.baseline_run_id)
         << "\",\"candidate_run_id\":\""
         << json_escape(comparison.candidate_run_id)
         << "\",\"different\":"
         << (comparison.different() ? "true" : "false")
         << ",\"outcome\":{\"baseline\":\""
         << json_escape(comparison.baseline_outcome)
         << "\",\"candidate\":\""
         << json_escape(comparison.candidate_outcome) << "\",\"changed\":"
         << (comparison.baseline_outcome != comparison.candidate_outcome
                 ? "true"
                 : "false")
         << "},\"parameters\":[";
  for (std::size_t index = 0; index < comparison.parameters.size(); ++index) {
    if (index != 0)
      output << ',';
    const auto &difference = comparison.parameters[index];
    output << "{\"name\":\"" << json_escape(difference.key)
           << "\",\"baseline\":";
    append_optional_string(output, difference.baseline);
    output << ",\"candidate\":";
    append_optional_string(output, difference.candidate);
    output << '}';
  }
  output << "],\"metrics\":[";
  for (std::size_t index = 0; index < comparison.metrics.size(); ++index) {
    if (index != 0)
      output << ',';
    const auto &difference = comparison.metrics[index];
    output << "{\"name\":\"" << json_escape(difference.key)
           << "\",\"baseline\":";
    append_optional_number(output, difference.baseline);
    output << ",\"candidate\":";
    append_optional_number(output, difference.candidate);
    output << ",\"delta\":";
    append_optional_number(output, difference.delta);
    output << '}';
  }
  output << "],\"events\":[";
  for (std::size_t index = 0; index < comparison.events.size(); ++index) {
    if (index != 0)
      output << ',';
    const auto &difference = comparison.events[index];
    output << "{\"type\":\"" << json_escape(difference.type)
           << "\",\"message\":\"" << json_escape(difference.message)
           << "\",\"baseline_count\":" << difference.baseline_count
           << ",\"candidate_count\":" << difference.candidate_count
           << ",\"delta\":" << difference.delta << '}';
  }
  output << "]}";
  return output.str();
}

void print_run_comparison(const RunComparison &comparison,
                          std::ostream &output) {
  output << "VEKTOR RUN COMPARISON\nbaseline: "
         << comparison.baseline_run_id
         << "\ncandidate: " << comparison.candidate_run_id
         << "\ndifferent: " << (comparison.different() ? "yes" : "no")
         << "\noutcome: " << comparison.baseline_outcome << " -> "
         << comparison.candidate_outcome << "\nparameters:\n";
  for (const auto &difference : comparison.parameters)
    output << "  " << difference.key << ": "
           << difference.baseline.value_or("<missing>") << " -> "
           << difference.candidate.value_or("<missing>") << '\n';
  output << "metrics:\n";
  for (const auto &difference : comparison.metrics) {
    output << "  " << difference.key << ": ";
    if (difference.baseline)
      output << std::setprecision(17) << *difference.baseline;
    else
      output << "<missing>";
    output << " -> ";
    if (difference.candidate)
      output << std::setprecision(17) << *difference.candidate;
    else
      output << "<missing>";
    if (difference.delta)
      output << " (delta " << std::setprecision(17) << *difference.delta
             << ')';
    output << '\n';
  }
  output << "events:\n";
  for (const auto &difference : comparison.events)
    output << "  " << difference.type << " [" << difference.message << "]: "
           << difference.baseline_count << " -> "
           << difference.candidate_count << " (delta " << difference.delta
           << ")\n";
}

} // namespace vektor
