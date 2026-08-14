#pragma once

#include "vektor/health_inspector.hpp"

#include <iosfwd>
#include <vector>

namespace vektor {
void print_results(const std::vector<CheckResult> &results, std::ostream &out);
void print_results_json(const std::vector<CheckResult> &results,
                        std::ostream &out);
bool all_checks_passed(const std::vector<CheckResult> &results);
} // namespace vektor
