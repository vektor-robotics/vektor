#include "vektor/runtime.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <stdexcept>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern char **environ;

namespace vektor {
namespace {
struct CommandResult {
  int exit_code;
  std::string output;
};

std::string trim(std::string value) {
  const auto whitespace = [](unsigned char character) {
    return std::isspace(character);
  };
  value.erase(value.begin(),
              std::find_if_not(value.begin(), value.end(), whitespace));
  value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(),
              value.end());
  return value;
}

CommandResult run_command(const std::string &executable,
                          const std::vector<std::string> &arguments) {
  std::array<int, 2> output_pipe{};
  if (pipe(output_pipe.data()) != 0)
    throw std::runtime_error("failed to create runtime output pipe: " +
                             std::string(std::strerror(errno)));

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(output_pipe[0]);
    close(output_pipe[1]);
    throw std::runtime_error("failed to initialize runtime process");
  }
  posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, output_pipe[1]);

  std::vector<std::string> storage;
  storage.reserve(arguments.size() + 1);
  storage.push_back(executable);
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  argv.reserve(storage.size() + 1);
  for (auto &item : storage)
    argv.push_back(item.data());
  argv.push_back(nullptr);

  pid_t process = 0;
  const int spawn_result = posix_spawnp(&process, executable.c_str(), &actions,
                                        nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(output_pipe[1]);
  if (spawn_result != 0) {
    close(output_pipe[0]);
    throw std::runtime_error("failed to start OCI runtime '" + executable +
                             "': " + std::to_string(spawn_result));
  }

  std::string output;
  std::array<char, 4096> buffer{};
  for (;;) {
    const auto count = read(output_pipe[0], buffer.data(), buffer.size());
    if (count > 0) {
      output.append(buffer.data(), static_cast<std::size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    break;
  }
  close(output_pipe[0]);

  int status = 0;
  while (waitpid(process, &status, 0) < 0) {
    if (errno != EINTR)
      throw std::runtime_error("failed waiting for OCI runtime");
  }
  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  return {exit_code, trim(std::move(output))};
}

void require_success(const CommandResult &result, std::string_view operation) {
  if (result.exit_code == 0)
    return;
  const auto detail = result.output.empty()
                          ? "exit code " + std::to_string(result.exit_code)
                          : result.output;
  throw std::runtime_error("OCI runtime " + std::string(operation) +
                           " failed: " + detail);
}

void validate_executable(const std::string &value) {
  if (value.empty() ||
      std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character);
      }))
    throw std::invalid_argument("OCI runtime must be one executable path");
}
} // namespace

bool is_valid_runtime_container_name(const std::string &value) {
  return !value.empty() &&
         std::isalnum(static_cast<unsigned char>(value.front())) &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isalnum(character) || character == '_' ||
                  character == '.' || character == '-';
         });
}

OciRuntimeDriver::OciRuntimeDriver(std::string executable,
                                   std::string container_name)
    : executable_(std::move(executable)),
      container_name_(std::move(container_name)) {
  validate_executable(executable_);
  if (!is_valid_runtime_container_name(container_name_))
    throw std::invalid_argument("invalid runtime container name");
}

void OciRuntimeDriver::prepare(const std::string &artifact) {
  require_success(run_command(executable_, {"pull", artifact}), "pull");
}

RuntimeObservation OciRuntimeDriver::activate(const std::string &artifact) {
  const auto existing = inspect();
  if (!existing.artifact.empty() && !existing.managed)
    throw std::runtime_error("refusing to replace unmanaged container '" +
                             container_name_ + "'");
  // Removal is intentionally idempotent. A non-existent workload is expected
  // for the first deployment and does not make activation fail.
  run_command(executable_, {"rm", "--force", container_name_});
  require_success(
      run_command(executable_,
                  {"run", "--detach", "--name", container_name_, "--network",
                   "host", "--restart", "unless-stopped", "--label",
                   "io.vektor.managed=true", artifact}),
      "activation");
  const auto observed = inspect();
  if (!observed.running || !observed.managed || observed.artifact != artifact)
    throw std::runtime_error(
        "OCI runtime did not activate the requested digest");
  return observed;
}

RuntimeObservation OciRuntimeDriver::stop() {
  const auto existing = inspect();
  if (existing.artifact.empty())
    return {};
  if (!existing.managed)
    throw std::runtime_error("refusing to stop unmanaged container '" +
                             container_name_ + "'");
  const auto result =
      run_command(executable_, {"rm", "--force", container_name_});
  if (result.exit_code != 0) {
    const auto observed = inspect();
    if (observed.running)
      require_success(result, "stop");
  }
  return {};
}

RuntimeObservation OciRuntimeDriver::inspect() {
  const auto result = run_command(
      executable_, {"inspect", "--format",
                    "{{.Config.Image}}|{{.Id}}|"
                    "{{index .Config.Labels \"io.vektor.managed\"}}|"
                    "{{.State.Running}}",
                    container_name_});
  if (result.exit_code != 0)
    return {};
  const auto first = result.output.find('|');
  const auto second = result.output.find('|', first + 1);
  const auto third = result.output.find('|', second + 1);
  if (first == std::string::npos || first == 0 || second == std::string::npos ||
      third == std::string::npos)
    throw std::runtime_error(
        "OCI runtime returned an invalid inspection result");
  return {result.output.substr(third + 1) == "true",
          result.output.substr(0, first),
          result.output.substr(first + 1, second - first - 1),
          result.output.substr(second + 1, third - second - 1) == "true"};
}

} // namespace vektor
