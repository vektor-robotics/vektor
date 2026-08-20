#include "vektor/reporter.hpp"

#include <gtest/gtest.h>

#include <sstream>

TEST(Reporter, ProducesMachineReadableJson) {
  const std::vector<vektor::CheckResult> results{
      {vektor::CheckStatus::Pass, "node", "/planner", "node is present"},
      {vektor::CheckStatus::Fail, "topic", "/quoted", "missing \"publisher\""}};
  std::ostringstream output;
  vektor::print_results_json(results, output);
  EXPECT_EQ(
      output.str(),
      "{\"schema_version\":1,\"ok\":false,\"summary\":{\"pass\":1,\"warn\":0,\"fail\":1},\"checks\":"
      "["
      "{\"status\":\"pass\",\"category\":\"node\",\"target\":\"/planner\","
      "\"message\":\"node is present\"},"
      "{\"status\":\"fail\",\"category\":\"topic\",\"target\":\"/quoted\","
      "\"message\":\"missing \\\"publisher\\\"\"}]}\n");
}
