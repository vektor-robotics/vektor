#include "vektor/audit.hpp"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {
std::string read_all(const std::filesystem::path &path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::vector<YAML::Node> read_events(const std::filesystem::path &path) {
  std::ifstream input(path);
  std::vector<YAML::Node> events;
  std::string line;
  while (std::getline(input, line))
    events.push_back(YAML::Load(line));
  return events;
}
} // namespace

TEST(AuditLog, AppendsStructuredJsonWithoutRewritingPriorEvents) {
  const auto path = std::filesystem::path("vektor_test_audit.jsonl");
  std::filesystem::remove(path);
  {
    vektor::JsonLinesAuditLog log(path);
    log.append({"mtls:operator@example.com", "deployment.prepare", "started",
                "release-1", "image@sha256:digest", "idle", "preparing",
                "requested \"safe\" rollout\nwave one"});
  }
  const auto first_bytes = read_all(path);
  ASSERT_FALSE(first_bytes.empty());
  {
    vektor::JsonLinesAuditLog restarted(path);
    restarted.append({"agent", "artifact.verify", "succeeded", "release-1",
                      "image@sha256:digest", "idle", "none", "trusted"});
  }
  const auto complete = read_all(path);
  EXPECT_TRUE(complete.starts_with(first_bytes));
  const auto events = read_events(path);
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0]["schema_version"].as<unsigned int>(), 1U);
  EXPECT_EQ(events[0]["actor"].as<std::string>(),
            "mtls:operator@example.com");
  EXPECT_EQ(events[0]["message"].as<std::string>(),
            "requested \"safe\" rollout\nwave one");
  EXPECT_FALSE(events[0]["timestamp"].as<std::string>().empty());
  EXPECT_EQ(events[1]["action"].as<std::string>(), "artifact.verify");
  std::filesystem::remove(path);
}

TEST(AuditLog, SerializesConcurrentAppendsAsCompleteRecords) {
  const auto path = std::filesystem::path("vektor_test_audit_concurrent.jsonl");
  std::filesystem::remove(path);
  vektor::JsonLinesAuditLog first_log(path);
  vektor::JsonLinesAuditLog second_log(path);
  std::vector<std::thread> writers;
  for (int writer = 0; writer < 4; ++writer) {
    writers.emplace_back([&, writer] {
      for (int event = 0; event < 10; ++event)
        (writer % 2 == 0 ? first_log : second_log)
            .append({"agent-" + std::to_string(writer), "runtime.inspect",
                     "succeeded", "release-1", "artifact", "active", "none",
                     std::to_string(event)});
    });
  }
  for (auto &writer : writers)
    writer.join();
  const auto events = read_events(path);
  ASSERT_EQ(events.size(), 40U);
  for (const auto &event : events) {
    EXPECT_EQ(event["schema_version"].as<unsigned int>(), 1U);
    EXPECT_EQ(event["action"].as<std::string>(), "runtime.inspect");
  }
  std::filesystem::remove(path);
}

TEST(AuditLog, RejectsAnUnwritableTarget) {
  const auto path = std::filesystem::path("vektor_test_audit_directory");
  std::filesystem::remove_all(path);
  std::filesystem::create_directory(path);
  vektor::JsonLinesAuditLog log(path);
  EXPECT_THROW(log.append({"agent", "test", "started"}), std::runtime_error);
  std::filesystem::remove_all(path);
}

TEST(AuditLog, RefusesToFollowSymbolicLinks) {
  const auto target = std::filesystem::path("vektor_test_audit_target.txt");
  const auto link = std::filesystem::path("vektor_test_audit_link.jsonl");
  std::filesystem::remove(target);
  std::filesystem::remove(link);
  {
    std::ofstream output(target);
    output << "protected\n";
  }
  std::error_code error;
  std::filesystem::create_symlink(target, link, error);
  if (error) {
    std::filesystem::remove(target);
    GTEST_SKIP() << "symbolic links unavailable: " << error.message();
  }
  vektor::JsonLinesAuditLog log(link);
  EXPECT_THROW(log.append({"agent", "test", "started"}), std::runtime_error);
  EXPECT_EQ(read_all(target), "protected\n");
  std::filesystem::remove(link);
  std::filesystem::remove(target);
}
