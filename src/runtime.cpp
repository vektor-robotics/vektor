#include "vektor/runtime.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <poll.h>
#include <regex>
#include <set>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern char **environ;

namespace vektor {
namespace {
struct CommandResult {
  int exit_code;
  std::string output;
  bool timed_out{false};
  bool output_truncated{false};
};

constexpr std::size_t kMaximumRuntimeOutputBytes = 64 * 1024;

void append_bounded_output(std::string &output, bool &truncated,
                           const char *data, std::size_t count) {
  const auto remaining = kMaximumRuntimeOutputBytes - output.size();
  const auto appended = std::min(count, remaining);
  output.append(data, appended);
  truncated = truncated || appended != count;
}

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
                          const std::vector<std::string> &arguments,
                          std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0)
    throw std::invalid_argument("runtime operation timeout must be positive");
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

  posix_spawnattr_t attributes;
  if (posix_spawnattr_init(&attributes) != 0) {
    posix_spawn_file_actions_destroy(&actions);
    close(output_pipe[0]);
    close(output_pipe[1]);
    throw std::runtime_error("failed to initialize runtime process attributes");
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
    throw std::runtime_error("failed to start OCI runtime '" + executable +
                             "': " + std::to_string(spawn_result));
  }

  const auto flags = fcntl(output_pipe[0], F_GETFL, 0);
  if (flags >= 0)
    fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);
  std::string output;
  bool output_truncated = false;
  std::array<char, 4096> buffer{};
  int status = 0;
  bool exited = false;
  bool timed_out = false;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!exited) {
    for (;;) {
      const auto count = read(output_pipe[0], buffer.data(), buffer.size());
      if (count > 0) {
        append_bounded_output(output, output_truncated, buffer.data(),
                              static_cast<std::size_t>(count));
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
      throw std::runtime_error("failed waiting for OCI runtime");
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
      append_bounded_output(output, output_truncated, buffer.data(),
                            static_cast<std::size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    break;
  }
  close(output_pipe[0]);
  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  return {exit_code, trim(std::move(output)), timed_out, output_truncated};
}

void require_success(const CommandResult &result, std::string_view operation) {
  if (result.timed_out)
    throw std::runtime_error("OCI runtime " + std::string(operation) +
                             " timed out");
  if (result.exit_code == 0)
    return;
  const auto detail = result.output.empty()
                          ? "exit code " + std::to_string(result.exit_code)
                          : result.output;
  throw std::runtime_error("OCI runtime " + std::string(operation) +
                           " failed: " + detail +
                           (result.output_truncated
                                ? "\n[OCI runtime output truncated]"
                                : ""));
}

std::chrono::milliseconds
remaining_until(std::chrono::steady_clock::time_point deadline,
                std::string_view operation) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline)
    throw std::runtime_error("OCI runtime " + std::string(operation) +
                             " timed out");
  return std::max(
      std::chrono::milliseconds(1),
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
}

void validate_executable(const std::string &value) {
  if (value.empty() ||
      std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character);
      }))
    throw std::invalid_argument("OCI runtime must be one executable path");
}

bool contains_invalid_argument_character(const std::string &value) {
  return value.find('\0') != std::string::npos ||
         value.find('\n') != std::string::npos ||
         value.find('\r') != std::string::npos;
}

void append_fingerprint_value(std::ostringstream &out,
                              const std::string &value) {
  out << value.size() << ':' << value;
}

std::uint64_t fnv1a(const std::string &value, std::uint64_t hash) {
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}
} // namespace

void RuntimeDriver::prepare(const std::string &artifact,
                            std::chrono::milliseconds) {
  prepare(artifact);
}

RuntimeObservation RuntimeDriver::activate(const std::string &artifact,
                                           const WorkloadSpec &spec,
                                           std::chrono::milliseconds,
                                           std::chrono::milliseconds) {
  return activate(artifact, spec);
}

RuntimeObservation RuntimeDriver::stop(std::chrono::milliseconds) {
  return stop();
}

RuntimeObservation RuntimeDriver::inspect(std::chrono::milliseconds) {
  return inspect();
}

const char *network_mode_name(NetworkMode mode) {
  switch (mode) {
  case NetworkMode::Host:
    return "host";
  case NetworkMode::Bridge:
    return "bridge";
  case NetworkMode::None:
    return "none";
  }
  return "unknown";
}

NetworkMode parse_network_mode(const std::string &value) {
  if (value == "host")
    return NetworkMode::Host;
  if (value == "bridge")
    return NetworkMode::Bridge;
  if (value == "none")
    return NetworkMode::None;
  throw std::invalid_argument("network must be host, bridge, or none");
}

void validate_workload_spec(const WorkloadSpec &spec) {
  static const std::set<std::string> restart_policies{
      "no", "always", "unless-stopped", "on-failure"};
  static const std::regex environment_name(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
  if (!restart_policies.contains(spec.restart_policy))
    throw std::invalid_argument(
        "restart_policy must be no, always, unless-stopped, or on-failure");
  for (const auto &[name, value] : spec.environment) {
    if (!std::regex_match(name, environment_name))
      throw std::invalid_argument("invalid environment variable name '" + name +
                                  "'");
    if (contains_invalid_argument_character(value))
      throw std::invalid_argument("environment values cannot contain newlines");
  }
  std::set<std::string> mount_targets;
  for (const auto &mount : spec.mounts) {
    if (!std::filesystem::path(mount.source).is_absolute() ||
        !std::filesystem::path(mount.target).is_absolute())
      throw std::invalid_argument("mount source and target must be absolute");
    if (contains_invalid_argument_character(mount.source) ||
        contains_invalid_argument_character(mount.target) ||
        mount.source.find(',') != std::string::npos ||
        mount.target.find(',') != std::string::npos)
      throw std::invalid_argument(
          "mount paths contain an unsupported character");
    if (!mount_targets.insert(mount.target).second)
      throw std::invalid_argument("duplicate mount target '" + mount.target +
                                  "'");
  }
  std::set<std::string> device_targets;
  for (const auto &device : spec.devices) {
    if (!std::filesystem::path(device.host_path).is_absolute() ||
        !std::filesystem::path(device.container_path).is_absolute())
      throw std::invalid_argument("device paths must be absolute");
    if (contains_invalid_argument_character(device.host_path) ||
        contains_invalid_argument_character(device.container_path) ||
        device.host_path.find(':') != std::string::npos ||
        device.container_path.find(':') != std::string::npos)
      throw std::invalid_argument(
          "device paths contain an unsupported character");
    if (!device_targets.insert(device.container_path).second)
      throw std::invalid_argument("duplicate device target '" +
                                  device.container_path + "'");
  }
  for (const auto &argument : spec.command) {
    if (argument.empty() || contains_invalid_argument_character(argument))
      throw std::invalid_argument(
          "command arguments must be non-empty and cannot contain newlines");
  }
}

bool is_default_workload_spec(const WorkloadSpec &spec) {
  return spec == WorkloadSpec{};
}

std::string workload_fingerprint(const WorkloadSpec &spec) {
  validate_workload_spec(spec);
  std::ostringstream out;
  append_fingerprint_value(out, network_mode_name(spec.network));
  append_fingerprint_value(out, spec.restart_policy);
  for (const auto &[name, value] : spec.environment) {
    append_fingerprint_value(out, name);
    append_fingerprint_value(out, value);
  }
  out << '|';
  for (const auto &mount : spec.mounts) {
    append_fingerprint_value(out, mount.source);
    append_fingerprint_value(out, mount.target);
    out << (mount.read_only ? '1' : '0');
  }
  out << '|';
  for (const auto &device : spec.devices) {
    append_fingerprint_value(out, device.host_path);
    append_fingerprint_value(out, device.container_path);
  }
  out << '|';
  for (const auto &argument : spec.command)
    append_fingerprint_value(out, argument);
  const auto canonical = out.str();
  const auto first = fnv1a(canonical, 14695981039346656037ULL);
  const auto second = fnv1a(canonical, 1099511628211ULL);
  std::ostringstream fingerprint;
  fingerprint << std::hex << std::setfill('0') << std::setw(16) << first
              << std::setw(16) << second;
  return fingerprint.str();
}

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
  prepare(artifact, std::chrono::minutes(5));
}

void OciRuntimeDriver::prepare(const std::string &artifact,
                               std::chrono::milliseconds operation_timeout) {
  require_success(
      run_command(executable_, {"pull", artifact}, operation_timeout), "pull");
}

RuntimeObservation OciRuntimeDriver::activate(const std::string &artifact,
                                              const WorkloadSpec &spec) {
  return activate(artifact, spec, std::chrono::minutes(5),
                  std::chrono::seconds(30));
}

RuntimeObservation
OciRuntimeDriver::activate(const std::string &artifact,
                           const WorkloadSpec &spec,
                           std::chrono::milliseconds operation_timeout,
                           std::chrono::milliseconds readiness_timeout) {
  validate_workload_spec(spec);
  if (operation_timeout.count() <= 0 || readiness_timeout.count() <= 0)
    throw std::invalid_argument("runtime timeouts must be positive");
  const auto operation_deadline =
      std::chrono::steady_clock::now() + operation_timeout;
  const auto existing =
      inspect(remaining_until(operation_deadline, "activation"));
  if (existing.running && existing.ready && existing.managed &&
      existing.artifact == artifact &&
      existing.workload_fingerprint == workload_fingerprint(spec))
    return existing;
  if (!existing.artifact.empty() && !existing.managed)
    throw std::runtime_error("refusing to replace unmanaged container '" +
                             container_name_ + "'");
  // Removal is intentionally idempotent. A non-existent workload is expected
  // for the first deployment and does not make activation fail.
  const auto removal =
      run_command(executable_, {"rm", "--force", container_name_},
                  remaining_until(operation_deadline, "activation"));
  if (removal.timed_out)
    require_success(removal, "activation cleanup");
  std::vector<std::string> arguments{
      "run",       "--detach",
      "--name",    container_name_,
      "--network", network_mode_name(spec.network),
      "--restart", spec.restart_policy,
      "--label",   "io.vektor.managed=true",
      "--label",   "io.vektor.workload=" + workload_fingerprint(spec)};
  for (const auto &[name, value] : spec.environment) {
    arguments.push_back("--env");
    arguments.push_back(name + "=" + value);
  }
  for (const auto &mount : spec.mounts) {
    arguments.push_back("--mount");
    auto value = "type=bind,source=" + mount.source + ",target=" + mount.target;
    if (mount.read_only)
      value += ",readonly";
    arguments.push_back(std::move(value));
  }
  for (const auto &device : spec.devices) {
    arguments.push_back("--device");
    arguments.push_back(device.host_path + ":" + device.container_path);
  }
  arguments.push_back(artifact);
  arguments.insert(arguments.end(), spec.command.begin(), spec.command.end());
  require_success(
      run_command(executable_, arguments,
                  remaining_until(operation_deadline, "activation")),
      "activation");
  const auto readiness_deadline = std::min(
      operation_deadline, std::chrono::steady_clock::now() + readiness_timeout);
  for (;;) {
    const auto observed =
        inspect(remaining_until(readiness_deadline, "readiness"));
    if (!observed.running || !observed.managed ||
        observed.artifact != artifact ||
        observed.workload_fingerprint != workload_fingerprint(spec))
      throw std::runtime_error(
          "OCI runtime observation does not match the requested workload: "
          "running=" +
          std::string(observed.running ? "true" : "false") +
          ", ready=" + (observed.ready ? "true" : "false") +
          ", managed=" + (observed.managed ? "true" : "false") +
          ", artifact='" + observed.artifact + "', readiness='" +
          observed.readiness_status + "'");
    if (observed.ready)
      return observed;
    if (observed.readiness_status == "unhealthy")
      throw std::runtime_error("OCI workload reported unhealthy");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

RuntimeObservation OciRuntimeDriver::stop() {
  return stop(std::chrono::minutes(5));
}

RuntimeObservation
OciRuntimeDriver::stop(std::chrono::milliseconds operation_timeout) {
  const auto deadline = std::chrono::steady_clock::now() + operation_timeout;
  const auto existing = inspect(remaining_until(deadline, "stop"));
  if (existing.artifact.empty())
    return {};
  if (!existing.managed)
    throw std::runtime_error("refusing to stop unmanaged container '" +
                             container_name_ + "'");
  const auto result =
      run_command(executable_, {"rm", "--force", container_name_},
                  remaining_until(deadline, "stop"));
  if (result.timed_out)
    require_success(result, "stop");
  if (result.exit_code != 0) {
    const auto observed = inspect(remaining_until(deadline, "stop"));
    if (observed.running)
      require_success(result, "stop");
  }
  return {};
}

RuntimeObservation OciRuntimeDriver::inspect() {
  return inspect(std::chrono::seconds(30));
}

RuntimeObservation
OciRuntimeDriver::inspect(std::chrono::milliseconds operation_timeout) {
  const auto deadline = std::chrono::steady_clock::now() + operation_timeout;
  const auto result =
      run_command(executable_,
                  {"inspect", "--format",
                   "{{.Config.Image}}|{{.Id}}|"
                   "{{index .Config.Labels \"io.vektor.managed\"}}|"
                   "{{.State.Running}}|"
                   "{{index .Config.Labels \"io.vektor.workload\"}}",
                   container_name_},
                  remaining_until(deadline, "inspection"));
  if (result.timed_out)
    require_success(result, "inspection");
  if (result.exit_code != 0)
    return {};
  const auto first = result.output.find('|');
  const auto second = result.output.find('|', first + 1);
  const auto third = result.output.find('|', second + 1);
  const auto fourth = result.output.find('|', third + 1);
  if (first == std::string::npos || first == 0 || second == std::string::npos ||
      third == std::string::npos || fourth == std::string::npos)
    throw std::runtime_error(
        "OCI runtime returned an invalid inspection result");
  const auto health = run_command(
      executable_,
      {"inspect", "--format", "{{.State.Health.Status}}", container_name_},
      remaining_until(deadline, "health inspection"));
  if (health.timed_out)
    require_success(health, "health inspection");
  const auto readiness =
      health.exit_code == 0 &&
              (health.output == "starting" || health.output == "healthy" ||
               health.output == "unhealthy")
          ? health.output
          : "none";
  const auto running =
      result.output.substr(third + 1, fourth - third - 1) == "true";
  return {running,
          running && (readiness == "none" || readiness == "healthy"),
          result.output.substr(0, first),
          result.output.substr(first + 1, second - first - 1),
          result.output.substr(second + 1, third - second - 1) == "true",
          result.output.substr(fourth + 1),
          readiness};
}

} // namespace vektor
