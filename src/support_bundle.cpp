#include "vektor/support_bundle.hpp"
#include <openssl/sha.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace vektor {
namespace {
void bounded_copy(const std::filesystem::path &source, const std::filesystem::path &destination) {
  std::ifstream input(source); if (!input) return;
  std::vector<std::string> lines; std::string line;
  while (std::getline(input, line)) { lines.push_back(std::move(line)); if (lines.size() > 50) lines.erase(lines.begin()); }
  std::ofstream output(destination); for (const auto &value : lines) output << value << '\n';
}
}
void create_support_bundle(const std::filesystem::path &output, const std::filesystem::path &config, const std::filesystem::path &history, const std::filesystem::path &metrics) {
  if (output.empty() || config.empty() || std::filesystem::exists(output)) throw std::invalid_argument("support-bundle requires a new --output directory and --config");
  std::ifstream input(config, std::ios::binary); if (!input) throw std::runtime_error("cannot read support-bundle configuration");
  SHA256_CTX context; SHA256_Init(&context); char buffer[4096];
  while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) SHA256_Update(&context, buffer, input.gcount());
  unsigned char digest[SHA256_DIGEST_LENGTH]; SHA256_Final(digest, &context);
  std::filesystem::create_directories(output); std::ofstream manifest(output / "manifest.txt");
  manifest << "schema_version: 1\nvektor_version: 1.0.0\nhealth_config_sha256: ";
  for (const auto byte : digest) manifest << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(byte);
  manifest << "\nredaction: configuration, credentials, trust, approval, authorization, and audit contents are excluded\n";
  bounded_copy(history, output / "status-history.jsonl");
  bounded_copy(metrics, output / "metrics.prom");
}
} // namespace vektor
