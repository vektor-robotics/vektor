#include "vektor/deployment.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
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

ReconciliationOperation parse_operation(const std::string &value) {
  if (value == "none")
    return ReconciliationOperation::None;
  if (value == "verifying")
    return ReconciliationOperation::Verifying;
  if (value == "preparing")
    return ReconciliationOperation::Preparing;
  if (value == "activating")
    return ReconciliationOperation::Activating;
  if (value == "rolling_back")
    return ReconciliationOperation::RollingBack;
  throw std::runtime_error("unknown persisted reconciliation operation '" +
                           value + "'");
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

vektor::agent::v1::ReconciliationOperation
proto_operation(ReconciliationOperation operation) {
  switch (operation) {
  case ReconciliationOperation::None:
    return vektor::agent::v1::RECONCILIATION_OPERATION_NONE;
  case ReconciliationOperation::Verifying:
    return vektor::agent::v1::RECONCILIATION_OPERATION_VERIFYING;
  case ReconciliationOperation::Preparing:
    return vektor::agent::v1::RECONCILIATION_OPERATION_PREPARING;
  case ReconciliationOperation::Activating:
    return vektor::agent::v1::RECONCILIATION_OPERATION_ACTIVATING;
  case ReconciliationOperation::RollingBack:
    return vektor::agent::v1::RECONCILIATION_OPERATION_ROLLING_BACK;
  }
  return vektor::agent::v1::RECONCILIATION_OPERATION_UNSPECIFIED;
}

void apply_observation(DeploymentRecord &record,
                       const RuntimeObservation &observed) {
  record.observed_artifact = observed.artifact;
  record.observed_workload_fingerprint = observed.workload_fingerprint;
  record.runtime_id = observed.runtime_id;
  record.runtime_running = observed.running;
  record.runtime_ready = observed.ready;
  record.runtime_managed = observed.managed;
  record.runtime_readiness_status = observed.readiness_status;
}

bool matches_desired(const DeploymentRecord &record,
                     const RuntimeObservation &observed) {
  return record.artifact.empty()
             ? !observed.running
             : observed.running && observed.ready && observed.managed &&
                   observed.artifact == record.artifact &&
                   observed.workload_fingerprint ==
                       workload_fingerprint(record.workload);
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

ArtifactVerification load_verification(const YAML::Node &node) {
  ArtifactVerification verification;
  if (!node)
    return verification;
  verification.verified = node["verified"].as<bool>(false);
  verification.method = node["method"].as<std::string>("");
  verification.signer = node["signer"].as<std::string>("");
  verification.issuer = node["issuer"].as<std::string>("");
  verification.verified_at = node["verified_at"].as<std::string>("");
  return verification;
}

void emit_verification(YAML::Emitter &output, const char *key,
                       const ArtifactVerification &verification) {
  output << YAML::Key << key << YAML::Value << YAML::BeginMap << YAML::Key
         << "verified" << YAML::Value << verification.verified << YAML::Key
         << "method" << YAML::Value << verification.method << YAML::Key
         << "signer" << YAML::Value << verification.signer << YAML::Key
         << "issuer" << YAML::Value << verification.issuer << YAML::Key
         << "verified_at" << YAML::Value << verification.verified_at
         << YAML::EndMap;
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

const char *reconciliation_operation_name(ReconciliationOperation operation) {
  switch (operation) {
  case ReconciliationOperation::None:
    return "none";
  case ReconciliationOperation::Verifying:
    return "verifying";
  case ReconciliationOperation::Preparing:
    return "preparing";
  case ReconciliationOperation::Activating:
    return "activating";
  case ReconciliationOperation::RollingBack:
    return "rolling_back";
  }
  return "unknown";
}

bool is_valid_deployment_id(const std::string &value) {
  return valid_deployment_id(value);
}

bool is_valid_workload_id(const std::string &value) {
  return valid_deployment_id(value);
}

std::filesystem::path workload_state_path(const std::filesystem::path &base,
                                          const std::string &workload_id) {
  if (base.empty())
    throw std::invalid_argument("deployment state path cannot be empty");
  if (!is_valid_workload_id(workload_id))
    throw std::invalid_argument("invalid workload ID");
  if (workload_id == "default")
    return base;
  const auto extension = base.extension();
  const auto name = base.stem().string() + "." + workload_id + extension.string();
  return base.parent_path() / name;
}

bool is_pinned_oci_artifact(const std::string &artifact) {
  static const std::regex pattern(
      R"(^[A-Za-z0-9][A-Za-z0-9._:-]*(/[A-Za-z0-9][A-Za-z0-9._-]*)*@sha256:[0-9a-f]{64}$)");
  return std::regex_match(artifact, pattern);
}

AgentDeploymentState::AgentDeploymentState(
    std::filesystem::path state_path, std::shared_ptr<RuntimeDriver> runtime,
    std::shared_ptr<ArtifactVerifier> verifier,
    std::shared_ptr<AuditSink> audit)
    : state_path_(std::move(state_path)), runtime_(std::move(runtime)),
      verifier_(std::move(verifier)), audit_(std::move(audit)) {
  if (state_path_.empty())
    throw std::invalid_argument("deployment state path cannot be empty");
  if (!runtime_)
    throw std::invalid_argument("deployment runtime driver is required");
  if (runtime_->interface_version() != 1)
    throw std::invalid_argument("unsupported runtime driver interface version");
  if (verifier_ && verifier_->interface_version() != 1)
    throw std::invalid_argument(
        "unsupported artifact verifier interface version");
  if (audit_ && audit_->interface_version() != 1)
    throw std::invalid_argument("unsupported audit sink interface version");
  load();
}

WorkloadDeploymentStates::WorkloadDeploymentStates(
    std::filesystem::path state_path, RuntimeFactory runtime_factory,
    std::shared_ptr<ArtifactVerifier> verifier, std::shared_ptr<AuditSink> audit)
    : state_path_(std::move(state_path)),
      manifest_path_(state_path_.string() + ".workloads"),
      runtime_factory_(std::move(runtime_factory)), verifier_(std::move(verifier)),
      audit_(std::move(audit)) {
  if (state_path_.empty())
    throw std::invalid_argument("deployment state path cannot be empty");
  if (!runtime_factory_)
    throw std::invalid_argument("workload runtime factory is required");
  load_manifest();
}

void WorkloadDeploymentStates::load_manifest() {
  if (!std::filesystem::exists(manifest_path_))
    return;
  YAML::Node root;
  try {
    root = YAML::LoadFile(manifest_path_.string());
    if (!root.IsMap() || root["schema_version"].as<unsigned int>() != 1 ||
        !root["workloads"].IsSequence())
      throw std::runtime_error("unsupported workload state manifest");
    for (const auto &item : root["workloads"])
      for_workload(item.as<std::string>());
  } catch (const std::exception &error) {
    throw std::runtime_error("failed to load workload state manifest '" +
                             manifest_path_.string() + "': " + error.what());
  }
}

void WorkloadDeploymentStates::persist_manifest_locked() const {
  const auto parent = manifest_path_.parent_path();
  if (!parent.empty())
    std::filesystem::create_directories(parent);
  YAML::Emitter output;
  output << YAML::BeginMap << YAML::Key << "schema_version" << YAML::Value << 1
         << YAML::Key << "workloads" << YAML::Value << YAML::BeginSeq;
  for (const auto &[id, state] : states_)
    output << id;
  output << YAML::EndSeq << YAML::EndMap;
  const auto temporary = manifest_path_.string() + ".tmp";
  std::ofstream file(temporary, std::ios::trunc);
  if (!file || !(file << output.c_str() << '\n'))
    throw std::runtime_error("failed to write workload state manifest");
  file.close();
  std::error_code error;
  std::filesystem::rename(temporary, manifest_path_, error);
  if (error) {
    std::filesystem::remove(manifest_path_, error);
    error.clear();
    std::filesystem::rename(temporary, manifest_path_, error);
  }
  if (error)
    throw std::runtime_error("failed to commit workload state manifest: " +
                             error.message());
}

AgentDeploymentState &
WorkloadDeploymentStates::for_workload(const std::string &workload_id) {
  if (!is_valid_workload_id(workload_id))
    throw std::invalid_argument("invalid workload ID");
  std::lock_guard lock(mutex_);
  if (const auto found = states_.find(workload_id); found != states_.end())
    return *found->second;
  auto runtime = runtime_factory_(workload_id);
  if (!runtime)
    throw std::runtime_error("workload runtime factory returned null");
  auto state = std::make_unique<AgentDeploymentState>(
      workload_state_path(state_path_, workload_id), std::move(runtime),
      verifier_, audit_);
  const auto [inserted, added] = states_.emplace(workload_id, std::move(state));
  try {
    persist_manifest_locked();
  } catch (...) {
    states_.erase(inserted);
    throw;
  }
  return *inserted->second;
}

std::vector<std::string> WorkloadDeploymentStates::workload_ids() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> result;
  result.reserve(states_.size());
  for (const auto &[id, state] : states_)
    result.push_back(id);
  return result;
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
    if (schema_version < 1 || schema_version > 5)
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
    if (schema_version >= 4) {
      record_.runtime_ready = root["runtime_ready"].as<bool>(false);
      record_.runtime_readiness_status =
          root["runtime_readiness_status"].as<std::string>("");
      record_.operation = parse_operation(
          root["reconciliation_operation"].as<std::string>("none"));
      record_.operation_started_at =
          root["operation_started_at"].as<std::string>("");
      record_.operation_attempt =
          root["operation_attempt"].as<std::uint64_t>(0);
    }
    if (schema_version >= 5) {
      record_.verification = load_verification(root["verification"]);
      record_.previous_verification =
          load_verification(root["previous_verification"]);
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
  output << YAML::BeginMap << YAML::Key << "schema_version" << YAML::Value << 5
         << YAML::Key << "deployment_id" << YAML::Value << record_.deployment_id
         << YAML::Key << "artifact" << YAML::Value << record_.artifact
         << YAML::Key << "previous_artifact" << YAML::Value
         << record_.previous_artifact;
  emit_workload(output, "workload", record_.workload);
  emit_workload(output, "previous_workload", record_.previous_workload);
  emit_verification(output, "verification", record_.verification);
  emit_verification(output, "previous_verification",
                    record_.previous_verification);
  output << YAML::Key << "observed_artifact" << YAML::Value
         << record_.observed_artifact << YAML::Key
         << "observed_workload_fingerprint" << YAML::Value
         << record_.observed_workload_fingerprint << YAML::Key << "runtime_id"
         << YAML::Value << record_.runtime_id << YAML::Key << "runtime_running"
         << YAML::Value << record_.runtime_running << YAML::Key
         << "runtime_ready" << YAML::Value << record_.runtime_ready << YAML::Key
         << "runtime_readiness_status" << YAML::Value
         << record_.runtime_readiness_status << YAML::Key << "runtime_managed"
         << YAML::Value << record_.runtime_managed << YAML::Key
         << "drift_detected" << YAML::Value << record_.drift_detected
         << YAML::Key << "phase" << YAML::Value
         << deployment_phase_name(record_.phase) << YAML::Key
         << "reconciliation_operation" << YAML::Value
         << reconciliation_operation_name(record_.operation) << YAML::Key
         << "operation_started_at" << YAML::Value
         << record_.operation_started_at << YAML::Key << "operation_attempt"
         << YAML::Value << record_.operation_attempt << YAML::Key << "message"
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

DeploymentRecord AgentDeploymentState::prepare(
    const std::string &deployment_id, const std::string &artifact,
    WorkloadSpec workload, std::chrono::milliseconds operation_timeout,
    const std::string &actor) {
  if (!is_valid_deployment_id(deployment_id))
    throw std::invalid_argument("invalid deployment ID");
  if (!is_pinned_oci_artifact(artifact))
    throw std::invalid_argument(
        "artifact must be an OCI reference pinned by sha256 digest");
  validate_workload_spec(workload);
  std::unique_lock lock(mutex_);
  if (record_.deployment_id == deployment_id && record_.artifact == artifact &&
      record_.workload == workload &&
      (record_.phase == DeploymentPhase::Staged ||
       record_.phase == DeploymentPhase::Active) &&
      (!verifier_ || record_.verification.verified)) {
    audit_locked(actor, "deployment.prepare", "unchanged",
                 "deployment already prepared");
    return record_;
  }
  if (record_.phase == DeploymentPhase::Staged)
    throw std::runtime_error("another deployment is already staged");
  if (record_.operation != ReconciliationOperation::None ||
      operation_in_progress_)
    throw std::runtime_error("another reconciliation operation is in progress");
  const bool current_is_rollback_target =
      record_.phase == DeploymentPhase::Active ||
      record_.phase == DeploymentPhase::RolledBack;
  const auto previous =
      current_is_rollback_target ? record_.artifact : record_.previous_artifact;
  const auto previous_workload =
      current_is_rollback_target ? record_.workload : record_.previous_workload;
  const auto previous_verification = current_is_rollback_target
                                         ? record_.verification
                                         : record_.previous_verification;
  const auto original_record = record_;
  record_.deployment_id = deployment_id;
  record_.artifact = artifact;
  record_.previous_artifact = previous;
  record_.workload = std::move(workload);
  record_.previous_workload = previous_workload;
  record_.verification = {};
  record_.previous_verification = previous_verification;
  try {
    audit_locked(actor, "deployment.prepare", "started");
  } catch (...) {
    record_ = original_record;
    throw;
  }
  const auto deadline = std::chrono::steady_clock::now() + operation_timeout;
  const auto remaining = [&] {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      throw std::runtime_error("deployment preparation timed out");
    return std::max(
        std::chrono::milliseconds(1),
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
  };
  if (verifier_) {
    record_.message = "verifying artifact trust policy";
    audit_locked("agent", "artifact.verify", "started");
    begin_operation_locked(ReconciliationOperation::Verifying);
    persist_locked();
    try {
      lock.unlock();
      const auto verification = verifier_->verify(artifact, remaining());
      lock.lock();
      if (!verification.verified)
        throw std::runtime_error("verifier returned an unverified result");
      record_.verification = verification;
      complete_operation_locked();
      persist_locked();
      audit_locked("agent", "artifact.verify", "succeeded");
    } catch (const std::exception &error) {
      if (!lock.owns_lock())
        lock.lock();
      record_failure_locked(std::string("artifact verification failed: ") +
                            error.what());
      persist_locked();
      audit_locked("agent", "artifact.verify", "failed", record_.message);
      throw;
    }
  }
  record_.message = "preparing artifact";
  audit_locked("agent", "runtime.prepare", "started");
  begin_operation_locked(ReconciliationOperation::Preparing);
  persist_locked();
  try {
    lock.unlock();
    runtime_->prepare(artifact, remaining());
    lock.lock();
  } catch (const std::exception &error) {
    if (!lock.owns_lock())
      lock.lock();
    record_failure_locked(error.what());
    persist_locked();
    audit_locked("agent", "runtime.prepare", "failed", record_.message);
    throw;
  }
  record_.phase = DeploymentPhase::Staged;
  record_.message = "artifact prepared; awaiting activation";
  record_.updated_at = utc_timestamp();
  record_.drift_detected = false;
  complete_operation_locked();
  persist_locked();
  audit_locked(actor, "deployment.prepare", "succeeded", record_.message);
  return record_;
}

DeploymentRecord
AgentDeploymentState::activate(const std::string &deployment_id,
                               std::chrono::milliseconds operation_timeout,
                               std::chrono::milliseconds readiness_timeout,
                               const std::string &actor) {
  std::unique_lock lock(mutex_);
  if (record_.deployment_id != deployment_id ||
      (record_.phase != DeploymentPhase::Staged &&
       record_.phase != DeploymentPhase::Active))
    throw std::runtime_error("deployment is not staged");
  record_.message = "activating desired workload";
  audit_locked(actor, "deployment.activate", "started");
  begin_operation_locked(ReconciliationOperation::Activating);
  persist_locked();
  const auto artifact = record_.artifact;
  const auto workload = record_.workload;
  try {
    lock.unlock();
    const auto observed = runtime_->activate(
        artifact, workload, operation_timeout, readiness_timeout);
    lock.lock();
    apply_observation(record_, observed);
    if (!matches_desired(record_, observed))
      throw std::runtime_error(
          "observed runtime is not ready or does not match desired artifact");
    record_.drift_detected = false;
    record_.phase = DeploymentPhase::Active;
    record_.message = "desired artifact is running and observed";
    record_.updated_at = utc_timestamp();
    complete_operation_locked();
    persist_locked();
    audit_locked(actor, "deployment.activate", "succeeded", record_.message);
    return record_;
  } catch (const std::exception &error) {
    if (!lock.owns_lock())
      lock.lock();
    record_failure_locked(std::string("activation failed: ") + error.what());
    persist_locked();
    audit_locked(actor, "deployment.activate", "failed", record_.message);
    throw;
  }
}

DeploymentRecord
AgentDeploymentState::rollback(const std::string &deployment_id,
                               std::chrono::milliseconds operation_timeout,
                               const std::string &actor) {
  std::unique_lock lock(mutex_);
  if (record_.deployment_id != deployment_id ||
      (record_.phase != DeploymentPhase::Staged &&
       record_.phase != DeploymentPhase::Active &&
       record_.phase != DeploymentPhase::Failed &&
       record_.phase != DeploymentPhase::RolledBack))
    throw std::runtime_error("deployment cannot be rolled back");
  if (record_.operation != ReconciliationOperation::None ||
      operation_in_progress_)
    throw std::runtime_error("another reconciliation operation is in progress");
  const auto target = record_.previous_artifact;
  const auto target_workload = record_.previous_workload;
  const auto original_record = record_;
  record_.artifact = target;
  record_.workload = target_workload;
  record_.verification = record_.previous_verification;
  record_.message = "reconciling rollback target";
  try {
    audit_locked(actor, "deployment.rollback", "started");
  } catch (...) {
    record_ = original_record;
    throw;
  }
  begin_operation_locked(ReconciliationOperation::RollingBack);
  persist_locked();
  try {
    lock.unlock();
    const auto observed =
        target.empty()
            ? runtime_->stop(operation_timeout)
            : runtime_->activate(target, target_workload, operation_timeout,
                                 operation_timeout);
    lock.lock();
    apply_observation(record_, observed);
    if (!matches_desired(record_, observed))
      throw std::runtime_error(
          "observed runtime does not match rollback target");
    record_.drift_detected = false;
    record_.phase = DeploymentPhase::RolledBack;
    record_.message = target.empty()
                          ? "managed workload stopped"
                          : "previous artifact restored and observed";
    record_.updated_at = utc_timestamp();
    complete_operation_locked();
    persist_locked();
    audit_locked(actor, "deployment.rollback", "succeeded", record_.message);
    return record_;
  } catch (const std::exception &error) {
    if (!lock.owns_lock())
      lock.lock();
    record_failure_locked(std::string("rollback failed: ") + error.what());
    persist_locked();
    audit_locked(actor, "deployment.rollback", "failed", record_.message);
    throw;
  }
}

void AgentDeploymentState::record_failure_locked(const std::string &message) {
  record_.phase = DeploymentPhase::Failed;
  record_.message = message;
  record_.updated_at = utc_timestamp();
  complete_operation_locked();
}

void AgentDeploymentState::begin_operation_locked(
    ReconciliationOperation operation) {
  if (record_.operation != ReconciliationOperation::None ||
      operation_in_progress_)
    throw std::runtime_error("another reconciliation operation is in progress");
  record_.operation = operation;
  operation_in_progress_ = true;
  record_.operation_started_at = utc_timestamp();
  ++record_.operation_attempt;
  record_.updated_at = record_.operation_started_at;
}

void AgentDeploymentState::complete_operation_locked() {
  record_.operation = ReconciliationOperation::None;
  operation_in_progress_ = false;
  record_.operation_started_at.clear();
}

void AgentDeploymentState::audit_locked(const std::string &actor,
                                        const std::string &action,
                                        const std::string &outcome,
                                        const std::string &message) const {
  if (!audit_)
    return;
  audit_->append({actor.empty() ? "unknown" : actor, action, outcome,
                  record_.deployment_id, record_.artifact,
                  deployment_phase_name(record_.phase),
                  reconciliation_operation_name(record_.operation),
                  message.empty() ? record_.message : message});
}

void AgentDeploymentState::audit_authorization_denied(
    const std::string &actor, const std::string &action,
    const std::string &message) const {
  std::lock_guard lock(mutex_);
  audit_locked(actor, "authorization." + action, "denied", message);
}

bool AgentDeploymentState::observe_locked(
    std::chrono::milliseconds operation_timeout) {
  const auto previous_artifact = record_.observed_artifact;
  const auto previous_workload_fingerprint =
      record_.observed_workload_fingerprint;
  const auto previous_runtime_id = record_.runtime_id;
  const auto previous_running = record_.runtime_running;
  const auto previous_ready = record_.runtime_ready;
  const auto previous_managed = record_.runtime_managed;
  const auto previous_readiness_status = record_.runtime_readiness_status;
  const auto previous_drift = record_.drift_detected;
  const auto previous_phase = record_.phase;
  const auto observed = runtime_->inspect(operation_timeout);
  apply_observation(record_, observed);
  if (record_.phase != DeploymentPhase::Active &&
      record_.phase != DeploymentPhase::RolledBack)
    return previous_artifact != record_.observed_artifact ||
           previous_workload_fingerprint !=
               record_.observed_workload_fingerprint ||
           previous_runtime_id != record_.runtime_id ||
           previous_running != record_.runtime_running ||
           previous_ready != record_.runtime_ready ||
           previous_managed != record_.runtime_managed ||
           previous_readiness_status != record_.runtime_readiness_status;
  const bool matches = matches_desired(record_, observed);
  record_.drift_detected = !matches;
  if (!matches)
    record_failure_locked("runtime drift detected: observed workload does not "
                          "match desired artifact");
  if (!matches && !previous_drift)
    audit_locked("agent", "runtime.drift", "detected", record_.message);
  return previous_artifact != record_.observed_artifact ||
         previous_workload_fingerprint !=
             record_.observed_workload_fingerprint ||
         previous_runtime_id != record_.runtime_id ||
         previous_running != record_.runtime_running ||
         previous_ready != record_.runtime_ready ||
         previous_managed != record_.runtime_managed ||
         previous_readiness_status != record_.runtime_readiness_status ||
         previous_drift != record_.drift_detected ||
         previous_phase != record_.phase;
}

void AgentDeploymentState::recover_interrupted_locked(
    std::chrono::milliseconds operation_timeout) {
  const auto interrupted = record_.operation;
  const auto observed = runtime_->inspect(operation_timeout);
  apply_observation(record_, observed);
  if (interrupted == ReconciliationOperation::RollingBack &&
      matches_desired(record_, observed)) {
    record_.phase = DeploymentPhase::RolledBack;
    record_.drift_detected = false;
    record_.message = "interrupted rollback completed before agent restart";
  } else if (interrupted == ReconciliationOperation::Activating &&
             matches_desired(record_, observed)) {
    record_.phase = DeploymentPhase::Staged;
    record_.drift_detected = false;
    record_.message =
        "interrupted activation observed; retry activation for ROS readiness";
  } else {
    record_.drift_detected =
        interrupted != ReconciliationOperation::Preparing &&
        interrupted != ReconciliationOperation::Verifying;
    record_.phase = DeploymentPhase::Failed;
    record_.message = std::string("interrupted ") +
                      reconciliation_operation_name(interrupted) +
                      " requires operator retry or rollback";
  }
  record_.updated_at = utc_timestamp();
  complete_operation_locked();
  audit_locked("agent", "deployment.recover", "completed", record_.message);
}

DeploymentRecord AgentDeploymentState::refresh_observed(
    std::chrono::milliseconds operation_timeout) {
  std::lock_guard lock(mutex_);
  try {
    if (record_.operation != ReconciliationOperation::None) {
      if (operation_in_progress_)
        return record_;
      recover_interrupted_locked(operation_timeout);
      persist_locked();
    } else if (observe_locked(operation_timeout)) {
      persist_locked();
    }
  } catch (const std::exception &error) {
    record_.drift_detected = true;
    record_failure_locked(std::string("runtime inspection failed: ") +
                          error.what());
    persist_locked();
    audit_locked("agent", "runtime.inspect", "failed", record_.message);
  }
  return record_;
}

DeploymentRecord
AgentDeploymentState::fail_activation(const std::string &deployment_id,
                                      const std::string &message,
                                      const std::string &actor) {
  std::lock_guard lock(mutex_);
  if (record_.deployment_id != deployment_id ||
      record_.phase != DeploymentPhase::Active ||
      record_.operation != ReconciliationOperation::None)
    throw std::runtime_error("deployment is not active");
  record_failure_locked("activation readiness failed: " + message);
  persist_locked();
  audit_locked(actor, "deployment.readiness", "failed", record_.message);
  return record_;
}

DeploymentRecord AgentDeploymentState::current() const {
  std::lock_guard lock(mutex_);
  return record_;
}

vektor::agent::v1::DeploymentRecord to_proto(const DeploymentRecord &record) {
  vektor::agent::v1::DeploymentRecord result;
  result.set_schema_version(5);
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
  result.set_runtime_ready(record.runtime_ready);
  result.set_runtime_managed(record.runtime_managed);
  result.set_runtime_readiness_status(record.runtime_readiness_status);
  result.set_drift_detected(record.drift_detected);
  *result.mutable_workload() = to_proto(record.workload);
  *result.mutable_previous_workload() = to_proto(record.previous_workload);
  result.set_workload_fingerprint(workload_fingerprint(record.workload));
  result.set_reconciliation_operation(proto_operation(record.operation));
  result.set_operation_started_at(record.operation_started_at);
  result.set_operation_attempt(record.operation_attempt);
  result.set_artifact_verified(record.verification.verified);
  result.set_verification_method(record.verification.method);
  result.set_verified_signer(record.verification.signer);
  result.set_verified_issuer(record.verification.issuer);
  result.set_verified_at(record.verification.verified_at);
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
