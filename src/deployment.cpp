#include "vektor/deployment.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <system_error>

extern char **environ;

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

OciArtifactBackend::OciArtifactBackend(std::string runtime)
    : runtime_(std::move(runtime)) {
  if (runtime_.empty() ||
      std::any_of(runtime_.begin(), runtime_.end(),
                  [](unsigned char value) { return std::isspace(value); }))
    throw std::invalid_argument("OCI runtime must be one executable path");
}

void OciArtifactBackend::prepare(const std::string &artifact) {
  std::array<char *, 4> arguments{runtime_.data(), const_cast<char *>("pull"),
                                  const_cast<char *>(artifact.c_str()),
                                  nullptr};
  pid_t process = 0;
  const int spawn_result = posix_spawnp(&process, runtime_.c_str(), nullptr,
                                        nullptr, arguments.data(), environ);
  if (spawn_result != 0)
    throw std::runtime_error("failed to start OCI runtime '" + runtime_ +
                             "': " + std::to_string(spawn_result));
  int status = 0;
  if (waitpid(process, &status, 0) < 0 || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0)
    throw std::runtime_error("OCI runtime failed to pull '" + artifact + "'");
}

AgentDeploymentState::AgentDeploymentState(
    std::filesystem::path state_path, std::shared_ptr<ArtifactBackend> backend)
    : state_path_(std::move(state_path)), backend_(std::move(backend)) {
  if (state_path_.empty())
    throw std::invalid_argument("deployment state path cannot be empty");
  if (!backend_)
    throw std::invalid_argument("deployment artifact backend is required");
  load();
}

void AgentDeploymentState::load() {
  if (!std::filesystem::exists(state_path_))
    return;
  YAML::Node root;
  try {
    root = YAML::LoadFile(state_path_.string());
    if (!root.IsMap() || root["schema_version"].as<unsigned int>() != 1)
      throw std::runtime_error("unsupported deployment state schema");
    record_.deployment_id = root["deployment_id"].as<std::string>();
    record_.artifact = root["artifact"].as<std::string>();
    record_.previous_artifact = root["previous_artifact"].as<std::string>();
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
  output << YAML::BeginMap << YAML::Key << "schema_version" << YAML::Value << 1
         << YAML::Key << "deployment_id" << YAML::Value << record_.deployment_id
         << YAML::Key << "artifact" << YAML::Value << record_.artifact
         << YAML::Key << "previous_artifact" << YAML::Value
         << record_.previous_artifact << YAML::Key << "phase" << YAML::Value
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
                                               const std::string &artifact) {
  if (!is_valid_deployment_id(deployment_id))
    throw std::invalid_argument("invalid deployment ID");
  if (!is_pinned_oci_artifact(artifact))
    throw std::invalid_argument(
        "artifact must be an OCI reference pinned by sha256 digest");
  std::lock_guard lock(mutex_);
  if (record_.deployment_id == deployment_id && record_.artifact == artifact &&
      (record_.phase == DeploymentPhase::Staged ||
       record_.phase == DeploymentPhase::Active))
    return record_;
  if (record_.phase == DeploymentPhase::Staged)
    throw std::runtime_error("another deployment is already staged");
  const auto previous = record_.phase == DeploymentPhase::Active
                            ? record_.artifact
                            : record_.previous_artifact;
  try {
    backend_->prepare(artifact);
  } catch (const std::exception &error) {
    record_ = {deployment_id,           artifact,     previous,
               DeploymentPhase::Failed, error.what(), utc_timestamp()};
    persist_locked();
    throw;
  }
  record_ = {
      deployment_id,       artifact,       previous, DeploymentPhase::Staged,
      "artifact prepared", utc_timestamp()};
  persist_locked();
  return record_;
}

DeploymentRecord
AgentDeploymentState::activate(const std::string &deployment_id) {
  std::lock_guard lock(mutex_);
  if (record_.deployment_id == deployment_id &&
      record_.phase == DeploymentPhase::Active)
    return record_;
  if (record_.deployment_id != deployment_id ||
      record_.phase != DeploymentPhase::Staged)
    throw std::runtime_error("deployment is not staged");
  record_.phase = DeploymentPhase::Active;
  record_.message = "desired artifact activated";
  record_.updated_at = utc_timestamp();
  persist_locked();
  return record_;
}

DeploymentRecord
AgentDeploymentState::rollback(const std::string &deployment_id) {
  std::lock_guard lock(mutex_);
  if (record_.deployment_id == deployment_id &&
      record_.phase == DeploymentPhase::RolledBack)
    return record_;
  if (record_.deployment_id != deployment_id ||
      (record_.phase != DeploymentPhase::Staged &&
       record_.phase != DeploymentPhase::Active &&
       record_.phase != DeploymentPhase::Failed))
    throw std::runtime_error("deployment cannot be rolled back");
  record_.artifact = record_.previous_artifact;
  record_.phase = DeploymentPhase::RolledBack;
  record_.message = "previous desired artifact restored";
  record_.updated_at = utc_timestamp();
  persist_locked();
  return record_;
}

DeploymentRecord AgentDeploymentState::current() const {
  std::lock_guard lock(mutex_);
  return record_;
}

vektor::agent::v1::DeploymentRecord to_proto(const DeploymentRecord &record) {
  vektor::agent::v1::DeploymentRecord result;
  result.set_schema_version(1);
  result.set_deployment_id(record.deployment_id);
  result.set_artifact(record.artifact);
  result.set_previous_artifact(record.previous_artifact);
  result.set_phase(proto_phase(record.phase));
  result.set_message(record.message);
  result.set_updated_at(record.updated_at);
  return result;
}

} // namespace vektor
