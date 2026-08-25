#include "vektor/deployment.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
constexpr auto kArtifact =
    "ghcr.io/vektor-robotics/demo@sha256:"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr auto kArtifactB =
    "ghcr.io/vektor-robotics/demo@sha256:"
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

class FakeRuntime final : public vektor::RuntimeDriver {
public:
  void prepare(const std::string &artifact) override {
    ++calls;
    last_artifact = artifact;
    if (on_prepare)
      on_prepare();
    if (fail)
      throw std::runtime_error("simulated pull failure");
  }

  vektor::RuntimeObservation
  activate(const std::string &artifact,
           const vektor::WorkloadSpec &workload) override {
    ++activate_calls;
    last_workload = workload;
    observation = {true,     true,
                   artifact, "container-123",
                   true,     vektor::workload_fingerprint(workload),
                   "none"};
    if (on_activate)
      on_activate();
    if (fail_activate)
      throw std::runtime_error("simulated activation failure");
    return observation;
  }

  vektor::RuntimeObservation stop() override {
    ++stop_calls;
    observation = {};
    return observation;
  }

  vektor::RuntimeObservation inspect() override { return observation; }

  int calls{0};
  int activate_calls{0};
  int stop_calls{0};
  bool fail{false};
  bool fail_activate{false};
  std::function<void()> on_activate;
  std::function<void()> on_prepare;
  std::string last_artifact;
  vektor::RuntimeObservation observation;
  vektor::WorkloadSpec last_workload;
};

class FakeVerifier final : public vektor::ArtifactVerifier {
public:
  vektor::ArtifactVerification
  verify(const std::string &artifact,
         std::chrono::milliseconds timeout) override {
    ++calls;
    last_artifact = artifact;
    last_timeout = timeout;
    if (on_verify)
      on_verify();
    if (fail)
      throw std::runtime_error("signature is not trusted");
    return {true, "test_keyless", "release@example.com",
            "https://issuer.example.com", "2026-08-14T12:00:00Z"};
  }

  int calls{0};
  bool fail{false};
  std::string last_artifact;
  std::chrono::milliseconds last_timeout{0};
  std::function<void()> on_verify;
};

class RecordingAuditSink final : public vektor::AuditSink {
public:
  void append(const vektor::AuditEvent &event) override {
    events.push_back(event);
  }
  std::vector<vektor::AuditEvent> events;
};

class FailingAuditSink final : public vektor::AuditSink {
public:
  void append(const vektor::AuditEvent &) override {
    throw std::runtime_error("audit storage unavailable");
  }
};
} // namespace

TEST(Deployment, RequiresDigestPinnedOciArtifact) {
  EXPECT_TRUE(vektor::is_pinned_oci_artifact(kArtifact));
  EXPECT_FALSE(vektor::is_pinned_oci_artifact("ghcr.io/demo:latest"));
  EXPECT_FALSE(vektor::is_pinned_oci_artifact("demo@sha256:abc"));
  EXPECT_FALSE(vektor::is_pinned_oci_artifact(
      "demo@sha256:"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
  EXPECT_FALSE(vektor::is_valid_deployment_id(".."));
}

TEST(Deployment, DerivesIsolatedStableWorkloadStatePaths) {
  const auto base = std::filesystem::path(".vektor/deployment.yaml");
  EXPECT_TRUE(vektor::is_valid_workload_id("camera.front-1"));
  EXPECT_FALSE(vektor::is_valid_workload_id("../escape"));
  EXPECT_EQ(vektor::workload_state_path(base, "default"), base);
  EXPECT_EQ(vektor::workload_state_path(base, "camera.front-1"),
            ".vektor/deployment.camera.front-1.yaml");
  EXPECT_THROW(vektor::workload_state_path(base, ""), std::invalid_argument);
}

TEST(Deployment, PersistsIndependentNamedWorkloadStates) {
  const auto base = std::filesystem::path("vektor_test_workloads.yaml");
  std::filesystem::remove(base);
  std::filesystem::remove(base.string() + ".workloads");
  std::map<std::string, std::shared_ptr<FakeRuntime>> runtimes;
  const auto factory = [&runtimes](const std::string &id) {
    auto runtime = std::make_shared<FakeRuntime>();
    runtimes.emplace(id, runtime);
    return runtime;
  };
  {
    vektor::WorkloadDeploymentStates states(base, factory);
    states.for_workload("camera").prepare("camera-release", kArtifact);
    states.for_workload("navigation").prepare("nav-release", kArtifactB);
    EXPECT_EQ(states.workload_ids(),
              (std::vector<std::string>{"camera", "navigation"}));
    EXPECT_EQ(runtimes.at("camera")->last_artifact, kArtifact);
    EXPECT_EQ(runtimes.at("navigation")->last_artifact, kArtifactB);
  }
  {
    vektor::WorkloadDeploymentStates restored(base, factory);
    EXPECT_EQ(restored.for_workload("camera").current().artifact, kArtifact);
    EXPECT_EQ(restored.for_workload("navigation").current().artifact, kArtifactB);
  }
  std::filesystem::remove(base);
  std::filesystem::remove(vektor::workload_state_path(base, "camera"));
  std::filesystem::remove(vektor::workload_state_path(base, "navigation"));
  std::filesystem::remove(base.string() + ".workloads");
}

TEST(Deployment, MigratesLegacySingleWorkloadStateToDefaultRegistryEntry) {
  const auto base = std::filesystem::path("vektor_test_legacy_state.yaml");
  std::filesystem::remove(base);
  std::filesystem::remove(base.string() + ".workloads");
  auto legacy_runtime = std::make_shared<FakeRuntime>();
  {
    vektor::AgentDeploymentState legacy(base, legacy_runtime);
    ASSERT_EQ(legacy.prepare("legacy-release", kArtifact).phase,
              vektor::DeploymentPhase::Staged);
  }
  std::map<std::string, std::shared_ptr<FakeRuntime>> runtimes;
  vektor::WorkloadDeploymentStates restored(
      base, [&runtimes](const std::string &id) {
        auto runtime = std::make_shared<FakeRuntime>();
        runtimes.emplace(id, runtime);
        return runtime;
      });
  ASSERT_EQ(restored.workload_ids(), std::vector<std::string>{"default"});
  EXPECT_EQ(restored.for_workload("default").current().artifact, kArtifact);
  std::filesystem::remove(base);
  std::filesystem::remove(base.string() + ".workloads");
}

TEST(Deployment, PersistsPrepareActivateAndRollback) {
  const auto path = std::filesystem::path("vektor_test_deployment.yaml");
  std::filesystem::remove(path);
  auto backend = std::make_shared<FakeRuntime>();
  {
    vektor::AgentDeploymentState state(path, backend);
    const auto staged = state.prepare("release-1", kArtifact);
    EXPECT_EQ(staged.phase, vektor::DeploymentPhase::Staged);
    EXPECT_EQ(backend->calls, 1);
    const auto active = state.activate("release-1");
    EXPECT_EQ(active.phase, vektor::DeploymentPhase::Active);
    EXPECT_TRUE(active.runtime_running);
    EXPECT_EQ(active.observed_artifact, kArtifact);
  }
  {
    vektor::AgentDeploymentState restored(path, backend);
    EXPECT_EQ(restored.current().phase, vektor::DeploymentPhase::Active);
    const auto rolled_back = restored.rollback("release-1");
    EXPECT_EQ(rolled_back.phase, vektor::DeploymentPhase::RolledBack);
    EXPECT_TRUE(rolled_back.artifact.empty());
  }
  std::filesystem::remove(path);
}

TEST(Deployment, MigratesLegacyPersistedStateSchemas) {
  const auto path = std::filesystem::path("vektor_test_deployment_legacy.yaml");
  const auto fingerprint = vektor::workload_fingerprint(vektor::WorkloadSpec{});
  const auto workload =
      "  network: host\n  restart_policy: unless-stopped\n"
      "  environment: {}\n  mounts: []\n  devices: []\n  command: []\n";

  for (const auto schema_version : {1U, 2U, 3U, 4U}) {
    std::filesystem::remove(path);
    std::ofstream file(path);
    file << "schema_version: " << schema_version << "\n"
         << "deployment_id: release-1\nartifact: " << kArtifact << "\n"
         << "previous_artifact: ''\n";
    if (schema_version >= 2) {
      file << "observed_artifact: " << kArtifact << "\n"
           << "runtime_id: container-123\nruntime_running: true\n"
           << "drift_detected: false\n";
    }
    if (schema_version >= 3) {
      file << "workload:\n" << workload << "previous_workload:\n"
           << workload << "observed_workload_fingerprint: " << fingerprint
           << "\n";
    }
    if (schema_version >= 4) {
      file << "runtime_ready: true\nruntime_readiness_status: none\n"
           << "reconciliation_operation: none\noperation_started_at: ''\n"
           << "operation_attempt: 0\n";
    }
    file << "phase: active\nmessage: legacy state\n"
         << "updated_at: 2026-08-14T12:00:00Z\n";
    file.close();

    auto runtime = std::make_shared<FakeRuntime>();
    runtime->observation = {true, true, kArtifact, "container-123", true,
                            fingerprint, "none"};
    vektor::AgentDeploymentState state(path, runtime);
    const auto migrated = state.refresh_observed();
    EXPECT_EQ(migrated.phase, vektor::DeploymentPhase::Active);
    EXPECT_FALSE(migrated.drift_detected);
    EXPECT_EQ(migrated.operation, vektor::ReconciliationOperation::None);

    std::ifstream persisted(path);
    const std::string contents((std::istreambuf_iterator<char>(persisted)),
                               std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find("schema_version: 5"), std::string::npos);
  }
  std::filesystem::remove(path);
}

TEST(Deployment, RecordsBackendFailure) {
  const auto path =
      std::filesystem::path("vektor_test_deployment_failure.yaml");
  std::filesystem::remove(path);
  auto backend = std::make_shared<FakeRuntime>();
  backend->fail = true;
  vektor::AgentDeploymentState state(path, backend);
  EXPECT_THROW(state.prepare("release-1", kArtifact), std::runtime_error);
  EXPECT_EQ(state.current().phase, vektor::DeploymentPhase::Failed);
  EXPECT_NE(state.current().message.find("simulated pull failure"),
            std::string::npos);
  std::filesystem::remove(path);
}

TEST(Deployment, VerifiesBeforeRuntimePreparationAndPersistsProvenance) {
  const auto path =
      std::filesystem::path("vektor_test_deployment_verified.yaml");
  std::filesystem::remove(path);
  auto runtime = std::make_shared<FakeRuntime>();
  auto verifier = std::make_shared<FakeVerifier>();
  {
    vektor::AgentDeploymentState state(path, runtime, verifier);
    const auto staged = state.prepare("release-1", kArtifact);
    EXPECT_EQ(verifier->calls, 1);
    EXPECT_EQ(runtime->calls, 1);
    EXPECT_TRUE(staged.verification.verified);
    EXPECT_EQ(staged.verification.signer, "release@example.com");
    const auto response = vektor::to_proto(staged);
    EXPECT_EQ(response.schema_version(), 5U);
    EXPECT_TRUE(response.artifact_verified());
    EXPECT_EQ(response.verification_method(), "test_keyless");
    EXPECT_EQ(response.verified_issuer(), "https://issuer.example.com");
  }
  {
    vektor::AgentDeploymentState restored(path, runtime, verifier);
    EXPECT_TRUE(restored.current().verification.verified);
    EXPECT_EQ(restored.current().verification.signer, "release@example.com");
  }
  std::filesystem::remove(path);
}

TEST(Deployment, RejectsUntrustedArtifactBeforeRuntimePull) {
  const auto path =
      std::filesystem::path("vektor_test_deployment_untrusted.yaml");
  std::filesystem::remove(path);
  auto runtime = std::make_shared<FakeRuntime>();
  auto verifier = std::make_shared<FakeVerifier>();
  verifier->fail = true;
  vektor::AgentDeploymentState state(path, runtime, verifier);

  EXPECT_THROW(state.prepare("release-1", kArtifact), std::runtime_error);
  EXPECT_EQ(verifier->calls, 1);
  EXPECT_EQ(runtime->calls, 0);
  EXPECT_EQ(state.current().phase, vektor::DeploymentPhase::Failed);
  EXPECT_FALSE(state.current().verification.verified);
  EXPECT_NE(state.current().message.find("artifact verification failed"),
            std::string::npos);
  EXPECT_EQ(state.current().operation, vektor::ReconciliationOperation::None);
  std::filesystem::remove(path);
}

TEST(Deployment, AttributesOperatorAndAgentAuditEvents) {
  const auto path =
      std::filesystem::path("vektor_test_deployment_audited.yaml");
  std::filesystem::remove(path);
  auto runtime = std::make_shared<FakeRuntime>();
  auto verifier = std::make_shared<FakeVerifier>();
  auto audit = std::make_shared<RecordingAuditSink>();
  vektor::AgentDeploymentState state(path, runtime, verifier, audit);

  state.prepare("release-1", kArtifact, {}, std::chrono::minutes(5),
                "mtls:operator@example.com");
  state.activate("release-1", std::chrono::minutes(5),
                 std::chrono::seconds(30), "mtls:operator@example.com");

  ASSERT_GE(audit->events.size(), 7U);
  EXPECT_EQ(audit->events.front().actor, "mtls:operator@example.com");
  EXPECT_EQ(audit->events.front().action, "deployment.prepare");
  EXPECT_EQ(audit->events.front().outcome, "started");
  EXPECT_EQ(audit->events.front().deployment_id, "release-1");
  EXPECT_EQ(audit->events.front().artifact, kArtifact);
  EXPECT_TRUE(std::any_of(
      audit->events.begin(), audit->events.end(), [](const auto &event) {
        return event.actor == "agent" && event.action == "artifact.verify" &&
               event.outcome == "succeeded";
      }));
  EXPECT_EQ(audit->events.back().actor, "mtls:operator@example.com");
  EXPECT_EQ(audit->events.back().action, "deployment.activate");
  EXPECT_EQ(audit->events.back().outcome, "succeeded");
  std::filesystem::remove(path);
}

TEST(Deployment, AuditsRejectedArtifactAndNeverCallsRuntime) {
  const auto path =
      std::filesystem::path("vektor_test_deployment_audit_rejected.yaml");
  std::filesystem::remove(path);
  auto runtime = std::make_shared<FakeRuntime>();
  auto verifier = std::make_shared<FakeVerifier>();
  verifier->fail = true;
  auto audit = std::make_shared<RecordingAuditSink>();
  vektor::AgentDeploymentState state(path, runtime, verifier, audit);

  EXPECT_THROW(state.prepare("release-1", kArtifact, {},
                             std::chrono::minutes(5), "mtls:release-manager"),
               std::runtime_error);
  EXPECT_EQ(runtime->calls, 0);
  ASSERT_FALSE(audit->events.empty());
  const auto &failed = audit->events.back();
  EXPECT_EQ(failed.actor, "agent");
  EXPECT_EQ(failed.action, "artifact.verify");
  EXPECT_EQ(failed.outcome, "failed");
  EXPECT_NE(failed.message.find("signature is not trusted"), std::string::npos);
  std::filesystem::remove(path);
}

TEST(Deployment, BlocksRuntimeMutationWhenAuditCannotAppend) {
  const auto path =
      std::filesystem::path("vektor_test_deployment_audit_failure.yaml");
  std::filesystem::remove(path);
  auto runtime = std::make_shared<FakeRuntime>();
  auto audit = std::make_shared<FailingAuditSink>();
  vektor::AgentDeploymentState state(path, runtime, nullptr, audit);

  EXPECT_THROW(state.prepare("release-1", kArtifact), std::runtime_error);
  EXPECT_EQ(runtime->calls, 0);
  EXPECT_EQ(state.current().phase, vektor::DeploymentPhase::Idle);
  EXPECT_EQ(state.current().operation,
            vektor::ReconciliationOperation::None);
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(Deployment, ExposesPersistedVerificationProgress) {
  const auto path =
      std::filesystem::path("vektor_test_deployment_verifying.yaml");
  std::filesystem::remove(path);
  auto runtime = std::make_shared<FakeRuntime>();
  auto verifier = std::make_shared<FakeVerifier>();
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future().share();
  verifier->on_verify = [&] {
    entered.set_value();
    release_future.wait();
  };
  vektor::AgentDeploymentState state(path, runtime, verifier);

  auto pending = std::async(std::launch::async, [&] {
    return state.prepare("release-1", kArtifact);
  });
  entered_future.wait();
  const auto progress = state.refresh_observed();
  EXPECT_EQ(progress.operation, vektor::ReconciliationOperation::Verifying);
  EXPECT_EQ(progress.phase, vektor::DeploymentPhase::Idle);
  const auto response = vektor::to_proto(progress);
  EXPECT_EQ(response.reconciliation_operation(),
            vektor::agent::v1::RECONCILIATION_OPERATION_VERIFYING);
  EXPECT_FALSE(response.artifact_verified());

  release.set_value();
  const auto staged = pending.get();
  EXPECT_EQ(staged.phase, vektor::DeploymentPhase::Staged);
  EXPECT_TRUE(staged.verification.verified);
  std::filesystem::remove(path);
}

TEST(Deployment, DetectsObservedRuntimeDrift) {
  const auto path = std::filesystem::path("vektor_test_deployment_drift.yaml");
  std::filesystem::remove(path);
  auto runtime = std::make_shared<FakeRuntime>();
  vektor::AgentDeploymentState state(path, runtime);
  state.prepare("release-1", kArtifact);
  state.activate("release-1");

  runtime->observation.workload_fingerprint = "tampered";
  const auto drifted = state.refresh_observed();
  EXPECT_EQ(drifted.phase, vektor::DeploymentPhase::Failed);
  EXPECT_TRUE(drifted.drift_detected);
  EXPECT_TRUE(drifted.runtime_running);
  EXPECT_EQ(drifted.observed_artifact, drifted.artifact);
  EXPECT_NE(drifted.observed_workload_fingerprint,
            vektor::workload_fingerprint(drifted.workload));
  std::filesystem::remove(path);
}

TEST(Deployment, RecordsActivationFailureAndCanRollback) {
  const auto path =
      std::filesystem::path("vektor_test_deployment_activation.yaml");
  std::filesystem::remove(path);
  auto runtime = std::make_shared<FakeRuntime>();
  vektor::AgentDeploymentState state(path, runtime);
  state.prepare("release-1", kArtifact);
  runtime->fail_activate = true;
  EXPECT_THROW(state.activate("release-1"), std::runtime_error);
  EXPECT_EQ(state.current().phase, vektor::DeploymentPhase::Failed);
  runtime->fail_activate = false;
  const auto rolled_back = state.rollback("release-1");
  EXPECT_EQ(rolled_back.phase, vektor::DeploymentPhase::RolledBack);
  EXPECT_FALSE(rolled_back.runtime_running);
  EXPECT_EQ(runtime->stop_calls, 1);
  std::filesystem::remove(path);
}

TEST(Deployment, PersistsAndRestoresPreviousWorkloadSpec) {
  const auto path =
      std::filesystem::path("vektor_test_deployment_workload.yaml");
  std::filesystem::remove(path);
  auto runtime = std::make_shared<FakeRuntime>();
  vektor::WorkloadSpec first;
  first.environment["ROS_DOMAIN_ID"] = "42";
  first.command = {"ros2", "launch", "picker", "robot.launch.py"};
  vektor::WorkloadSpec second;
  second.network = vektor::NetworkMode::Bridge;
  second.restart_policy = "always";
  second.mounts.push_back({"/var/lib/vektor", "/data", true});

  {
    vektor::AgentDeploymentState state(path, runtime);
    state.prepare("release-1", kArtifact, first);
    state.activate("release-1");
    state.prepare("release-2", kArtifactB, second);
    state.activate("release-2");
  }
  {
    vektor::AgentDeploymentState restored(path, runtime);
    EXPECT_EQ(restored.current().workload, second);
    EXPECT_EQ(restored.current().previous_workload, first);
    const auto rolled_back = restored.rollback("release-2");
    EXPECT_EQ(rolled_back.artifact, kArtifact);
    EXPECT_EQ(rolled_back.workload, first);
    EXPECT_EQ(runtime->last_workload, first);
  }
  std::filesystem::remove(path);
}

TEST(Deployment, RecoversInterruptedActivationWithoutSilentPromotion) {
  const auto path =
      std::filesystem::path("vektor_test_deployment_interrupted.yaml");
  const auto crash_copy =
      std::filesystem::path("vektor_test_deployment_interrupted_copy.yaml");
  std::filesystem::remove(path);
  std::filesystem::remove(crash_copy);
  auto runtime = std::make_shared<FakeRuntime>();
  {
    vektor::AgentDeploymentState state(path, runtime);
    state.prepare("release-1", kArtifact);
    runtime->on_activate = [&] {
      std::filesystem::copy_file(
          path, crash_copy, std::filesystem::copy_options::overwrite_existing);
    };
    state.activate("release-1");
  }

  runtime->on_activate = nullptr;
  vektor::AgentDeploymentState restored(crash_copy, runtime);
  EXPECT_EQ(restored.current().operation,
            vektor::ReconciliationOperation::Activating);
  const auto recovered = restored.refresh_observed();
  EXPECT_EQ(recovered.phase, vektor::DeploymentPhase::Staged);
  EXPECT_EQ(recovered.operation, vektor::ReconciliationOperation::None);
  EXPECT_TRUE(recovered.runtime_ready);
  EXPECT_NE(recovered.message.find("retry activation"), std::string::npos);
  const auto active = restored.activate("release-1");
  EXPECT_EQ(active.phase, vektor::DeploymentPhase::Active);

  std::filesystem::remove(path);
  std::filesystem::remove(crash_copy);
}

TEST(Deployment, ExposesPersistedOperationProgressWhileRuntimeIsBusy) {
  const auto path =
      std::filesystem::path("vektor_test_deployment_progress.yaml");
  std::filesystem::remove(path);
  auto runtime = std::make_shared<FakeRuntime>();
  vektor::AgentDeploymentState state(path, runtime);
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future().share();
  runtime->on_prepare = [&] {
    entered.set_value();
    release_future.wait();
  };

  auto pending = std::async(std::launch::async, [&] {
    return state.prepare("release-1", kArtifact);
  });
  entered_future.wait();
  const auto progress = state.refresh_observed();
  EXPECT_EQ(progress.operation, vektor::ReconciliationOperation::Preparing);
  EXPECT_FALSE(progress.operation_started_at.empty());
  EXPECT_EQ(progress.operation_attempt, 1U);
  const auto response = vektor::to_proto(progress);
  EXPECT_EQ(response.reconciliation_operation(),
            vektor::agent::v1::RECONCILIATION_OPERATION_PREPARING);

  release.set_value();
  const auto staged = pending.get();
  EXPECT_EQ(staged.phase, vektor::DeploymentPhase::Staged);
  EXPECT_EQ(staged.operation, vektor::ReconciliationOperation::None);
  std::filesystem::remove(path);
}

TEST(Runtime, ValidatesManagedContainerName) {
  EXPECT_TRUE(vektor::is_valid_runtime_container_name("vektor-workload_1"));
  EXPECT_FALSE(vektor::is_valid_runtime_container_name("bad name"));
  EXPECT_FALSE(vektor::is_valid_runtime_container_name("-bad"));
  EXPECT_THROW(vektor::OciRuntimeDriver("docker", "bad name"),
               std::invalid_argument);
  EXPECT_EQ(vektor::workload_runtime_container_name("vektor", "default"),
            "vektor");
  EXPECT_EQ(vektor::workload_runtime_container_name("vektor", "camera"),
            "vektor-camera");
  EXPECT_THROW(vektor::workload_runtime_container_name("vektor", "../bad"),
               std::invalid_argument);
}

TEST(Runtime, ValidatesAndFingerprintsWorkloadSpec) {
  vektor::WorkloadSpec spec;
  spec.environment = {{"MODE", "production"}, {"ROS_DOMAIN_ID", "42"}};
  spec.cpu_limit = "1.5";
  spec.memory_limit = "512M";
  spec.mounts.push_back({"/var/lib/vektor", "/data", true});
  spec.devices.push_back({"/dev/video0", "/dev/video0"});
  spec.command = {"robot", "--safe"};
  EXPECT_NO_THROW(vektor::validate_workload_spec(spec));
  EXPECT_EQ(vektor::workload_fingerprint(spec),
            vektor::workload_fingerprint(spec));

  auto invalid = spec;
  invalid.environment["BAD-NAME"] = "value";
  EXPECT_THROW(vektor::validate_workload_spec(invalid), std::invalid_argument);
  invalid = spec;
  invalid.mounts.push_back({"relative", "/other", false});
  EXPECT_THROW(vektor::validate_workload_spec(invalid), std::invalid_argument);
  invalid = spec;
  invalid.cpu_limit = "0";
  EXPECT_THROW(vektor::validate_workload_spec(invalid), std::invalid_argument);
  invalid = spec;
  invalid.memory_limit = "512";
  EXPECT_THROW(vektor::validate_workload_spec(invalid), std::invalid_argument);
}

TEST(Runtime, WorkloadProtoRoundTrips) {
  vektor::WorkloadSpec expected;
  expected.network = vektor::NetworkMode::None;
  expected.restart_policy = "on-failure";
  expected.cpu_limit = "2";
  expected.memory_limit = "1G";
  expected.environment["MODE"] = "test";
  expected.mounts.push_back({"/source", "/target", true});
  expected.devices.push_back({"/dev/input0", "/dev/input0"});
  expected.command = {"run", "--once"};
  EXPECT_EQ(vektor::from_proto(vektor::to_proto(expected)), expected);
}

TEST(Runtime, OciDriverActivatesInspectsAndProtectsUnmanagedContainer) {
  const auto executable = std::filesystem::path("vektor_fake_runtime.sh");
  const auto state = std::filesystem::path(executable.string() + ".state");
  const auto managed = std::filesystem::path(executable.string() + ".managed");
  const auto workload_state =
      std::filesystem::path(executable.string() + ".workload");
  const auto arguments = std::filesystem::path(executable.string() + ".args");
  std::filesystem::remove(executable);
  std::filesystem::remove(state);
  std::filesystem::remove(managed);
  std::filesystem::remove(workload_state);
  std::filesystem::remove(arguments);
  {
    std::ofstream script(executable);
    script
        << "#!/bin/sh\n"
           "state=\"$0.state\"\n"
           "managed=\"$0.managed\"\n"
           "case \"$1\" in\n"
           "  pull) exit 0 ;;\n"
           "  inspect)\n"
           "    test -f \"$state\" || exit 1\n"
           "    printf '%s|fake-id|%s|true|%s\\n' \"$(cat \"$state\")\" "
           "\"$(cat \"$managed\")\" \"$(cat \"$0.workload\")\" ;;\n"
           "  rm) rm -f \"$state\" \"$managed\" \"$0.workload\" ;;\n"
           "  run)\n"
           "    printf '%s\\n' \"$@\" > \"$0.args\"\n"
           "    for value in \"$@\"; do\n"
           "      case \"$value\" in *@sha256:*) artifact=\"$value\" ;; esac\n"
           "      case \"$value\" in io.vektor.workload=*) "
           "printf '%s' \"${value#*=}\" > \"$0.workload\" ;; esac\n"
           "    done\n"
           "    printf '%s' \"$artifact\" > \"$state\"\n"
           "    printf 'true' > \"$managed\"\n"
           "    printf 'fake-id\\n' ;;\n"
           "  *) exit 2 ;;\n"
           "esac\n";
  }
  std::filesystem::permissions(executable,
                               std::filesystem::perms::owner_all |
                                   std::filesystem::perms::group_read |
                                   std::filesystem::perms::group_exec);

  vektor::OciRuntimeDriver driver("./" + executable.string(), "workload");
  EXPECT_NO_THROW(driver.prepare(kArtifact));
  vektor::WorkloadSpec workload;
  workload.network = vektor::NetworkMode::Bridge;
  workload.restart_policy = "always";
  workload.environment["MODE"] = "test";
  workload.mounts.push_back({"/source", "/target", true});
  workload.devices.push_back({"/dev/video0", "/dev/video0"});
  workload.command = {"robot", "--safe"};
  const auto active = driver.activate(kArtifact, workload);
  EXPECT_TRUE(active.running);
  EXPECT_TRUE(active.ready);
  EXPECT_TRUE(active.managed);
  EXPECT_EQ(active.artifact, kArtifact);
  EXPECT_EQ(driver.inspect().runtime_id, "fake-id");
  std::ifstream argument_file(arguments);
  const std::string recorded_arguments{
      std::istreambuf_iterator<char>(argument_file), {}};
  EXPECT_NE(recorded_arguments.find("bridge"), std::string::npos);
  EXPECT_NE(recorded_arguments.find("MODE=test"), std::string::npos);
  EXPECT_NE(recorded_arguments.find(
                "type=bind,source=/source,target=/target,readonly"),
            std::string::npos);
  EXPECT_NE(recorded_arguments.find("/dev/video0:/dev/video0"),
            std::string::npos);
  EXPECT_NE(recorded_arguments.find("robot\n--safe"), std::string::npos);

  {
    std::ofstream state_file(state);
    state_file << kArtifact;
    std::ofstream managed_file(managed);
    managed_file << "false";
  }
  EXPECT_THROW(driver.activate(kArtifact, {}), std::runtime_error);
  EXPECT_TRUE(std::filesystem::exists(state));

  std::filesystem::remove(executable);
  std::filesystem::remove(state);
  std::filesystem::remove(managed);
  std::filesystem::remove(workload_state);
  std::filesystem::remove(arguments);
}

TEST(Runtime, OciDriverBoundsCommands) {
  using namespace std::chrono_literals;
  const auto executable = std::filesystem::path("vektor_slow_runtime.sh");
  const auto state = std::filesystem::path(executable.string() + ".state");
  std::filesystem::remove(executable);
  std::filesystem::remove(state);
  {
    std::ofstream script(executable);
    script << "#!/bin/sh\n"
              "state=\"$0.state\"\n"
              "case \"$1\" in\n"
              "  pull) sleep 5 ;;\n"
              "  inspect)\n"
              "    test -f \"$state\" || exit 1\n"
              "    printf '%s|fake-id|true|true|%s\\n' \"$2\" "
              "'00000000000000000000000000000000' ;;\n"
              "  rm) rm -f \"$state\" ;;\n"
              "  run) touch \"$state\"; printf 'fake-id\\n' ;;\n"
              "esac\n";
  }
  std::filesystem::permissions(executable,
                               std::filesystem::perms::owner_all |
                                   std::filesystem::perms::group_read |
                                   std::filesystem::perms::group_exec);

  vektor::OciRuntimeDriver driver("./" + executable.string(), "workload");
  const auto started = std::chrono::steady_clock::now();
  EXPECT_THROW(driver.prepare(kArtifact, 50ms), std::runtime_error);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);

  std::filesystem::remove(executable);
  std::filesystem::remove(state);
}

TEST(Runtime, OciDriverBoundsCapturedFailureOutput) {
  const auto executable = std::filesystem::path("vektor_noisy_runtime.sh");
  std::filesystem::remove(executable);
  {
    std::ofstream script(executable);
    script << "#!/bin/sh\n"
              "case \"$1\" in\n"
              "  pull) dd if=/dev/zero bs=65536 count=2 2>/dev/null | "
              "tr '\\000' X; exit 1 ;;\n"
              "  *) exit 2 ;;\n"
              "esac\n";
  }
  std::filesystem::permissions(executable,
                               std::filesystem::perms::owner_all |
                                   std::filesystem::perms::group_read |
                                   std::filesystem::perms::group_exec);

  vektor::OciRuntimeDriver driver("./" + executable.string(), "workload");
  try {
    driver.prepare(kArtifact);
    FAIL() << "expected noisy runtime failure";
  } catch (const std::runtime_error &error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("output truncated"), std::string::npos);
    EXPECT_LT(message.size(), 66000U);
  }

  std::filesystem::remove(executable);
}

TEST(Runtime, OciDriverWaitsForContainerHealth) {
  using namespace std::chrono_literals;
  const auto executable = std::filesystem::path("vektor_ready_runtime.sh");
  const auto state = std::filesystem::path(executable.string() + ".state");
  const auto workload =
      std::filesystem::path(executable.string() + ".workload");
  const auto attempts =
      std::filesystem::path(executable.string() + ".attempts");
  std::filesystem::remove(executable);
  std::filesystem::remove(state);
  std::filesystem::remove(workload);
  std::filesystem::remove(attempts);
  {
    std::ofstream script(executable);
    script
        << "#!/bin/sh\n"
           "state=\"$0.state\"\n"
           "case \"$1\" in\n"
           "  pull) exit 0 ;;\n"
           "  inspect)\n"
           "    test -f \"$state\" || exit 1\n"
           "    count=0; test ! -f \"$0.attempts\" || count=$(cat "
           "\"$0.attempts\")\n"
           "    health=starting; test \"$count\" -lt 2 || health=healthy\n"
           "    case \"$3\" in\n"
           "      *State.Health.Status*) printf '%s\\n' \"$health\" ;;\n"
           "      *) count=$((count + 1)); printf '%s' \"$count\" > "
           "\"$0.attempts\"; printf '%s|fake-id|true|true|%s\\n' "
           "\"$(cat \"$state\")\" \"$(cat \"$0.workload\")\" ;;\n"
           "    esac ;;\n"
           "  rm) rm -f \"$state\" \"$0.workload\" \"$0.attempts\" ;;\n"
           "  run)\n"
           "    for value in \"$@\"; do\n"
           "      case \"$value\" in *@sha256:*) artifact=\"$value\" ;; esac\n"
           "      case \"$value\" in io.vektor.workload=*) "
           "fingerprint=\"${value#*=}\" ;; esac\n"
           "    done\n"
           "    printf '%s' \"$artifact\" > \"$state\"\n"
           "    printf '%s' \"$fingerprint\" > \"$0.workload\"\n"
           "    printf 'fake-id\\n' ;;\n"
           "esac\n";
  }
  std::filesystem::permissions(executable,
                               std::filesystem::perms::owner_all |
                                   std::filesystem::perms::group_read |
                                   std::filesystem::perms::group_exec);

  vektor::OciRuntimeDriver driver("./" + executable.string(), "workload");
  const auto observed = driver.activate(kArtifact, {}, 2s, 1s);
  EXPECT_TRUE(observed.ready);
  EXPECT_EQ(observed.readiness_status, "healthy");

  std::filesystem::remove(executable);
  std::filesystem::remove(state);
  std::filesystem::remove(workload);
  std::filesystem::remove(attempts);
}
