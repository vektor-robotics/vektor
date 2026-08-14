#pragma once

#include "vektor/agent/v1/agent.grpc.pb.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace vektor {

enum class DeploymentPhase { Idle, Staged, Active, RolledBack, Failed };

struct DeploymentRecord {
  std::string deployment_id;
  std::string artifact;
  std::string previous_artifact;
  DeploymentPhase phase{DeploymentPhase::Idle};
  std::string message;
  std::string updated_at;
};

const char *deployment_phase_name(DeploymentPhase phase);
bool is_valid_deployment_id(const std::string &value);
bool is_pinned_oci_artifact(const std::string &artifact);

class ArtifactBackend {
public:
  virtual ~ArtifactBackend() = default;
  virtual void prepare(const std::string &artifact) = 0;
};

class OciArtifactBackend final : public ArtifactBackend {
public:
  explicit OciArtifactBackend(std::string runtime);
  void prepare(const std::string &artifact) override;

private:
  std::string runtime_;
};

class AgentDeploymentState {
public:
  AgentDeploymentState(std::filesystem::path state_path,
                       std::shared_ptr<ArtifactBackend> backend);

  DeploymentRecord prepare(const std::string &deployment_id,
                           const std::string &artifact);
  DeploymentRecord activate(const std::string &deployment_id);
  DeploymentRecord rollback(const std::string &deployment_id);
  DeploymentRecord current() const;

private:
  void load();
  void persist_locked() const;

  std::filesystem::path state_path_;
  std::shared_ptr<ArtifactBackend> backend_;
  mutable std::mutex mutex_;
  DeploymentRecord record_;
};

vektor::agent::v1::DeploymentRecord to_proto(const DeploymentRecord &record);

} // namespace vektor
