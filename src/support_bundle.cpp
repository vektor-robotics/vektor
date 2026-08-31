#include "vektor/support_bundle.hpp"
#include <openssl/evp.h>
#include <array>
#include <fstream>
#include <iomanip>
#include <memory>
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
  using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("failed to initialize support-bundle fingerprint");
  std::array<char, 4096> buffer{};
  while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
    if (EVP_DigestUpdate(context.get(), buffer.data(),
                         static_cast<std::size_t>(input.gcount())) != 1)
      throw std::runtime_error("failed to fingerprint support-bundle configuration");
  }
  if (!input.eof())
    throw std::runtime_error("failed to read support-bundle configuration");
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1)
    throw std::runtime_error("failed to finalize support-bundle fingerprint");
  std::filesystem::create_directories(output); std::ofstream manifest(output / "manifest.txt");
  manifest << "schema_version: 1\nvektor_version: 1.0.0\nhealth_config_sha256: ";
  for (unsigned int index = 0; index < digest_size; ++index)
    manifest << std::hex << std::setw(2) << std::setfill('0')
             << static_cast<unsigned int>(digest[index]);
  manifest << "\nredaction: configuration, credentials, trust, approval, authorization, and audit contents are excluded\n";
  bounded_copy(history, output / "status-history.jsonl");
  bounded_copy(metrics, output / "metrics.prom");
}
} // namespace vektor
