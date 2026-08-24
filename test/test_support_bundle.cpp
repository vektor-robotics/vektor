#include "vektor/support_bundle.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

TEST(SupportBundle, WritesOnlyFingerprintMetadata) {
  const auto config = std::filesystem::path("vektor_support_config.yaml");
  const auto output = std::filesystem::path("vektor_support_bundle");
  std::filesystem::remove_all(output);
  { std::ofstream file(config); file << "robot_id: robot-1\nsecret: do-not-copy\n"; }
  vektor::create_support_bundle(output, config);
  std::ifstream manifest(output / "manifest.txt");
  const std::string text((std::istreambuf_iterator<char>(manifest)), {});
  EXPECT_NE(text.find("health_config_sha256:"), std::string::npos);
  EXPECT_EQ(text.find("do-not-copy"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(output / "policy.yaml"));
  EXPECT_THROW(vektor::create_support_bundle(output, config), std::invalid_argument);
  std::filesystem::remove(config); std::filesystem::remove_all(output);
}
