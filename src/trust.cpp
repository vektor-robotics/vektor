#include "vektor/trust.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <poll.h>
#include <set>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern char **environ;

namespace vektor {
namespace {
struct ProcessResult {
  int exit_code;
  std::string output;
  bool timed_out{false};
};

void append_bounded(std::string &output, const char *data, std::size_t size) {
  constexpr std::size_t maximum = 64 * 1024;
  if (output.size() >= maximum)
    return;
  output.append(data, std::min(size, maximum - output.size()));
}

std::string utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

void validate_executable(const std::string &value) {
  if (value.empty() || value.find('\0') != std::string::npos ||
      value.find('\n') != std::string::npos ||
      value.find('\r') != std::string::npos)
    throw std::invalid_argument("cosign executable must be one path");
}

std::string require_string(const YAML::Node &node, const std::string &field) {
  if (!node || !node.IsScalar())
    throw std::runtime_error("trust policy field '" + field +
                             "' must be a non-empty string");
  const auto value = node.as<std::string>();
  if (value.empty() || value.find('\0') != std::string::npos ||
      value.find('\n') != std::string::npos ||
      value.find('\r') != std::string::npos)
    throw std::runtime_error("trust policy field '" + field +
                             "' must be a non-empty single-line string");
  return value;
}

void reject_unknown(const YAML::Node &root) {
  const std::set<std::string> allowed{"schema_version",
                                      "mode",
                                      "cosign_executable",
                                      "key",
                                      "certificate_identity",
                                      "certificate_oidc_issuer",
                                      "allow_http_registry",
                                      "ignore_transparency_log",
                                      "timeout_ms"};
  if (!root || !root.IsMap())
    throw std::runtime_error("trust policy must be a mapping");
  for (const auto &entry : root) {
    const auto key = entry.first.as<std::string>();
    if (!allowed.contains(key))
      throw std::runtime_error("unknown trust policy field '" + key + "'");
  }
}

ProcessResult run_process(const std::string &executable,
                          const std::vector<std::string> &arguments,
                          std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0)
    throw std::invalid_argument(
        "artifact verification timeout must be positive");
  std::array<int, 2> output_pipe{};
  if (pipe(output_pipe.data()) != 0)
    throw std::runtime_error("failed to create verifier output pipe: " +
                             std::string(std::strerror(errno)));

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(output_pipe[0]);
    close(output_pipe[1]);
    throw std::runtime_error("failed to initialize verifier process");
  }
  posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, output_pipe[1]);

  std::vector<std::string> storage{executable};
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  argv.reserve(storage.size() + 1);
  for (auto &item : storage)
    argv.push_back(item.data());
  argv.push_back(nullptr);

  posix_spawnattr_t attributes;
  if (posix_spawnattr_init(&attributes) != 0) {
    posix_spawn_file_actions_destroy(&actions);
    close(output_pipe[0]);
    close(output_pipe[1]);
    throw std::runtime_error("failed to initialize verifier attributes");
  }
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attributes, 0);
  pid_t process = 0;
  const int spawn_result = posix_spawnp(&process, executable.c_str(), &actions,
                                        &attributes, argv.data(), environ);
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  close(output_pipe[1]);
  if (spawn_result != 0) {
    close(output_pipe[0]);
    throw std::runtime_error("failed to start cosign verifier '" + executable +
                             "': " + std::string(std::strerror(spawn_result)));
  }

  const auto flags = fcntl(output_pipe[0], F_GETFL, 0);
  if (flags >= 0)
    fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);
  std::string output;
  std::array<char, 4096> buffer{};
  int status = 0;
  bool exited = false;
  bool timed_out = false;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!exited) {
    for (;;) {
      const auto count = read(output_pipe[0], buffer.data(), buffer.size());
      if (count > 0) {
        append_bounded(output, buffer.data(), static_cast<std::size_t>(count));
        continue;
      }
      if (count < 0 && errno == EINTR)
        continue;
      break;
    }
    const auto waited = waitpid(process, &status, WNOHANG);
    if (waited == process) {
      exited = true;
      break;
    }
    if (waited < 0 && errno != EINTR) {
      close(output_pipe[0]);
      throw std::runtime_error("failed waiting for cosign verifier");
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      timed_out = true;
      if (kill(-process, SIGKILL) != 0)
        kill(process, SIGKILL);
      while (waitpid(process, &status, 0) < 0 && errno == EINTR) {
      }
      exited = true;
      break;
    }
    pollfd descriptor{output_pipe[0], POLLIN, 0};
    poll(&descriptor, 1, 20);
  }
  for (;;) {
    const auto count = read(output_pipe[0], buffer.data(), buffer.size());
    if (count > 0) {
      append_bounded(output, buffer.data(), static_cast<std::size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    break;
  }
  close(output_pipe[0]);
  return {WIFEXITED(status) ? WEXITSTATUS(status) : 128, std::move(output),
          timed_out};
}
} // namespace

TrustPolicy load_trust_policy(const std::filesystem::path &path) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path.string());
    reject_unknown(root);
    if (!root["schema_version"] ||
        root["schema_version"].as<unsigned int>() != 1)
      throw std::runtime_error("trust policy schema_version must be 1");
    TrustPolicy policy;
    const auto mode = require_string(root["mode"], "mode");
    if (mode == "public_key") {
      policy.mode = TrustMode::PublicKey;
      policy.key = require_string(root["key"], "key");
      if (root["certificate_identity"] || root["certificate_oidc_issuer"])
        throw std::runtime_error(
            "public_key policy cannot include certificate identity fields");
      if (policy.key.find("://") == std::string::npos) {
        auto key_path = std::filesystem::path(policy.key);
        if (key_path.is_relative())
          key_path = std::filesystem::absolute(path).parent_path() / key_path;
        policy.key = key_path.lexically_normal().string();
      }
    } else if (mode == "keyless") {
      policy.mode = TrustMode::Keyless;
      policy.certificate_identity =
          require_string(root["certificate_identity"], "certificate_identity");
      policy.certificate_oidc_issuer = require_string(
          root["certificate_oidc_issuer"], "certificate_oidc_issuer");
      if (root["key"])
        throw std::runtime_error("keyless policy cannot include key");
    } else {
      throw std::runtime_error(
          "trust policy mode must be public_key or keyless");
    }
    if (root["cosign_executable"])
      policy.cosign_executable =
          require_string(root["cosign_executable"], "cosign_executable");
    validate_executable(policy.cosign_executable);
    policy.allow_http_registry = root["allow_http_registry"]
                                     ? root["allow_http_registry"].as<bool>()
                                     : false;
    policy.ignore_transparency_log =
        root["ignore_transparency_log"]
            ? root["ignore_transparency_log"].as<bool>()
            : false;
    if (policy.mode == TrustMode::Keyless &&
        (policy.allow_http_registry || policy.ignore_transparency_log))
      throw std::runtime_error(
          "keyless policy cannot weaken registry or transparency-log checks");
    const auto timeout =
        root["timeout_ms"] ? root["timeout_ms"].as<long long>() : 30000;
    if (timeout <= 0 || timeout > 24LL * 60 * 60 * 1000)
      throw std::runtime_error(
          "trust policy timeout_ms must be between 1 and 86400000");
    policy.timeout = std::chrono::milliseconds(timeout);
    return policy;
  } catch (const std::exception &error) {
    throw std::runtime_error("failed to load trust policy '" + path.string() +
                             "': " + error.what());
  }
}

CosignArtifactVerifier::CosignArtifactVerifier(TrustPolicy policy)
    : policy_(std::move(policy)) {
  validate_executable(policy_.cosign_executable);
  if (policy_.mode == TrustMode::Keyless &&
      (policy_.allow_http_registry || policy_.ignore_transparency_log))
    throw std::invalid_argument("keyless verification cannot weaken registry "
                                "or transparency-log checks");
}

ArtifactVerification
CosignArtifactVerifier::verify(const std::string &artifact,
                               std::chrono::milliseconds operation_timeout) {
  const auto timeout = std::min(policy_.timeout, operation_timeout);
  std::vector<std::string> arguments{"verify"};
  ArtifactVerification verification;
  verification.method = policy_.mode == TrustMode::PublicKey
                            ? "cosign_public_key"
                            : "cosign_keyless";
  if (policy_.mode == TrustMode::PublicKey) {
    arguments.insert(arguments.end(), {"--key", policy_.key});
    verification.signer = policy_.key;
  } else {
    arguments.insert(arguments.end(),
                     {"--certificate-identity", policy_.certificate_identity,
                      "--certificate-oidc-issuer",
                      policy_.certificate_oidc_issuer});
    verification.signer = policy_.certificate_identity;
    verification.issuer = policy_.certificate_oidc_issuer;
  }
  if (policy_.allow_http_registry)
    arguments.push_back("--allow-http-registry");
  if (policy_.ignore_transparency_log)
    arguments.push_back("--insecure-ignore-tlog");
  arguments.push_back(artifact);
  const auto result =
      run_process(policy_.cosign_executable, arguments, timeout);
  if (result.timed_out)
    throw std::runtime_error("cosign verification timed out");
  if (result.exit_code != 0) {
    auto detail = result.output;
    detail.erase(std::remove_if(detail.begin(), detail.end(),
                                [](char value) {
                                  const auto character =
                                      static_cast<unsigned char>(value);
                                  return (character < 32 && value != '\n' &&
                                          value != '\t') ||
                                         character == 127;
                                }),
                 detail.end());
    std::replace(detail.begin(), detail.end(), '\n', ' ');
    std::replace(detail.begin(), detail.end(), '\t', ' ');
    while (!detail.empty() && detail.back() == ' ')
      detail.pop_back();
    if (detail.size() > 512)
      detail.resize(512);
    throw std::runtime_error("cosign rejected artifact" +
                             (detail.empty() ? std::string{} : ": " + detail));
  }
  verification.verified = true;
  verification.verified_at = utc_timestamp();
  return verification;
}

} // namespace vektor
