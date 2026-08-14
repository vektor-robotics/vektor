#include "vektor/deployment.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace vektor {
namespace {
std::string utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

bool valid_deployment_id(const std::string &value) {
  return !value.empty() &&
         std::isalnum(static_cast<unsigned char>(value.front())) &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isalnum(character) || character == '-' ||
                  character == '_' || character == '.';
         });
}

DeploymentPhase parse_phase(const std::string &value) {
  if (value == "idle")
    return DeploymentPhase::Idle;
  if (value == "staged")
    return DeploymentPhase::Staged;
  if (value == "active")
    return DeploymentPhase::Active;
  if (value == "rolled_back")
    return DeploymentPhase::RolledBack;
  if (value == "failed")
    return DeploymentPhase::Failed;
  throw std::runtime_error("unknown persisted deployment phase '" + value +
                           "'");
}

vektor::agent::v1::DeploymentPhase proto_phase(DeploymentPhase phase) {
  switch (phase) {
  case DeploymentPhase::Idle:
    return vektor::agent::v1::DEPLOYMENT_PHASE_IDLE;
  case DeploymentPhase::Staged:
    return vektor::agent::v1::DEPLOYMENT_PHASE_STAGED;
  case DeploymentPhase::Active:
    return vektor::agent::v1::DEPLOYMENT_PHASE_ACTIVE;
  case DeploymentPhase::RolledBack:
    return vektor::agent::v1::DEPLOYMENT_PHASE_ROLLED_BACK;
  case DeploymentPhase::Failed:
    return vektor::agent::v1::DEPLOYMENT_PHASE_FAILED;
  }
  return vektor::agent::v1::DEPLOYMENT_PHASE_UNSPECIFIED;
}

WorkloadSpec load_workload(const YAML::Node &node) {
  WorkloadSpec spec;
  if (!node)
    return spec;
  spec.network = parse_network_mode(node["network"].as<std::string>());
  spec.restart_policy = node["restart_policy"].as<std::string>();
  for (const auto &entry : node["environment"])
    spec.environment.emplace(entry.first.as<std::string>(),
                             entry.second.as<std::string>());
  for (const auto &item : node["mounts"])
    spec.mounts.push_back({item["source"].as<std::string>(),
                           item["target"].as<std::string>(),
                           item["read_only"].as<bool>()});
  for (const auto &item : node["devices"])
    spec.devices.push_back({item["host_path"].as<std::string>(),
                            item["container_path"].as<std::string>()});
  for (const auto &argument : node["command"])
    spec.command.push_back(argument.as<std::string>());
  validate_workload_spec(spec);
  return spec;
}

void emit_workload(YAML::Emitter &output, const char *key,
                   const WorkloadSpec &spec) {
  output << YAML::Key << key << YAML::Value << YAML::BeginMap << YAML::Key
         << "network" << YAML::Value << network_mode_name(spec.network)
         << YAML::Key << "restart_policy" << YAML::Value << spec.restart_policy
         << YAML::Key << "environment" << YAML::Value << YAML::BeginMap;
  for (const auto &[name, value] : spec.environment)
    output << YAML::Key << name << YAML::Value << value;
  output << YAML::EndMap << YAML::Key << "mounts" << YAML::Value
         << YAML::BeginSeq;
  for (const auto &mount : spec.mounts)
    output << YAML::BeginMap << YAML::Key << "source" << YAML::Value
           << mount.source << YAML::Key << "target" << YAML::Value
           << mount.target << YAML::Key << "read_only" << YAML::Value
           << mount.read_only << YAML::EndMap;
  output << YAML::EndSeq << YAML::Key << "devices" << YAML::Value
         << YAML::BeginSeq;
  for (const auto &device : spec.devices)
    output << YAML::BeginMap << YAML::Key << "host_path" << YAML::Value
           << device.host_path << YAML::Key << "container_path" << YAML::Value
           << device.container_path << YAML::EndMap;
  output << YAML::EndSeq << YAML::Key << "command" << YAML::Value
         << YAML::BeginSeq;
  for (const auto &argument : spec.command)
    output << argument;
  output << YAML::EndSeq << YAML::EndMap;
}

vektor::agent::v1::RuntimeNetworkMode proto_network(NetworkMode mode) {
  switch (mode) {
  case NetworkMode::Host:
    return vektor::agent::v1::RUNTIME_NETWORK_MODE_HOST;
  case NetworkMode::Bridge:
    return vektor::agent::v1::RUNTIME_NETWORK_MODE_BRIDGE;
  case NetworkMode::None:
    return vektor::agent::v1::RUNTIME_NETWORK_MODE_NONE;
  }
  return vektor::agent::v1::RUNTIME_NETWORK_MODE_UNSPECIFIED;
}
} // namespace

const char *deployment_phase_name(DeploymentPhase phase) {
  switch (phase) {
  case DeploymentPhase::Idle:
    return "idle";
  case DeploymentPhase::Staged:
    return "staged";
  case DeploymentPhase::Active:
    return "active";
  case DeploymentPhase::RolledBack:
    return "rolled_back";
  case DeploymentPhase::Failed:
    return "failed";
  }
  return "unknown";
}

bool is_valid_deployment_id(const std::string &value) {
  return valid_deployment_id(value);
}

bool is_pinned_oci_artifact(const std::string &artifact) {
  static const std::regex pattern(
      R"(^[A-Za-z0-9][A-Za-z0-9._:-]*(/[A-Za-z0-9][A-Za-z0-9._-]*)*@sha256:[0-9a-f]{64}$)");
  return std::regex_match(artifact, pattern);
}

AgentDeploymentState::AgentDeploymentState(
    std::filesystem::path state_path, std::shared_ptr<RuntimeDriver> runtime)
    : state_path_(std::move(state_path)), runtime_(std::move(runtime)) {
  if (state_path_.empty())
    throw std::invalid_argument("deployment state path cannot be empty");
  if (!runtime_)
    throw std::invalid_argument("deployment runtime driver is required");
  if (runtime_->interface_version() != 1)
    throw std::invalid_argument("unsupported runtime driver interface version");
  load();
}

void AgentDeploymentState::load() {
  if (!std::filesystem::exists(state_path_))
    return;
  YAML::Node root;
  try {
    root = YAML::LoadFile(state_path_.string());
    if (!root.IsMap())
      throw std::runtime_error("deployment state must be a mapping");
    const auto schema_version = root["schema_version"].as<unsigned int>();
    if (schema_version < 1 || schema_version > 3)
      throw std::runtime_error("unsupported deployment state schema");
    record_.deployment_id = root["deployment_id"].as<std::string>();
    record_.artifact = root["artifact"].as<std::string>();
    record_.previous_artifact = root["previous_artifact"].as<std::string>();
    if (schema_version >= 3) {
      record_.workload = load_workload(root["workload"]);
      record_.previous_workload = load_workload(root["previous_workload"]);
      record_.observed_workload_fingerprint =
          root["observed_workload_fingerprint"].as<std::string>("");
    }
    if (schema_version >= 2) {
      record_.observed_artifact = root["observed_artifact"].as<std::string>();
      record_.runtime_id = root["runtime_id"].as<std::string>();
      record_.runtime_running = root["runtime_running"].as<bool>();
      record_.runtime_managed = root["runtime_managed"].as<bool>(false);
      record_.drift_detected = root["drift_detected"].as<bool>();
    }
    record_.phase = parse_phase(root["phase"].as<std::string>());
    record_.message = root["message"].as<std::string>();
    record_.updated_at = root["updated_at"].as<std::string>();
  } catch (const std::exception &error) {
    throw std::runtime_error("failed to load deployment state '" +
                             state_path_.string() + "': " + error.what());
  }
}

void AgentDeploymentState::persist_locked() const {
  const auto parent = state_path_.parent_path();
  if (!parent.empty())
    std::filesystem::create_directories(parent);
  YAML::Emitter output;
  output << YAML::BeginMap << YAML::Key << "schema_version" << YAML::Value << 3
         << YAML::Key << "deployment_id" << YAML::Value << record_.deployment_id
         << YAML::Key << "artifact" << YAML::Value << record_.artifact
         << YAML::Key << "previous_artifact" << YAML::Value
         << record_.previous_artifact;
  emit_workload(output, "workload", record_.workload);
  emit_workload(output, "previous_workload", record_.previous_workload);
  output << YAML::Key << "observed_artifact" << YAML::Value
         << record_.observed_artifact << YAML::Key
         << "observed_workload_fingerprint" << YAML::Value
         << record_.observed_workload_fingerprint << YAML::Key << "runtime_id"
         << YAML::Value << record_.runtime_id << YAML::Key << "runtime_running"
         << YAML::Value << record_.runtime_running << YAML::Key
         << "runtime_managed" << YAML::Value << record_.runtime_managed
         << YAML::Key << "drift_detected" << YAML::Value
         << record_.drift_detected << YAML::Key << "phase" << YAML::Value
         << deployment_phase_name(record_.phase) << YAML::Key << "message"
         << YAML::Value << record_.message << YAML::Key << "updated_at"
         << YAML::Value << record_.updated_at << YAML::EndMap;
  const auto temporary = state_path_.string() + ".tmp";
  {
    std::ofstream file(temporary, std::ios::trunc);
    if (!file || !(file << output.c_str() << '\n'))
      throw std::runtime_error("failed to write deployment state '" +
                               temporary + "'");
  }
  std::error_code error;
  std::filesystem::rename(temporary, state_path_, error);
  if (error) {
    std::filesystem::remove(state_path_, error);
    error.clear();
    std::filesystem::rename(temporary, state_path_, error);
  }
  if (error)
    throw std::runtime_error("failed to commit deployment state '" +
                             state_path_.string() + "': " + error.message());
}

DeploymentRecord AgentDeploymentState::prepare(const std::string &deployment_id,
                                               const std::string &artifact,
                                               WorkloadSpec workload) {
  if (!is_valid_deployment_id(deployment_id))
    throw std::invalid_argument("invalid deployment ID");
  if (!is_pinned_oci_artifact(artifact))
    throw std::invalid_argument(
        "artifact must be an OCI reference pinned by sha256 digest");
  validate_workload_spec(workload);
  std::lock_guard lock(mutex_);
  if (record_.deployment_id == deployment_id && record_.artifact == artifact &&
      record_.workload == workload &&
      (record_.phase == DeploymentPhase::Staged ||
       record_.phase == DeploymentPhase::Active))
    return record_;
  if (record_.phase == DeploymentPhase::Staged)
    throw std::runtime_error("another deployment is already staged");
  const bool current_is_rollback_target =
      record_.phase == DeploymentPhase::Active ||
      record_.phase == DeploymentPhase::RolledBack;
  const auto previous =
      current_is_rollback_target ? record_.artifact : record_.previous_artifact;
  const auto previous_workload =
      current_is_rollback_target ? record_.workload : record_.previous_workload;
  try {
    runtime_->prepare(artifact);
  } catch (const std::exception &error) {
    record_.deployment_id = deployment_id;
    record_.artifact = artifact;
    record_.previous_artifact = previous;
    record_.workload = workload;
    record_.previous_workload = previous_workload;
    record_failure_locked(error.what());
    persist_locked();
    throw;
  }
  record_.deployment_id = deployment_id;
  record_.artifact = artifact;
  record_.previous_artifact = previous;
  record_.workload = std::move(workload);
  record_.previous_workload = previous_workload;
  record_.phase = DeploymentPhase::Staged;
  record_.message = "artifact prepared; awaiting activation";
  record_.updated_at = utc_timestamp();
  record_.drift_detected = false;
  persist_locked();
  return record_;
}

DeploymentRecord
AgentDeploymentState::activate(const std::string &deployment_id) {
  std::lock_guard lock(mutex_);
  if (record_.deployment_id != deployment_id ||
      (record_.phase != DeploymentPhase::Staged &&
       record_.phase != DeploymentPhase::Active))
    throw std::runtime_error("deployment is not staged");
  try {
    const auto observed =
        runtime_->activate(record_.artifact, record_.workload);
    record_.observed_artifact = observed.artifact;
    record_.observed_workload_fingerprint = observed.workload_fingerprint;
    record_.runtime_id = observed.runtime_id;
    record_.runtime_running = observed.running;
    record_.runtime_managed = observed.managed;
    if (!observed.running || !observed.managed ||
        observed.artifact != record_.artifact ||
        observed.workload_fingerprint != workload_fingerprint(record_.workload))
      throw std::runtime_error(
          "observed runtime does not match desired artifact");
    record_.drift_detected = false;
    record_.phase = DeploymentPhase::Active;
    record_.message = "desired artifact is running and observed";
    record_.updated_at = utc_timestamp();
    persist_locked();
    return record_;
  } catch (const std::exception &error) {
    record_failure_locked(std::string("activation failed: ") + error.what());
    persist_locked();
    throw;
  }
}

DeploymentRecord
AgentDeploymentState::rollback(const std::string &deployment_id) {
  std::lock_guard lock(mutex_);
  if (record_.deployment_id != deployment_id ||
      (record_.phase != DeploymentPhase::Staged &&
       record_.phase != DeploymentPhase::Active &&
       record_.phase != DeploymentPhase::Failed &&
       record_.phase != DeploymentPhase::RolledBack))
    throw std::runtime_error("deployment cannot be rolled back");
  const auto target = record_.previous_artifact;
  const auto target_workload = record_.previous_workload;
  record_.artifact = target;
  record_.workload = target_workload;
  try {
    const auto observed = target.empty()
                              ? runtime_->stop()
                              : runtime_->activate(target, target_workload);
    record_.observed_artifact = observed.artifact;
    record_.observed_workload_fingerprint = observed.workload_fingerprint;
    record_.runtime_id = observed.runtime_id;
    record_.runtime_running = observed.running;
    record_.runtime_managed = observed.managed;
    const bool matches = target.empty()
                             ? !observed.running
                             : observed.running && observed.managed &&
                                   observed.artifact == target &&
                                   observed.workload_fingerprint ==
                                       workload_fingerprint(target_workload);
    if (!matches)
      throw std::runtime_error(
          "observed runtime does not match rollback target");
    record_.drift_detected = false;
    record_.phase = DeploymentPhase::RolledBack;
    record_.message = target.empty()
                          ? "managed workload stopped"
                          : "previous artifact restored and observed";
    record_.updated_at = utc_timestamp();
    persist_locked();
    return record_;
  } catch (const std::exception &error) {
    record_failure_locked(std::string("rollback failed: ") + error.what());
    persist_locked();
    throw;
  }
}

void AgentDeploymentState::record_failure_locked(const std::string &message) {
  record_.phase = DeploymentPhase::Failed;
  record_.message = message;
  record_.updated_at = utc_timestamp();
}

bool AgentDeploymentState::observe_locked() {
  const auto previous_artifact = record_.observed_artifact;
  const auto previous_workload_fingerprint =
      record_.observed_workload_fingerprint;
  const auto previous_runtime_id = record_.runtime_id;
  const auto previous_running = record_.runtime_running;
  const auto previous_managed = record_.runtime_managed;
  const auto previous_drift = record_.drift_detected;
  const auto previous_phase = record_.phase;
  const auto observed = runtime_->inspect();
  record_.observed_artifact = observed.artifact;
  record_.observed_workload_fingerprint = observed.workload_fingerprint;
  record_.runtime_id = observed.runtime_id;
  record_.runtime_running = observed.running;
  record_.runtime_managed = observed.managed;
  if (record_.phase != DeploymentPhase::Active &&
      record_.phase != DeploymentPhase::RolledBack)
    return previous_artifact != record_.observed_artifact ||
           previous_workload_fingerprint !=
               record_.observed_workload_fingerprint ||
           previous_runtime_id != record_.runtime_id ||
           previous_running != record_.runtime_running ||
           previous_managed != record_.runtime_managed;
  const bool matches = record_.artifact.empty()
                           ? !observed.running
                           : observed.running && observed.managed &&
                                 observed.artifact == record_.artifact &&
                                 observed.workload_fingerprint ==
                                     workload_fingerprint(record_.workload);
  record_.drift_detected = !matches;
  if (!matches)
    record_failure_locked("runtime drift detected: observed workload does not "
                          "match desired artifact");
  return previous_artifact != record_.observed_artifact ||
         previous_workload_fingerprint !=
             record_.observed_workload_fingerprint ||
         previous_runtime_id != record_.runtime_id ||
         previous_running != record_.runtime_running ||
         previous_managed != record_.runtime_managed ||
         previous_drift != record_.drift_detected ||
         previous_phase != record_.phase;
}

DeploymentRecord AgentDeploymentState::refresh_observed() {
  std::lock_guard lock(mutex_);
  try {
    if (observe_locked())
      persist_locked();
  } catch (const std::exception &error) {
    record_.drift_detected = true;
    record_failure_locked(std::string("runtime inspection failed: ") +
                          error.what());
    persist_locked();
  }
  return record_;
}

DeploymentRecord AgentDeploymentState::current() const {
  std::lock_guard lock(mutex_);
  return record_;
}

vektor::agent::v1::DeploymentRecord to_proto(const DeploymentRecord &record) {
  vektor::agent::v1::DeploymentRecord result;
  result.set_schema_version(3);
  result.set_deployment_id(record.deployment_id);
  result.set_artifact(record.artifact);
  result.set_previous_artifact(record.previous_artifact);
  result.set_phase(proto_phase(record.phase));
  result.set_message(record.message);
  result.set_updated_at(record.updated_at);
  result.set_observed_artifact(record.observed_artifact);
  result.set_observed_workload_fingerprint(
      record.observed_workload_fingerprint);
  result.set_runtime_id(record.runtime_id);
  result.set_runtime_running(record.runtime_running);
  result.set_runtime_managed(record.runtime_managed);
  result.set_drift_detected(record.drift_detected);
  *result.mutable_workload() = to_proto(record.workload);
  *result.mutable_previous_workload() = to_proto(record.previous_workload);
  result.set_workload_fingerprint(workload_fingerprint(record.workload));
  return result;
}

vektor::agent::v1::RuntimeWorkloadSpec to_proto(const WorkloadSpec &spec) {
  validate_workload_spec(spec);
  vektor::agent::v1::RuntimeWorkloadSpec result;
  result.set_network(proto_network(spec.network));
  result.set_restart_policy(spec.restart_policy);
  for (const auto &[name, value] : spec.environment) {
    auto *item = result.add_environment();
    item->set_name(name);
    item->set_value(value);
  }
  for (const auto &mount : spec.mounts) {
    auto *item = result.add_mounts();
    item->set_source(mount.source);
    item->set_target(mount.target);
    item->set_read_only(mount.read_only);
  }
  for (const auto &device : spec.devices) {
    auto *item = result.add_devices();
    item->set_host_path(device.host_path);
    item->set_container_path(device.container_path);
  }
  for (const auto &argument : spec.command)
    result.add_command(argument);
  return result;
}

WorkloadSpec from_proto(const vektor::agent::v1::RuntimeWorkloadSpec &proto,
                        bool use_defaults_for_unspecified) {
  WorkloadSpec spec;
  switch (proto.network()) {
  case vektor::agent::v1::RUNTIME_NETWORK_MODE_HOST:
    spec.network = NetworkMode::Host;
    break;
  case vektor::agent::v1::RUNTIME_NETWORK_MODE_BRIDGE:
    spec.network = NetworkMode::Bridge;
    break;
  case vektor::agent::v1::RUNTIME_NETWORK_MODE_NONE:
    spec.network = NetworkMode::None;
    break;
  case vektor::agent::v1::RUNTIME_NETWORK_MODE_UNSPECIFIED:
    if (!use_defaults_for_unspecified)
      throw std::invalid_argument("runtime network mode is required");
    break;
  default:
    throw std::invalid_argument("unsupported runtime network mode");
  }
  if (!proto.restart_policy().empty())
    spec.restart_policy = proto.restart_policy();
  else if (!use_defaults_for_unspecified)
    throw std::invalid_argument("runtime restart policy is required");
  for (const auto &item : proto.environment()) {
    if (!spec.environment.emplace(item.name(), item.value()).second)
      throw std::invalid_argument("duplicate environment variable '" +
                                  item.name() + "'");
  }
  for (const auto &item : proto.mounts())
    spec.mounts.push_back({item.source(), item.target(), item.read_only()});
  for (const auto &item : proto.devices())
    spec.devices.push_back({item.host_path(), item.container_path()});
  for (const auto &argument : proto.command())
    spec.command.push_back(argument);
  validate_workload_spec(spec);
  return spec;
}

} // namespace vektor
