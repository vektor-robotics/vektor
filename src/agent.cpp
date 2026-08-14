#include "vektor/agent.hpp"

#include "vektor/health_inspector.hpp"

#include <grpcpp/security/server_credentials.h>

#include <atomic>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace vektor {
namespace {
std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::invalid_argument("cannot read TLS file '" + path.string() + "'");
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

bool is_loopback_address(const std::string &address) {
  return address.starts_with("127.0.0.1:") ||
         address.starts_with("localhost:") || address.starts_with("[::1]:") ||
         address.starts_with("unix:");
}

std::shared_ptr<grpc::ServerCredentials>
server_credentials(const AgentOptions &options) {
  if (options.insecure)
    return grpc::InsecureServerCredentials();

  grpc::SslServerCredentialsOptions ssl;
  ssl.pem_root_certs = read_file(*options.tls_client_ca);
  ssl.pem_key_cert_pairs.push_back({read_file(*options.tls_private_key),
                                    read_file(*options.tls_certificate)});
  ssl.client_certificate_request =
      GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
  return grpc::SslServerCredentials(ssl);
}

vektor::agent::v1::HealthState proto_health_state(HealthState state) {
  switch (state) {
  case HealthState::Healthy:
    return vektor::agent::v1::HEALTH_STATE_HEALTHY;
  case HealthState::Degraded:
    return vektor::agent::v1::HEALTH_STATE_DEGRADED;
  case HealthState::Unhealthy:
    return vektor::agent::v1::HEALTH_STATE_UNHEALTHY;
  case HealthState::Unreachable:
    return vektor::agent::v1::HEALTH_STATE_UNREACHABLE;
  }
  return vektor::agent::v1::HEALTH_STATE_UNSPECIFIED;
}

vektor::agent::v1::CheckStatus proto_check_status(CheckStatus status) {
  switch (status) {
  case CheckStatus::Pass:
    return vektor::agent::v1::CHECK_STATUS_PASS;
  case CheckStatus::Warn:
    return vektor::agent::v1::CHECK_STATUS_WARN;
  case CheckStatus::Fail:
    return vektor::agent::v1::CHECK_STATUS_FAIL;
  }
  return vektor::agent::v1::CHECK_STATUS_UNSPECIFIED;
}

std::chrono::milliseconds request_timeout(std::int64_t value,
                                          std::chrono::milliseconds fallback,
                                          const char *field) {
  constexpr std::int64_t maximum = 24LL * 60 * 60 * 1000;
  if (value == 0)
    return fallback;
  if (value < 0 || value > maximum)
    throw std::invalid_argument(std::string(field) +
                                " must be between 1 ms and 24 hours");
  return std::chrono::milliseconds(value);
}

bool health_ready(HealthState state, bool allow_degraded) {
  return state == HealthState::Healthy ||
         (allow_degraded && state == HealthState::Degraded);
}

std::string audit_actor(const grpc::ServerContext &context) {
  const auto authentication = context.auth_context();
  if (authentication && authentication->IsPeerAuthenticated()) {
    const auto identities = authentication->GetPeerIdentity();
    if (!identities.empty())
      return "mtls:" +
             std::string(identities.front().data(), identities.front().size());
  }
  return "unauthenticated:" + context.peer();
}
} // namespace

void validate_agent_options(const AgentOptions &options) {
  if (options.listen_address.empty())
    throw std::invalid_argument("agent listen address cannot be empty");
  if (options.interval.count() <= 0)
    throw std::invalid_argument("agent interval must be greater than zero");
  if (options.deployment_state_path.empty())
    throw std::invalid_argument("deployment state path cannot be empty");
  if (options.audit_log_path.empty())
    throw std::invalid_argument("audit log path cannot be empty");
  if (options.oci_runtime.empty())
    throw std::invalid_argument("OCI runtime cannot be empty");
  if (!is_valid_runtime_container_name(options.runtime_container))
    throw std::invalid_argument("invalid runtime container name");
  if (options.trust_policy_path && options.trust_policy_path->empty())
    throw std::invalid_argument("trust policy path cannot be empty");
  if (options.insecure) {
    if (!is_loopback_address(options.listen_address))
      throw std::invalid_argument(
          "--insecure is limited to loopback or Unix socket listeners");
    if (options.tls_certificate || options.tls_private_key ||
        options.tls_client_ca)
      throw std::invalid_argument(
          "TLS options cannot be combined with --insecure");
    return;
  }
  if (!options.tls_certificate || !options.tls_private_key ||
      !options.tls_client_ca)
    throw std::invalid_argument(
        "agent requires --tls-cert, --tls-key, and --tls-ca unless "
        "--insecure is used on loopback");
}

void AgentStatusState::publish(StatusSnapshot snapshot) {
  {
    std::lock_guard lock(mutex_);
    latest_ = std::move(snapshot);
    ++sequence_;
  }
  changed_.notify_all();
}

std::optional<VersionedSnapshot> AgentStatusState::latest() const {
  std::lock_guard lock(mutex_);
  if (!latest_)
    return std::nullopt;
  return VersionedSnapshot{*latest_, sequence_};
}

std::optional<VersionedSnapshot>
AgentStatusState::wait_for_change(std::uint64_t after_sequence,
                                  std::chrono::milliseconds timeout) const {
  std::unique_lock lock(mutex_);
  changed_.wait_for(lock, timeout,
                    [&] { return stopped_ || sequence_ > after_sequence; });
  if (stopped_ || !latest_ || sequence_ <= after_sequence)
    return std::nullopt;
  return VersionedSnapshot{*latest_, sequence_};
}

void AgentStatusState::stop() {
  {
    std::lock_guard lock(mutex_);
    stopped_ = true;
  }
  changed_.notify_all();
}

vektor::agent::v1::StatusSnapshot to_proto(const VersionedSnapshot &versioned) {
  const auto &snapshot = versioned.snapshot;
  vektor::agent::v1::StatusSnapshot result;
  result.set_schema_version(1);
  result.set_timestamp(snapshot.timestamp);
  result.set_robot_id(snapshot.robot_id);
  result.set_hostname(snapshot.hostname);
  result.set_ros_domain_id(snapshot.ros_domain_id);
  result.set_state(proto_health_state(snapshot.state));
  result.set_duration_ms(snapshot.duration.count());
  result.set_sequence(versioned.sequence);
  for (const auto &check : snapshot.checks) {
    auto *item = result.add_checks();
    item->set_status(proto_check_status(check.status));
    item->set_category(check.category);
    item->set_target(check.target);
    item->set_message(check.message);
    item->set_duration_ms(check.duration.count());
  }
  return result;
}

GrpcAgentService::GrpcAgentService(const AgentStatusState &state)
    : state_(state) {}

GrpcAgentService::GrpcAgentService(const AgentStatusState &state,
                                   AgentDeploymentState &deployment_state)
    : state_(state), deployment_state_(&deployment_state) {}

grpc::Status
GrpcAgentService::GetStatus(grpc::ServerContext *,
                            const vektor::agent::v1::GetStatusRequest *,
                            vektor::agent::v1::StatusSnapshot *response) {
  const auto latest = state_.latest();
  if (!latest)
    return {grpc::StatusCode::UNAVAILABLE,
            "the first health inspection has not completed"};
  *response = to_proto(*latest);
  return grpc::Status::OK;
}

grpc::Status GrpcAgentService::PrepareDeployment(
    grpc::ServerContext *context,
    const vektor::agent::v1::PrepareDeploymentRequest *request,
    vektor::agent::v1::DeploymentRecord *response) {
  if (!deployment_state_)
    return {grpc::StatusCode::UNIMPLEMENTED,
            "deployment support is not configured"};
  try {
    const auto workload = request->has_workload()
                              ? from_proto(request->workload())
                              : WorkloadSpec{};
    const auto timeout =
        request_timeout(request->operation_timeout_ms(),
                        std::chrono::minutes(5), "operation_timeout_ms");
    *response = to_proto(deployment_state_->prepare(
        request->deployment_id(), request->artifact(), workload, timeout,
        audit_actor(*context)));
    return grpc::Status::OK;
  } catch (const std::invalid_argument &error) {
    return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
  } catch (const std::exception &error) {
    return {grpc::StatusCode::FAILED_PRECONDITION, error.what()};
  }
}

grpc::Status GrpcAgentService::ActivateDeployment(
    grpc::ServerContext *context,
    const vektor::agent::v1::ActivateDeploymentRequest *request,
    vektor::agent::v1::DeploymentRecord *response) {
  if (!deployment_state_)
    return {grpc::StatusCode::UNIMPLEMENTED,
            "deployment support is not configured"};
  try {
    const auto operation_timeout =
        request_timeout(request->operation_timeout_ms(),
                        std::chrono::minutes(5), "operation_timeout_ms");
    const auto readiness_timeout =
        request_timeout(request->readiness_timeout_ms(),
                        std::chrono::seconds(30), "readiness_timeout_ms");
    const auto before = state_.latest();
    const auto after_sequence = before ? before->sequence : 0;
    const auto record = deployment_state_->activate(
        request->deployment_id(), operation_timeout, readiness_timeout,
        audit_actor(*context));
    const auto fresh =
        state_.wait_for_change(after_sequence, readiness_timeout);
    if (context->IsCancelled())
      throw std::runtime_error(
          "activation cancelled while awaiting ROS health");
    if (!fresh)
      throw std::runtime_error(
          "fresh ROS health snapshot was not published before timeout");
    if (!health_ready(fresh->snapshot.state, request->allow_degraded()))
      throw std::runtime_error(
          "fresh ROS health snapshot did not satisfy rollout policy");
    *response = to_proto(record);
    response->set_message(record.message + "; fresh ROS health is " +
                          health_state_name(fresh->snapshot.state));
    return grpc::Status::OK;
  } catch (const std::invalid_argument &error) {
    return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
  } catch (const std::exception &error) {
    try {
      deployment_state_->fail_activation(request->deployment_id(),
                                         error.what(), audit_actor(*context));
    } catch (...) {
    }
    return {grpc::StatusCode::FAILED_PRECONDITION, error.what()};
  }
}

grpc::Status GrpcAgentService::RollbackDeployment(
    grpc::ServerContext *context,
    const vektor::agent::v1::RollbackDeploymentRequest *request,
    vektor::agent::v1::DeploymentRecord *response) {
  if (!deployment_state_)
    return {grpc::StatusCode::UNIMPLEMENTED,
            "deployment support is not configured"};
  try {
    const auto timeout =
        request_timeout(request->operation_timeout_ms(),
                        std::chrono::minutes(5), "operation_timeout_ms");
    *response = to_proto(
        deployment_state_->rollback(request->deployment_id(), timeout,
                                    audit_actor(*context)));
    return grpc::Status::OK;
  } catch (const std::invalid_argument &error) {
    return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
  } catch (const std::exception &error) {
    return {grpc::StatusCode::FAILED_PRECONDITION, error.what()};
  }
}

grpc::Status
GrpcAgentService::GetDeployment(grpc::ServerContext *,
                                const vektor::agent::v1::GetDeploymentRequest *,
                                vektor::agent::v1::DeploymentRecord *response) {
  if (!deployment_state_)
    return {grpc::StatusCode::UNIMPLEMENTED,
            "deployment support is not configured"};
  *response = to_proto(deployment_state_->refresh_observed());
  return grpc::Status::OK;
}

grpc::Status GrpcAgentService::WatchStatus(
    grpc::ServerContext *context, const vektor::agent::v1::WatchStatusRequest *,
    grpc::ServerWriter<vektor::agent::v1::StatusSnapshot> *writer) {
  std::uint64_t sequence = 0;
  while (!context->IsCancelled()) {
    const auto snapshot =
        state_.wait_for_change(sequence, std::chrono::milliseconds(250));
    if (!snapshot)
      continue;
    if (!writer->Write(to_proto(*snapshot)))
      break;
    sequence = snapshot->sequence;
  }
  return grpc::Status::OK;
}

AgentRunner::AgentRunner(rclcpp::Node::SharedPtr node, CheckConfig config,
                         std::string robot_id, AgentOptions options)
    : node_(std::move(node)), config_(std::move(config)),
      robot_id_(std::move(robot_id)), options_(std::move(options)) {}

int AgentRunner::run() {
  validate_agent_options(options_);
  auto runtime = std::make_shared<OciRuntimeDriver>(options_.oci_runtime,
                                                    options_.runtime_container);
  std::shared_ptr<ArtifactVerifier> verifier;
  if (options_.trust_policy_path)
    verifier = std::make_shared<CosignArtifactVerifier>(
        load_trust_policy(*options_.trust_policy_path));
  auto audit = std::make_shared<JsonLinesAuditLog>(options_.audit_log_path);
  AgentDeploymentState deployment_state(
      options_.deployment_state_path, std::move(runtime), std::move(verifier),
      std::move(audit));
  if (deployment_state.current().phase != DeploymentPhase::Idle ||
      deployment_state.current().operation != ReconciliationOperation::None) {
    const auto restored = deployment_state.refresh_observed();
    if (restored.drift_detected)
      RCLCPP_ERROR(node_->get_logger(), "%s", restored.message.c_str());
  }
  GrpcAgentService service(state_, deployment_state);
  grpc::ServerBuilder builder;
  int bound_port = 0;
  builder.AddListeningPort(options_.listen_address,
                           server_credentials(options_), &bound_port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  if (!server ||
      (bound_port == 0 && !options_.listen_address.starts_with("unix:")))
    throw std::runtime_error("failed to start agent on " +
                             options_.listen_address);

  const auto history_path =
      options_.history_path.value_or(default_status_history_path());
  std::optional<SnapshotStore> store;
  if (options_.history)
    store.emplace(history_path);

  std::atomic_bool stop{false};
  std::thread health_worker([&] {
    while (!stop.load() && rclcpp::ok()) {
      const auto started_at = std::chrono::steady_clock::now();
      StatusSnapshot snapshot;
      try {
        auto results = HealthInspector(node_).inspect(config_);
        const auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_at);
        snapshot =
            make_status_snapshot(robot_id_, std::move(results), duration);
      } catch (const std::exception &error) {
        RCLCPP_ERROR(node_->get_logger(), "health inspection failed: %s",
                     error.what());
        snapshot = make_status_snapshot(
            robot_id_, {},
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_at));
      }
      if (store) {
        try {
          store->append(snapshot);
        } catch (const std::exception &error) {
          RCLCPP_WARN(node_->get_logger(), "status history write failed: %s",
                      error.what());
        }
      }
      state_.publish(std::move(snapshot));

      if (deployment_state.current().phase != DeploymentPhase::Idle ||
          deployment_state.current().operation !=
              ReconciliationOperation::None) {
        try {
          const auto deployment = deployment_state.refresh_observed();
          if (deployment.drift_detected)
            RCLCPP_ERROR(node_->get_logger(), "%s",
                         deployment.message.c_str());
        } catch (const std::exception &error) {
          RCLCPP_ERROR(node_->get_logger(),
                       "deployment observation or audit failed: %s",
                       error.what());
        }
      }

      const auto next_run = started_at + options_.interval;
      while (!stop.load() && rclcpp::ok() &&
             std::chrono::steady_clock::now() < next_run) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                next_run - std::chrono::steady_clock::now());
        const auto slice = std::min(remaining, std::chrono::milliseconds(100));
        std::this_thread::sleep_for(slice);
      }
    }
  });

  RCLCPP_INFO(node_->get_logger(), "VEKTOR agent listening on %s",
              options_.listen_address.c_str());
  while (rclcpp::ok())
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

  stop.store(true);
  state_.stop();
  server->Shutdown();
  health_worker.join();
  return 0;
}

} // namespace vektor
