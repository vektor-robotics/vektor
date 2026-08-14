#pragma once

#include "vektor/agent/v1/agent.grpc.pb.h"
#include "vektor/runtime.hpp"

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
  WorkloadSpec workload;
  WorkloadSpec previous_workload;
  std::string observed_artifact;
  std::string observed_workload_fingerprint;
  std::string runtime_id;
  bool runtime_running{false};
  bool runtime_managed{false};
  bool drift_detected{false};
  DeploymentPhase phase{DeploymentPhase::Idle};
  std::string message;
  std::string updated_at;
};

const char *deployment_phase_name(DeploymentPhase phase);
bool is_valid_deployment_id(const std::string &value);
bool is_pinned_oci_artifact(const std::string &artifact);

class AgentDeploymentState {
public:
  AgentDeploymentState(std::filesystem::path state_path,
                       std::shared_ptr<RuntimeDriver> runtime);

  DeploymentRecord prepare(const std::string &deployment_id,
                           const std::string &artifact,
                           WorkloadSpec workload = {});
  DeploymentRecord activate(const std::string &deployment_id);
  DeploymentRecord rollback(const std::string &deployment_id);
  DeploymentRecord refresh_observed();
  DeploymentRecord current() const;

private:
  void load();
  void persist_locked() const;

  std::filesystem::path state_path_;
  bool observe_locked();
  void record_failure_locked(const std::string &message);

  std::shared_ptr<RuntimeDriver> runtime_;
  mutable std::mutex mutex_;
  DeploymentRecord record_;
};

vektor::agent::v1::DeploymentRecord to_proto(const DeploymentRecord &record);
vektor::agent::v1::RuntimeWorkloadSpec to_proto(const WorkloadSpec &spec);
WorkloadSpec from_proto(const vektor::agent::v1::RuntimeWorkloadSpec &spec,
                        bool use_defaults_for_unspecified = true);

} // namespace vektor
