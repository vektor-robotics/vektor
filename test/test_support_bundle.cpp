#include "vektor/support_bundle.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

TEST(SupportBundle, WritesOnlyFingerprintMetadata) {
  const auto config = std::filesystem::path("vektor_support_config.yaml");
  const auto output = std::filesystem::path("vektor_support_bundle");
  const auto history = std::filesystem::path("vektor_support_history.jsonl");
  const auto metrics = std::filesystem::path("vektor_support_metrics.prom");
  std::filesystem::remove_all(output);
  { std::ofstream file(config); file << "robot_id: robot-1\nsecret: do-not-copy\n"; }
  { std::ofstream file(history); file << "status diagnostic\n"; }
  { std::ofstream file(metrics); file << "vektor_rpc_requests_total 1\n"; }
  vektor::create_support_bundle(output, config, history, metrics);
  std::ifstream manifest(output / "manifest.txt");
  const std::string text((std::istreambuf_iterator<char>(manifest)), {});
  EXPECT_NE(text.find("health_config_sha256: "
                      "c61e7a850043336b00c3d2319e11d84739925df07a9b5735fe703dac06cbb6e8"),
            std::string::npos);
  EXPECT_EQ(text.find("do-not-copy"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(output / "policy.yaml"));
  EXPECT_TRUE(std::filesystem::exists(output / "status-history.jsonl"));
  EXPECT_TRUE(std::filesystem::exists(output / "metrics.prom"));
  EXPECT_THROW(vektor::create_support_bundle(output, config, history, metrics), std::invalid_argument);
  std::filesystem::remove(config); std::filesystem::remove(history); std::filesystem::remove(metrics); std::filesystem::remove_all(output);
}
