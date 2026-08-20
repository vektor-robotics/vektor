#include "vektor/runtime.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string>

namespace {
const char *environment(const char *name) {
  const auto *value = std::getenv(name);
  return value && *value ? value : nullptr;
}
} // namespace

TEST(OciRuntimeIntegration, ActivatesInspectsAndRemovesDigestPinnedWorkload) {
  const auto *runtime_environment = environment("VEKTOR_TEST_OCI_RUNTIME");
  const auto *artifact_environment = environment("VEKTOR_TEST_OCI_ARTIFACT");
  if (!runtime_environment || !artifact_environment)
    GTEST_SKIP() << "set VEKTOR_TEST_OCI_RUNTIME and VEKTOR_TEST_OCI_ARTIFACT "
                    "to run real OCI qualification";
  const std::string runtime = runtime_environment;
  const std::string artifact = artifact_environment;
  const auto suffix = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  vektor::OciRuntimeDriver driver(runtime, "vektor-runtime-it-" + suffix);
  vektor::WorkloadSpec workload;
  workload.command = {"sleep", "30"};

  try {
    driver.prepare(artifact, std::chrono::minutes(2));
    const auto active = driver.activate(artifact, workload,
                                        std::chrono::minutes(2),
                                        std::chrono::seconds(15));
    EXPECT_TRUE(active.running);
    EXPECT_TRUE(active.ready);
    EXPECT_TRUE(active.managed);
    EXPECT_EQ(active.artifact, artifact);
    EXPECT_EQ(active.workload_fingerprint, vektor::workload_fingerprint(workload));
    EXPECT_TRUE(driver.inspect(std::chrono::seconds(15)).running);
    EXPECT_NO_THROW(driver.stop(std::chrono::seconds(15)));
    EXPECT_FALSE(driver.inspect(std::chrono::seconds(15)).running);
  } catch (...) {
    try { driver.stop(std::chrono::seconds(15)); } catch (...) {}
    throw;
  }
}
