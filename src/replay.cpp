#include "vektor/replay.hpp"

#include "vektor/capture.hpp"

#include <yaml-cpp/yaml.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace vektor {
namespace {
constexpr std::size_t kMaxArguments = 64;
constexpr std::size_t kMaxArgumentLength = 1024;
constexpr std::size_t kMaxRemaps = 64;

std::string json_escape(const std::string &value) {
  std::ostringstream output;
  for (const auto character : value) {
    switch (character) {
    case '\\':
      output << "\\\\";
      break;
    case '"':
      output << "\\\"";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      output << character;
    }
  }
  return output.str();
}

std::string utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

void validate_id(const std::string &id, const char *field) {
  static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$");
  if (!std::regex_match(id, pattern))
    throw std::invalid_argument(std::string("invalid ") + field + " '" + id +
                                "'");
}

void validate_topic(const std::string &topic) {
  static const std::regex pattern("^/[A-Za-z0-9_~/]+$");
  if (topic.size() > 256 || !std::regex_match(topic, pattern))
    throw std::invalid_argument("invalid replay topic '" + topic + "'");
}

void reject_unknown(const YAML::Node &node,
                    const std::set<std::string> &allowed,
                    const std::string &field) {
  if (!node.IsMap())
    throw std::invalid_argument(field + " must be a map");
  for (const auto &entry : node) {
    const auto key = entry.first.as<std::string>();
    if (!allowed.contains(key))
      throw std::invalid_argument("unknown " + field + " field '" + key + "'");
  }
}

void validate_definition(const ReplayDefinition &definition) {
  validate_id(definition.replay_id, "replay ID");
  validate_id(definition.source_run_id, "source run ID");
  if (definition.ros_domain_id == 0 || definition.ros_domain_id > 232)
    throw std::invalid_argument(
        "replay ROS domain ID must be between 1 and 232");
  if (definition.timeout < std::chrono::seconds(1) ||
      definition.timeout > std::chrono::hours(24))
    throw std::invalid_argument(
        "replay timeout must be between 1 and 86400 seconds");
  if (definition.topic_remaps.size() > kMaxRemaps)
    throw std::invalid_argument("replay supports at most 64 topic remaps");
  for (const auto &[source, target] : definition.topic_remaps) {
    validate_topic(source);
    validate_topic(target);
  }
  if (definition.arguments.size() > kMaxArguments)
    throw std::invalid_argument(
        "simulator adapter supports at most 64 arguments");
  for (const auto &argument : definition.arguments)
    if (argument.size() > kMaxArgumentLength)
      throw std::invalid_argument("simulator argument exceeds 1024 bytes");
  if (definition.adapter == ReplayAdapter::Rosbag2 &&
      (!definition.executable.empty() || !definition.arguments.empty()))
    throw std::invalid_argument(
        "rosbag2 replay does not accept simulator executable or arguments");
  if (definition.adapter == ReplayAdapter::Simulator &&
      definition.executable.empty())
    throw std::invalid_argument("simulator replay requires an executable");
}

std::string required_scalar(const YAML::Node &node, const char *field) {
  if (!node[field] || !node[field].IsScalar())
    throw std::invalid_argument(std::string("replay definition requires '") +
                                field + "'");
  const auto value = node[field].as<std::string>();
  if (value.empty())
    throw std::invalid_argument(std::string("replay definition field '") +
                                field + "' cannot be empty");
  return value;
}

ReplayAdapter parse_adapter(const std::string &value) {
  if (value == "rosbag2")
    return ReplayAdapter::Rosbag2;
  if (value == "simulator")
    return ReplayAdapter::Simulator;
  throw std::invalid_argument("replay adapter must be rosbag2 or simulator");
}

std::string replace_all(std::string value, const std::string &placeholder,
                        const std::string &replacement) {
  std::size_t position = 0;
  while ((position = value.find(placeholder, position)) != std::string::npos) {
    value.replace(position, placeholder.size(), replacement);
    position += replacement.size();
  }
  return value;
}

std::string expand_argument(std::string argument,
                            const ReplayDefinition &definition,
                            const RunManifest &source_run,
                            const RunArtifact &bag,
                            const std::filesystem::path &output_directory) {
  argument = replace_all(argument, "${bag_path}", bag.uri);
  argument = replace_all(argument, "${source_run_id}", source_run.run_id);
  argument = replace_all(argument, "${replay_id}", definition.replay_id);
  argument = replace_all(argument, "${ros_domain_id}",
                         std::to_string(definition.ros_domain_id));
  argument = replace_all(argument, "${output_dir}", output_directory.string());
  if (argument.find("${") != std::string::npos)
    throw std::invalid_argument("unsupported replay argument placeholder");
  if (argument.size() > kMaxArgumentLength)
    throw std::invalid_argument(
        "expanded simulator argument exceeds 1024 bytes");
  return argument;
}

RunArtifact verified_bag(const RunManifest &source_run) {
  if (source_run.status != RunStatus::Completed)
    throw std::invalid_argument("replay source run must be completed");
  for (const auto &artifact : source_run.artifacts) {
    if (artifact.kind != "rosbag2")
      continue;
    const auto actual = fingerprint_run_artifact(artifact.uri, "rosbag2");
    if (actual.sha256 != artifact.sha256 ||
        actual.size_bytes != artifact.size_bytes)
      throw std::runtime_error("source rosbag2 artifact fingerprint mismatch");
    return artifact;
  }
  throw std::invalid_argument("replay source run has no rosbag2 artifact");
}

struct ProcessResult {
  int exit_code{-1};
  bool timed_out{false};
};

bool wait_for_process(pid_t pid, std::chrono::milliseconds timeout,
                      int &status) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = waitpid(pid, &status, WNOHANG);
    if (result == pid)
      return true;
    if (result < 0 && errno != EINTR)
      throw std::runtime_error("failed to wait for replay adapter: " +
                               std::string(std::strerror(errno)));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

void signal_group(pid_t pid, int signal) {
  if (kill(-pid, signal) != 0 && errno != ESRCH)
    throw std::runtime_error("failed to signal replay adapter: " +
                             std::string(std::strerror(errno)));
}

ProcessResult run_process(const std::vector<std::string> &command,
                          unsigned int ros_domain_id,
                          std::chrono::milliseconds timeout,
                          const std::filesystem::path &log_path) {
  if (command.empty())
    throw std::invalid_argument("replay command cannot be empty");
  std::filesystem::create_directories(log_path.parent_path());
  const auto pid = fork();
  if (pid < 0)
    throw std::runtime_error("failed to fork replay adapter: " +
                             std::string(std::strerror(errno)));
  if (pid == 0) {
    if (setsid() < 0)
      _exit(126);
    const int log = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (log < 0 || dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0)
      _exit(126);
    if (log != STDOUT_FILENO && log != STDERR_FILENO)
      close(log);
    const int null_input = open("/dev/null", O_RDONLY);
    if (null_input < 0 || dup2(null_input, STDIN_FILENO) < 0)
      _exit(126);
    if (null_input != STDIN_FILENO)
      close(null_input);
    const auto domain = std::to_string(ros_domain_id);
    if (setenv("ROS_DOMAIN_ID", domain.c_str(), 1) != 0 ||
        setenv("ROS_LOCALHOST_ONLY", "1", 1) != 0 ||
        setenv("RCUTILS_COLORIZED_OUTPUT", "0", 1) != 0)
      _exit(126);
    std::vector<std::string> mutable_command = command;
    std::vector<char *> argv;
    argv.reserve(mutable_command.size() + 1);
    for (auto &argument : mutable_command)
      argv.push_back(argument.data());
    argv.push_back(nullptr);
    execvp(argv.front(), argv.data());
    _exit(127);
  }

  int status = 0;
  if (wait_for_process(pid, timeout, status)) {
    if (WIFEXITED(status))
      return {WEXITSTATUS(status), false};
    if (WIFSIGNALED(status))
      return {128 + WTERMSIG(status), false};
  }

  signal_group(pid, SIGINT);
  if (!wait_for_process(pid, std::chrono::seconds(2), status)) {
    signal_group(pid, SIGTERM);
    if (!wait_for_process(pid, std::chrono::seconds(2), status)) {
      signal_group(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
    }
  }
  return {-1, true};
}

YAML::Node manifest_node(const ReplayManifest &manifest) {
  YAML::Node node;
  node["schema_version"] = manifest.schema_version;
  node["replay_id"] = manifest.replay_id;
  node["source_run_id"] = manifest.source_run_id;
  node["source_artifact"] = manifest.source_artifact;
  node["source_bag"]["kind"] = manifest.source_bag.kind;
  node["source_bag"]["uri"] = manifest.source_bag.uri;
  node["source_bag"]["sha256"] = manifest.source_bag.sha256;
  node["source_bag"]["size_bytes"] = manifest.source_bag.size_bytes;
  node["adapter"] = replay_adapter_name(manifest.adapter);
  node["adapter_version"] = manifest.adapter_version;
  node["ros_domain_id"] = manifest.ros_domain_id;
  node["localhost_only"] = manifest.localhost_only;
  node["timeout_ms"] = manifest.timeout.count();
  node["topic_remaps"] = manifest.topic_remaps;
  node["command"] = manifest.command;
  node["started_at"] = manifest.started_at;
  node["stopped_at"] = manifest.stopped_at;
  node["status"] = replay_status_name(manifest.status);
  node["exit_code"] = manifest.exit_code;
  node["log_path"] = manifest.log_path;
  return node;
}
} // namespace

const char *replay_adapter_name(ReplayAdapter adapter) {
  return adapter == ReplayAdapter::Rosbag2 ? "rosbag2" : "simulator";
}

const char *replay_status_name(ReplayStatus status) {
  if (status == ReplayStatus::Completed)
    return "completed";
  if (status == ReplayStatus::TimedOut)
    return "timed_out";
  return "failed";
}

ReplayDefinition load_replay_definition(const std::filesystem::path &path) {
  YAML::Node node;
  try {
    node = YAML::LoadFile(path.string());
  } catch (const YAML::Exception &error) {
    throw std::invalid_argument("failed to load replay definition: " +
                                std::string(error.what()));
  }
  if (!node["schema_version"] || node["schema_version"].as<unsigned int>() != 1)
    throw std::invalid_argument("replay definition requires schema_version: 1");
  reject_unknown(node,
                 {"schema_version", "replay_id", "source_run_id", "adapter",
                  "ros_domain_id", "timeout_seconds", "topic_remaps",
                  "qos_overrides", "simulator"},
                 "replay definition");
  if (!node["ros_domain_id"] || !node["ros_domain_id"].IsScalar())
    throw std::invalid_argument("replay definition requires 'ros_domain_id'");
  ReplayDefinition definition;
  definition.replay_id = required_scalar(node, "replay_id");
  definition.source_run_id = required_scalar(node, "source_run_id");
  definition.adapter = parse_adapter(required_scalar(node, "adapter"));
  definition.ros_domain_id = node["ros_domain_id"].as<unsigned int>();
  if (node["timeout_seconds"])
    definition.timeout =
        std::chrono::seconds(node["timeout_seconds"].as<unsigned int>());
  if (node["topic_remaps"])
    definition.topic_remaps =
        node["topic_remaps"].as<std::map<std::string, std::string>>();
  if (node["qos_overrides"]) {
    const auto configured =
        std::filesystem::path(required_scalar(node, "qos_overrides"));
    definition.qos_overrides = std::filesystem::absolute(
        configured.is_absolute() ? configured
                                 : path.parent_path() / configured);
    if (!std::filesystem::is_regular_file(definition.qos_overrides))
      throw std::invalid_argument("replay QoS overrides file does not exist");
  }
  if (node["simulator"]) {
    reject_unknown(node["simulator"], {"executable", "arguments"},
                   "replay simulator configuration");
    definition.executable = required_scalar(node["simulator"], "executable");
    if (node["simulator"]["arguments"])
      definition.arguments =
          node["simulator"]["arguments"].as<std::vector<std::string>>();
  }
  validate_definition(definition);
  return definition;
}

ReplayExecutor::ReplayExecutor(std::filesystem::path ros2_executable)
    : ros2_executable_(std::move(ros2_executable)) {
  if (ros2_executable_.empty())
    throw std::invalid_argument("ros2 executable cannot be empty");
}

ReplayManifest
ReplayExecutor::execute(const ReplayDefinition &definition,
                        const RunManifest &source_run,
                        const std::filesystem::path &replay_directory) const {
  validate_definition(definition);
  if (definition.source_run_id != source_run.run_id)
    throw std::invalid_argument("replay source run ID does not match manifest");
  const auto bag = verified_bag(source_run);
  const auto replay_root =
      std::filesystem::absolute(replay_directory).lexically_normal();
  std::filesystem::create_directories(replay_root);
  const auto manifest_path = replay_root / (definition.replay_id + ".yaml");
  if (std::filesystem::exists(manifest_path))
    throw std::runtime_error("replay '" + definition.replay_id +
                             "' already exists");
  const auto output_directory = replay_root / definition.replay_id;
  std::filesystem::create_directories(output_directory);
  const auto log_path = output_directory / "adapter.log";

  std::vector<std::string> command;
  if (definition.adapter == ReplayAdapter::Rosbag2) {
    command = {ros2_executable_.string(),    "bag", "play", bag.uri, "--clock",
               "--disable-keyboard-controls"};
    if (!definition.qos_overrides.empty()) {
      command.push_back("--qos-profile-overrides-path");
      command.push_back(definition.qos_overrides.string());
    }
    if (!definition.topic_remaps.empty()) {
      command.push_back("--remap");
      for (const auto &[source, target] : definition.topic_remaps)
        command.push_back(source + ":=" + target);
    }
  } else {
    command.push_back(definition.executable.string());
    for (const auto &argument : definition.arguments)
      command.push_back(expand_argument(argument, definition, source_run, bag,
                                        output_directory));
  }

  ReplayManifest manifest;
  manifest.replay_id = definition.replay_id;
  manifest.source_run_id = source_run.run_id;
  manifest.source_artifact = source_run.artifact;
  manifest.source_bag = bag;
  manifest.adapter = definition.adapter;
  manifest.ros_domain_id = definition.ros_domain_id;
  manifest.timeout = definition.timeout;
  manifest.topic_remaps = definition.topic_remaps;
  manifest.command = command;
  manifest.started_at = utc_timestamp();
  manifest.log_path = std::filesystem::absolute(log_path).string();
  const auto result = run_process(command, definition.ros_domain_id,
                                  definition.timeout, log_path);
  manifest.stopped_at = utc_timestamp();
  manifest.exit_code = result.exit_code;
  manifest.status = result.timed_out        ? ReplayStatus::TimedOut
                    : result.exit_code == 0 ? ReplayStatus::Completed
                                            : ReplayStatus::Failed;
  std::ofstream output(manifest_path, std::ios::trunc);
  if (!output)
    throw std::runtime_error("failed to persist replay manifest");
  output << manifest_node(manifest);
  if (!output)
    throw std::runtime_error("failed to persist replay manifest");
  return manifest;
}

std::string replay_manifest_to_json(const ReplayManifest &manifest) {
  std::ostringstream output;
  output << "{\"schema_version\":" << manifest.schema_version
         << ",\"replay_id\":\"" << json_escape(manifest.replay_id)
         << "\",\"source_run_id\":\"" << json_escape(manifest.source_run_id)
         << "\",\"source_artifact\":\"" << json_escape(manifest.source_artifact)
         << "\",\"source_bag\":{\"kind\":\""
         << json_escape(manifest.source_bag.kind) << "\",\"uri\":\""
         << json_escape(manifest.source_bag.uri) << "\",\"sha256\":\""
         << json_escape(manifest.source_bag.sha256)
         << "\",\"size_bytes\":" << manifest.source_bag.size_bytes
         << "},\"adapter\":\"" << replay_adapter_name(manifest.adapter)
         << "\",\"adapter_version\":" << manifest.adapter_version
         << ",\"ros_domain_id\":" << manifest.ros_domain_id
         << ",\"localhost_only\":"
         << (manifest.localhost_only ? "true" : "false")
         << ",\"timeout_ms\":" << manifest.timeout.count()
         << ",\"topic_remaps\":{";
  std::size_t index = 0;
  for (const auto &[source, target] : manifest.topic_remaps) {
    if (index++ != 0)
      output << ',';
    output << '"' << json_escape(source) << "\":\"" << json_escape(target)
           << '"';
  }
  output << "},\"command\":[";
  for (index = 0; index < manifest.command.size(); ++index) {
    if (index != 0)
      output << ',';
    output << '"' << json_escape(manifest.command[index]) << '"';
  }
  output << "],\"started_at\":\"" << json_escape(manifest.started_at)
         << "\",\"stopped_at\":\"" << json_escape(manifest.stopped_at)
         << "\",\"status\":\"" << replay_status_name(manifest.status)
         << "\",\"exit_code\":" << manifest.exit_code << ",\"log_path\":\""
         << json_escape(manifest.log_path) << "\"}";
  return output.str();
}

void print_replay_manifest(const ReplayManifest &manifest,
                           std::ostream &output) {
  output << "replay: " << manifest.replay_id << '\n'
         << "source run: " << manifest.source_run_id << '\n'
         << "adapter: " << replay_adapter_name(manifest.adapter) << '\n'
         << "ROS domain: " << manifest.ros_domain_id << '\n'
         << "localhost only: " << (manifest.localhost_only ? "yes" : "no")
         << '\n'
         << "status: " << replay_status_name(manifest.status) << '\n'
         << "exit code: " << manifest.exit_code << '\n'
         << "log: " << manifest.log_path << '\n';
}

} // namespace vektor
