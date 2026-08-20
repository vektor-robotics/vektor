#include "vektor/reporter.hpp"

#include <ostream>
#include <string_view>

namespace vektor {
namespace {
const char *status_name(CheckStatus status) {
  if (status == CheckStatus::Pass)
    return "pass";
  if (status == CheckStatus::Warn)
    return "warn";
  return "fail";
}

void print_json_string(std::string_view value, std::ostream &out) {
  out << '"';
  for (const char character : value) {
    switch (character) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      out << character;
    }
  }
  out << '"';
}
} // namespace

void print_results(const std::vector<CheckResult> &results, std::ostream &out) {
  for (const auto &result : results) {
    const char *label = result.status == CheckStatus::Pass   ? "PASS"
                        : result.status == CheckStatus::Warn ? "WARN"
                                                             : "FAIL";
    out << '[' << label << "] " << result.category << " " << result.target
        << ": " << result.message << '\n';
  }
  out << "\n"
      << (all_checks_passed(results) ? "VEKTOR CHECK PASSED"
                                     : "VEKTOR CHECK FAILED")
      << '\n';
}

void print_results_json(const std::vector<CheckResult> &results,
                        std::ostream &out) {
  std::size_t passed = 0;
  std::size_t warned = 0;
  std::size_t failed = 0;
  for (const auto &result : results) {
    if (result.status == CheckStatus::Pass)
      ++passed;
    else if (result.status == CheckStatus::Warn)
      ++warned;
    else
      ++failed;
  }

  out << "{\"schema_version\":1,\"ok\":" << (failed == 0 ? "true" : "false")
      << ",\"summary\":{\"pass\":" << passed << ",\"warn\":" << warned
      << ",\"fail\":" << failed << "},\"checks\":[";
  for (std::size_t index = 0; index < results.size(); ++index) {
    if (index > 0)
      out << ',';
    const auto &result = results[index];
    out << "{\"status\":";
    print_json_string(status_name(result.status), out);
    out << ",\"category\":";
    print_json_string(result.category, out);
    out << ",\"target\":";
    print_json_string(result.target, out);
    out << ",\"message\":";
    print_json_string(result.message, out);
    out << '}';
  }
  out << "]}\n";
}

bool all_checks_passed(const std::vector<CheckResult> &results) {
  for (const auto &result : results)
    if (result.status == CheckStatus::Fail)
      return false;
  return true;
}
} // namespace vektor
