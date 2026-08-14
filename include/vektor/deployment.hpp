#pragma once

#include "vektor/audit.hpp"
#include "vektor/agent/v1/agent.grpc.pb.h"
#include "vektor/runtime.hpp"
#include "vektor/trust.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace vektor {

enum class DeploymentPhase { Idle, Staged, Active, RolledBack, Failed };
enum class ReconciliationOperation {
  None,
  Verifying,
  Preparing,
  Activating,
  RollingBack
};

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
  bool runtime_ready{false};
  bool runtime_managed{false};
  std::string runtime_readiness_status;
  ArtifactVerification verification;
  ArtifactVerification previous_verification;
  bool drift_detected{false};
  DeploymentPhase phase{DeploymentPhase::Idle};
  ReconciliationOperation operation{ReconciliationOperation::None};
  std::string operation_started_at;
  std::uint64_t operation_attempt{0};
  std::string message;
  std::string updated_at;
};

const char *deployment_phase_name(DeploymentPhase phase);
const char *reconciliation_operation_name(ReconciliationOperation operation);
bool is_valid_deployment_id(const std::string &value);
bool is_pinned_oci_artifact(const std::string &artifact);

class AgentDeploymentState {
public:
  AgentDeploymentState(std::filesystem::path state_path,
                       std::shared_ptr<RuntimeDriver> runtime,
                       std::shared_ptr<ArtifactVerifier> verifier = nullptr,
                       std::shared_ptr<AuditSink> audit = nullptr);

  DeploymentRecord prepare(
      const std::string &deployment_id, const std::string &artifact,
      WorkloadSpec workload = {},
      std::chrono::milliseconds operation_timeout = std::chrono::minutes(5),
      const std::string &actor = "agent");
  DeploymentRecord activate(
      const std::string &deployment_id,
      std::chrono::milliseconds operation_timeout = std::chrono::minutes(5),
      std::chrono::milliseconds readiness_timeout = std::chrono::seconds(30),
      const std::string &actor = "agent");
  DeploymentRecord rollback(
      const std::string &deployment_id,
      std::chrono::milliseconds operation_timeout = std::chrono::minutes(5),
      const std::string &actor = "agent");
  DeploymentRecord refresh_observed(
      std::chrono::milliseconds operation_timeout = std::chrono::seconds(30));
  DeploymentRecord fail_activation(const std::string &deployment_id,
                                   const std::string &message,
                                   const std::string &actor = "agent");
  DeploymentRecord current() const;

private:
  void load();
  void persist_locked() const;

  std::filesystem::path state_path_;
  bool observe_locked(std::chrono::milliseconds operation_timeout);
  void begin_operation_locked(ReconciliationOperation operation);
  void complete_operation_locked();
  void recover_interrupted_locked(std::chrono::milliseconds operation_timeout);
  void record_failure_locked(const std::string &message);
  void audit_locked(const std::string &actor, const std::string &action,
                    const std::string &outcome,
                    const std::string &message = {}) const;

  std::shared_ptr<RuntimeDriver> runtime_;
  std::shared_ptr<ArtifactVerifier> verifier_;
  std::shared_ptr<AuditSink> audit_;
  mutable std::mutex mutex_;
  bool operation_in_progress_{false};
  DeploymentRecord record_;
};

vektor::agent::v1::DeploymentRecord to_proto(const DeploymentRecord &record);
vektor::agent::v1::RuntimeWorkloadSpec to_proto(const WorkloadSpec &spec);
WorkloadSpec from_proto(const vektor::agent::v1::RuntimeWorkloadSpec &spec,
                        bool use_defaults_for_unspecified = true);

} // namespace vektor
